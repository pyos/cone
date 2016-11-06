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
static inline uint16_t R16(const uint8_t *p) { return (uint16_t)p[0] << 8 | p[1]; }
static inline uint32_t R32(const uint8_t *p) { return (uint32_t)R16(p) << 16 | R16(p + 2); }

static int mae_writer(struct mae *m) {
    ssize_t size = 0;
    while ((size = m->wbuffer.size > 1024 ? 1024 : m->wbuffer.size)) {
        // If `write` yields, some other coroutine may cause `wbuffer` to be reallocated.
        char buf[size]; memcpy(buf, m->wbuffer.data, size);
        if ((size = write(m->fd, buf, size)) < 0 MUN_RETHROW_OS)
            break;
        mun_vec_erase(&m->wbuffer, 0, size);
    }
    cone_drop(m->writer);
    m->writer = NULL;
    return size < 0 ? -1 : 0;
}

static int mae_write(struct mae *m, const uint8_t *data, size_t size) mun_throws(memory) {
    if (!m->writer && !(m->writer = cone(&mae_writer, m)) MUN_RETHROW)
        return -1;
    return mun_vec_extend(&m->wbuffer, data, size) MUN_RETHROW;
}

// mae_frame ::= {type : u8} {size : u24} {id : u32} {data : u8}[size]
static int mae_write_header(struct mae *m, enum mae_frame_type t, uint32_t id, size_t size) mun_throws(memory, mae_overflow) {
    if (size > MAE_MAX_FRAME_SIZE)
        return mun_error(mae_overflow, "frame too big");
    return mae_write(m, PACK(t, I24(size), I32(id)));
}

// data | type=MAE_FRAME_REQUEST ::= {function : i8}[] '\0' {args : u8}[]
static int mae_write_request(struct mae *m, uint32_t id, const char *fn, struct siy *args) mun_throws(memory, mae_overflow) {
    uint32_t name_len = strlen(fn) + 1;
    if (mae_write_header(m, MAE_FRAME_REQUEST, id, name_len + args->size) MUN_RETHROW)
        return -1;
    if (mae_write(m, (const uint8_t*)fn, name_len) MUN_RETHROW)
        return -1;
    return mae_write(m, args->data, args->size) MUN_RETHROW;
}

// data | type=MAE_FRAME_RESPONSE ::= {values : u8}[]
static int mae_write_response(struct mae *m, uint32_t id, struct siy *values) mun_throws(memory, mae_overflow) {
    if (mae_write_header(m, MAE_FRAME_RESPONSE, id, values->size) MUN_RETHROW)
        return -1;
    return mae_write(m, values->data, values->size) MUN_RETHROW;
}

// data | type=MAE_FRAME_RESPONSE_ERROR ::= {code : u32} {name : i8}[] '\0' {text : i8}[] '\0'
static int mae_write_response_error(struct mae *m, uint32_t id) mun_throws(memory, mae_overflow) {
    const struct mun_error *err = mun_last_error();
    uint32_t nlen = strlen(err->name) + 1;
    uint32_t tlen = strlen(err->text) + 1;
    uint8_t buf[nlen + tlen + 4];
    memcpy(buf, (uint8_t[]){I32(err->code)}, 4);
    memcpy(buf + 4, err->name, nlen);
    memcpy(buf + 4 + nlen, err->text, tlen);
    if (mae_write_header(m, MAE_FRAME_RESPONSE_ERROR, id, nlen + tlen + 4) MUN_RETHROW)
        return -1;
    return mae_write(m, buf, nlen + tlen + 4) MUN_RETHROW;
}

static int mae_restore_error(const uint8_t *data, size_t size, const char *fn) mun_throws(mae_remote, mae_protocol) {
    if (size < 6 || data[size - 1] != 0)
        return mun_error(mae_protocol, "truncated error response");
    uint32_t code = R32(data);
    const char *name = (const char *)data + 4;
    const char *text = (const char *)memchr(name, 0, size - 4) + 1;
    if (text == &name[size - 4])
        return mun_error(mae_protocol, "error response only has one string");
    return mun_error_at(code, "mae_remote", MUN_CURRENT_FRAME, "%s / %s: %s", fn, name, text);
}

// Handle an inbound frame.
//
// Errors:
//   * `memory`;
//   * `mae_protocol`: invalid frame format or unknown type;
//   * `mae_overflow`: the local implementation called by a peer generated too much output.
//
static int mae_on_frame(struct mae *m, enum mae_frame_type t, uint32_t id, const uint8_t *data, size_t size) {
    if (t == MAE_FRAME_REQUEST) {
        uint8_t *sep = memchr(data, 0, size);
        if (sep == NULL)
            return mun_error(mae_protocol, "malformed request");
        const char *function = (const char *)data;
        size_t i = mun_vec_find(&m->exported, !strcmp(function, _->name));
        if (i == m->exported.size)
            return mun_error(mae_not_exported, "%s", function), mae_write_response_error(m, id);
        struct mae_closure *c = &m->exported.data[i];
        _Alignas(max_align_t) char argbuf[siy_signinfo(c->isign).size];
        _Alignas(max_align_t) char retbuf[siy_signinfo(c->osign).size];
        if (siy_decode(&(struct siy)mun_vec_init_borrow(sep + 1, size - (sep - data) - 1), c->isign, argbuf))
            return mae_write_response_error(m, id);
        if (c->code(m, c->data, argbuf, retbuf))
            return mae_write_response_error(m, id);
        struct siy out = {};
        if (siy_encode(&out, c->osign, retbuf))
            return mun_vec_fini(&out), mae_write_response_error(m, id);
        if (mae_write_response(m, id, &out) MUN_RETHROW)
            return mun_vec_fini(&out), -1;
        return mun_vec_fini(&out), 0;
    }
    if (t == MAE_FRAME_RESPONSE || t == MAE_FRAME_RESPONSE_ERROR) {
        size_t i = mun_vec_find(&m->queued, id == (*_)->id);
        if (i != m->queued.size) {
            struct mae_future *fut = m->queued.data[i];
            mun_vec_erase(&m->queued, i, 1);
            fut->rr = t == MAE_FRAME_RESPONSE_ERROR ? MAE_RETURN_ERROR : MAE_RETURN_OK;
            if (mun_vec_extend(&fut->response, data, size) || cone_wake(&fut->wake, 1) MUN_RETHROW)
                return -1;
        }
        return 0;
    }
    return mun_error(mae_protocol, "unknown frame type %u", t);
}

void mae_fini(struct mae *m) {
    for mun_vec_iter(&m->queued, it) {
        (*it)->rr = MAE_RETURN_CANCEL;
        if (cone_wake(&(*it)->wake, 1))
            mun_error_show("could not wake coroutine due to", NULL);
    }
    mun_vec_fini(&m->rbuffer);
    mun_vec_fini(&m->wbuffer);
    mun_vec_fini(&m->queued);
    mun_vec_fini(&m->exported);
    if (m->writer) {
        cone_cancel(m->writer);
        cone_drop(m->writer);
    }
    close(m->fd);
}

int mae_run(struct mae *m) {
    uint8_t data[4096];
    for (ssize_t size; (size = read(m->fd, data, sizeof(data))); ) {
        if (size < 0)
            return errno != ECANCELED MUN_RETHROW_OS;
        if (mun_vec_extend(&m->rbuffer, data, size) MUN_RETHROW)
            return -1;
        while (m->rbuffer.size >= 8) {
            uint32_t size = R32(m->rbuffer.data) & 0xFFFFFFl;
            if (size > MAE_MAX_FRAME_SIZE)
                return mun_error(mae_protocol, "received frame too big");
            if (m->rbuffer.size < size + 8)
                break;
            if (mae_on_frame(m, m->rbuffer.data[0], R32(m->rbuffer.data + 4), m->rbuffer.data + 8, size) MUN_RETHROW)
                return -1;
            mun_vec_erase(&m->rbuffer, 0, size + 8);
        }
    }
    return 0;
}

int mae_call(struct mae *m, const char *f, const char *isign, const void *i, const char *osign, void *o) {
    struct siy enc = {};
    if (siy_encode(&enc, isign, i) MUN_RETHROW)
        return mun_vec_fini(&enc), -1;
    struct mae_future fut = {.id = ++m->last_id, .rr = ATOMIC_VAR_INIT((unsigned)MAE_RETURN_UNSET)};
    if (mun_vec_append(&m->queued, &((struct mae_future *){&fut})) MUN_RETHROW)
        return -1;
    if (mae_write_request(m, fut.id, f, &enc) MUN_RETHROW)
        goto fail;
    mun_vec_fini(&enc);
    if (cone_wait(&fut.wake, &fut.rr, MAE_RETURN_UNSET) < 0 MUN_RETHROW)
        goto fail;
    enum mae_return_reason rr = fut.rr;
    int fail = rr == MAE_RETURN_OK    ? siy_decode(&fut.response, osign, o) MUN_RETHROW
             : rr == MAE_RETURN_ERROR ? mae_restore_error(fut.response.data, fut.response.size, f) MUN_RETHROW
             : mun_error(cancelled, "connection closed");
    mun_vec_fini(&fut.response);
    return fail;
fail:
    mun_vec_fini(&enc);
    size_t index = mun_vec_find(&m->queued, *_ == &fut);
    if (index != m->queued.size)
        mun_vec_erase(&m->queued, index, 1);
    mun_vec_fini(&fut.response);
    return -1;
}
