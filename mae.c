#include "mae.h"
#include <unistd.h>
#include <stdatomic.h>

#ifndef MAE_MAX_FRAME_SIZE
#define MAE_MAX_FRAME_SIZE 65535
#endif

enum mae_frame_type
{
    MAE_FRAME_REQUEST = 0,
    MAE_FRAME_RESPONSE,
    MAE_FRAME_RESPONSE_ERROR,
};

enum mae_return_reason
{
    MAE_RETURN_OK = 0,
    MAE_RETURN_ERROR,
    MAE_RETURN_CANCEL,
    MAE_RETURN_UNSET,
};

struct mae_future
{
    uint32_t id;
    cone_atom rr;
    struct cone_event wake;
    struct siy response;
};

#define PACK(...) (uint8_t[]){__VA_ARGS__}, sizeof((uint8_t[]){__VA_ARGS__})
#define I24(x) (x) >> 16, (x) >> 8,  (x)
#define I32(x) (x) >> 24, (x) >> 16, (x) >> 8, (x)

static int mae_writer(struct mae *n) {
    ssize_t size = 0;
    while ((size = n->wbuffer.size > 1024 ? 1024 : n->wbuffer.size)) {
        // If `write` yields, some other coroutine may cause `wbuffer` to be reallocated.
        char buf[size]; memcpy(buf, n->wbuffer.data, size);
        if ((size = write(n->fd, buf, size)) < 0 MUN_RETHROW_OS)
            break;
        mun_vec_erase(&n->wbuffer, 0, size);
    }
    cone_decref(n->writer);
    n->writer = NULL;
    return size < 0 ? -1 : 0;
}

static int mae_write(struct mae *n, const uint8_t *data, size_t size) mun_throws(memory) {
    if (!n->writer && !(n->writer = cone(&mae_writer, n)) MUN_RETHROW)
        return -1;
    return mun_vec_extend(&n->wbuffer, data, size) MUN_RETHROW;
}

// mae_frame ::= {type : u8} {size : u24} {rqid : u32} {data : u8}[size]
static int mae_write_header(struct mae *n, enum mae_frame_type type, uint32_t rqid, size_t size) mun_throws(memory, mae_overflow) {
    if (size > MAE_MAX_FRAME_SIZE)
        return mun_error(mae_overflow, "frame too big");
    return mae_write(n, PACK(type, I24(size), I32(rqid)));
}

// data | type=MAE_FRAME_REQUEST ::= {name : i8}[] '\0' {args : u8}[]
static int mae_write_request(struct mae *n, uint32_t rqid, const char *function, struct siy *args) mun_throws(memory, mae_overflow) {
    uint32_t nlen = strlen(function) + 1;
    if (mae_write_header(n, MAE_FRAME_REQUEST, rqid, nlen + args->size) MUN_RETHROW) return -1;
    if (mae_write(n, (const uint8_t*)function, nlen) MUN_RETHROW) return -1;
    return mae_write(n, args->data, args->size) MUN_RETHROW;
}

// data | type=MAE_FRAME_RESPONSE ::= {args : u8}[]
static int mae_write_response(struct mae *n, uint32_t rqid, struct siy *ret) mun_throws(memory, mae_overflow) {
    if (mae_write_header(n, MAE_FRAME_RESPONSE, rqid, ret->size) MUN_RETHROW) return -1;
    return mae_write(n, ret->data, ret->size) MUN_RETHROW;
}

// data | type=MAE_FRAME_RESPONSE_ERROR ::= {code : u32} {name : i8}[] '\0' {text : i8}[] '\0'
static int mae_write_response_error(struct mae *n, uint32_t rqid) mun_throws(memory, mae_overflow) {
    const struct mun_error *err = mun_last_error();
    uint32_t nlen = strlen(err->name) + 1;
    uint32_t tlen = strlen(err->text) + 1;
    uint8_t buf[nlen + tlen + 4];
    memcpy(buf, (uint8_t[]){I32(err->code)}, 4);
    memcpy(buf + 4, err->name, nlen);
    memcpy(buf + 4 + nlen, err->text, tlen);
    if (mae_write_header(n, MAE_FRAME_RESPONSE_ERROR, rqid, nlen + tlen + 4) MUN_RETHROW) return -1;
    return mae_write(n, buf, nlen + tlen + 4) MUN_RETHROW;
}

// Parse data from a MAE_FRAME_RESPONSE_ERROR into a `mun_error`.
static int mae_restore_error(const uint8_t *data, size_t size, const char *function) mun_throws(mae_remote, mae_protocol) {
    if (size < 6 || data[size - 1] != 0)
        return mun_error(mae_protocol, "truncated error response");
    uint32_t code = siy_r4(data);
    const char *name = (const char *)data + 4;
    const char *text = (const char *)memchr(name, 0, size - 4) + 1;
    if (text == &name[size - 4])
        return mun_error(mae_protocol, "error response only has one string");
    return mun_error_at(code, "mae_remote", MUN_CURRENT_FRAME, "%s / %s: %s", function, name, text);
}

// Handle an inbound frame.
//
// Errors:
//   * `memory`;
//   * `mae_protocol`: invalid frame format or unknown type;
//   * `mae_overflow`: the local implementation called by a peer generated too much output.
//
static int mae_on_frame(struct mae *n, enum mae_frame_type type, uint32_t rqid, const uint8_t *data, size_t size) {
    if (type == MAE_FRAME_REQUEST) {
        uint8_t *sep = memchr(data, 0, size);
        if (sep == NULL)
            return mun_error(mae_protocol, "malformed request");
        const char *function = (const char *)data;
        size_t i = mun_vec_find(&n->exported, !strcmp(function, _->name));
        if (i == n->exported.size)
            return mun_error(mae_not_exported, "%s", function), mae_write_response_error(n, rqid);
        struct mae_closure *c = &n->exported.data[i];
        struct siy in = mun_vec_init_borrow(sep + 1, size - (sep - data) - 1);
        struct siy out = {};
        struct siy_signinfo isi = siy_signinfo(c->isign);
        struct siy_signinfo osi = siy_signinfo(c->osign);
        _Alignas(max_align_t) char ibuf[isi.size], obuf[osi.size];
        if (siy_decode(&in, c->isign, ibuf))
            return mae_write_response_error(n, rqid);
        if (c->code(n, c->data, ibuf, obuf))
            return mae_write_response_error(n, rqid);
        if (siy_encode(&out, c->osign, obuf))
            return mun_vec_fini(&out), mae_write_response_error(n, rqid);
        if (mae_write_response(n, rqid, &out) MUN_RETHROW)
            return mun_vec_fini(&out), -1;
        return mun_vec_fini(&out), 0;
    }
    if (type == MAE_FRAME_RESPONSE || type == MAE_FRAME_RESPONSE_ERROR) {
        size_t i = mun_vec_find(&n->queued, rqid == (*_)->id);
        if (i != n->queued.size) {
            struct mae_future *fut = n->queued.data[i];
            mun_vec_erase(&n->queued, i, 1);
            atomic_store(&fut->rr, type == MAE_FRAME_RESPONSE_ERROR ? MAE_RETURN_ERROR : MAE_RETURN_OK);
            if (mun_vec_extend(&fut->response, data, size) || cone_wake(&fut->wake, 1) MUN_RETHROW)
                return -1;
        }
        return 0;
    }
    return mun_error(mae_protocol, "unknown frame type %u", type);
}

// Call a remote function and wait on a specified future.
//
// Errors:
//   * `memory`;
//   * `siy_truncated`: could not decode a response;
//   * `siy_sign_syntax`: one of the signatures is invalid.
//
static int mae_call_wait(struct mae *n, struct mae_future *fut, const char *f,
                         const char *isign, const void *i, const char *osign, void *o) {
    struct siy enc = {};
    if (siy_encode(&enc, isign, i) MUN_RETHROW)
        return mun_vec_fini(&enc), -1;
    if (mae_write_request(n, fut->id, f, &enc) MUN_RETHROW)
        return mun_vec_fini(&enc), -1;
    mun_vec_fini(&enc);
    atomic_store(&fut->rr, MAE_RETURN_UNSET);
    if (cone_wait(&fut->wake, &fut->rr, MAE_RETURN_UNSET) < 0 MUN_RETHROW)
        return -1;
    unsigned rr = atomic_load(&fut->rr);
    if (rr == MAE_RETURN_OK)
        return siy_decode(&fut->response, osign, o) MUN_RETHROW;
    if (rr == MAE_RETURN_ERROR)
        return mae_restore_error(fut->response.data, fut->response.size, f) MUN_RETHROW;
    if (rr == MAE_RETURN_CANCEL)
        return mun_error(cancelled, "connection closed");
    return mun_error(assert, "invalid mae_return_reason");
}

void mae_fini(struct mae *n) {
    for mun_vec_iter(&n->queued, it) {
        atomic_store(&(*it)->rr, MAE_RETURN_CANCEL);
        if (cone_wake(&(*it)->wake, 1))
            mun_error_show("could not wake coroutine due to", NULL);
    }
    mun_vec_fini(&n->rbuffer);
    mun_vec_fini(&n->wbuffer);
    mun_vec_fini(&n->queued);
    mun_vec_fini(&n->exported);
    if (n->writer) {
        cone_cancel(n->writer);
        cone_decref(n->writer);
    }
    close(n->fd);
}

int mae_run(struct mae *n) {
    uint8_t data[4096];
    for (ssize_t size; (size = read(n->fd, data, sizeof(data))); ) {
        if (size < 0)
            return errno != ECANCELED MUN_RETHROW_OS;
        if (mun_vec_extend(&n->rbuffer, data, size) MUN_RETHROW)
            return -1;
        while (n->rbuffer.size >= 8) {
            uint32_t size = siy_r4(n->rbuffer.data) & 0xFFFFFFl;
            if (size > MAE_MAX_FRAME_SIZE)
                return mun_error(mae_protocol, "received frame too big");
            if (n->rbuffer.size < size + 8)
                break;
            if (mae_on_frame(n, siy_r1(n->rbuffer.data), siy_r4(n->rbuffer.data + 4), n->rbuffer.data + 8, size) MUN_RETHROW)
                return -1;
            mun_vec_erase(&n->rbuffer, 0, size + 8);
        }
    }
    return 0;
}

int mae_call(struct mae *n, const char *f, const char *isign, const void *i, const char *osign, void *o) {
    struct mae_future fut = {.id = ++n->last_id, .rr = ATOMIC_VAR_INIT((unsigned)MAE_RETURN_UNSET)};
    struct mae_future *fp = &fut;
    if (mun_vec_append(&n->queued, &fp) MUN_RETHROW)
        return -1;
    if (mae_call_wait(n, &fut, f, isign, i, osign, o) MUN_RETHROW) {
        mun_vec_fini(&fut.response);
        size_t i = mun_vec_find(&n->queued, *_ == &fut);
        if (i != n->queued.size)
            mun_vec_erase(&n->queued, i, 1);
        return -1;
    }
    mun_vec_fini(&fut.response);
    return 0;
}
