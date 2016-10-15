//
// nero // network romp
//
// One-to-one RPC over file descriptors. See `nero.h` for API.
//
#include "nero.h"
#include <unistd.h>

enum nero_frame_type
{
    NERO_FRAME_REQUEST = 0,
    NERO_FRAME_RESPONSE,
    NERO_FRAME_RESPONSE_ERROR,
};

enum nero_return_reason
{
    NERO_RETURN_OK = 0,
    NERO_RETURN_ERROR,
    NERO_RETURN_CANCEL,
};

struct nero_future
{
    uint32_t id;
    enum nero_return_reason rr;
    struct cone_event wake;
    struct romp response;
};

#define PACK(...) (uint8_t[]){__VA_ARGS__}, sizeof((uint8_t[]){__VA_ARGS__})
#define I24(x) (x) >> 16, (x) >> 8,  (x)
#define I32(x) (x) >> 24, (x) >> 16, (x) >> 8, (x)

// A coroutine that drains the write buffer and stops when it's empty. (This is slightly
// easier than notifying it of new data.)
//
// Errors:
//     `os`: see write(2).
//
static int nero_writer(struct nero *n) {
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

// Put data into the write buffer and spawn a draining coroutine if one is not yet running.
//
// Errors:
//     `memory`: could not spawn a coroutine;
//     `memory`: no space left to extend the buffer.
//
static int nero_write(struct nero *n, const uint8_t *data, size_t size) {
    if (!n->writer && !(n->writer = cone(&nero_writer, n)) MUN_RETHROW)
        return -1;
    return mun_vec_extend(&n->wbuffer, data, size) MUN_RETHROW;
}

// nero_frame ::= {type : u8} {size : u24} {rqid : u32} {data : u8}[size]
//
// Errors:
//     `nero_overflow`: frame size limit exceeded;
//     `memory`: see `nero_write`.
//
static int nero_write_header(struct nero *n, enum nero_frame_type type, uint32_t rqid, size_t size) {
    if (size > NERO_MAX_FRAME_SIZE)
        return mun_error(nero_overflow, "frame too big");
    return nero_write(n, PACK(type, I24(size), I32(rqid)));
}

// data | type=NERO_FRAME_REQUEST ::= {name : i8}[] '\0' {args : u8}[]
//
// Errors: see `nero_write_header`.
//
static int nero_write_request(struct nero *n, uint32_t rqid, const char *function, struct romp *args) {
    uint32_t nlen = strlen(function) + 1;
    if (nero_write_header(n, NERO_FRAME_REQUEST, rqid, nlen + args->size) MUN_RETHROW) return -1;
    if (nero_write(n, (const uint8_t*)function, nlen) MUN_RETHROW) return -1;
    return nero_write(n, args->data, args->size) MUN_RETHROW;
}

// data | type=NERO_FRAME_RESPONSE ::= {args : u8}[]
//
// Errors: see `nero_write_header`.
//
static int nero_write_response(struct nero *n, uint32_t rqid, struct romp *ret) {
    if (nero_write_header(n, NERO_FRAME_RESPONSE, rqid, ret->size) MUN_RETHROW) return -1;
    return nero_write(n, ret->data, ret->size) MUN_RETHROW;
}

// data | type=NERO_FRAME_RESPONSE_ERROR ::= {code : u32} {name : i8}[] '\0' {text : i8}[] '\0'
//
// Errors: see `nero_write_header`.
//
static int nero_write_response_error(struct nero *n, uint32_t rqid) {
    const struct mun_error *err = mun_last_error();
    uint32_t nlen = strlen(err->name) + 1;
    uint32_t tlen = strlen(err->text) + 1;
    uint8_t buf[nlen + tlen + 4];
    memcpy(buf, (uint8_t[]){I32(err->code)}, 4);
    memcpy(buf + 4, err->name, nlen);
    memcpy(buf + 4 + nlen, err->text, tlen);
    if (nero_write_header(n, NERO_FRAME_RESPONSE_ERROR, rqid, nlen + tlen + 4) MUN_RETHROW) return -1;
    return nero_write(n, buf, nlen + tlen + 4) MUN_RETHROW;
}

// Parse data from a NERO_FRAME_RESPONSE_ERROR into a `mun_error`.
//
// Errors:
//     `nero_remote` (variable errno): on success;
//     `nero_protocol`: invalid frame format.
//
static int nero_restore_error(const uint8_t *data, size_t size, const char *function) {
    if (size < 6 || data[size - 1] != 0)
        return mun_error(nero_protocol, "truncated error response");
    uint32_t code = romp_r4(data);
    const char *name = (const char *)data + 4;
    const char *text = memchr(name, 0, size - 4) + 1;
    if (text == &name[size - 4])
        return mun_error(nero_protocol, "error response only has one string");
    return mun_error_at(code, "nero_remote", MUN_CURRENT_FRAME, "%s / %s: %s", function, name, text);
}

// Handle an inbound frame.
//
// Errors:
//     `memory`: could not write response to buffer or wake a coroutine waiting for one;
//     `nero_protocol`: invalid frame format or unknown type;
//     `nero_overflow`: the local implementation called by a peer generated too much output.
//
static int nero_on_frame(struct nero *n, enum nero_frame_type type, uint32_t rqid, const uint8_t *data, size_t size) {
    if (type == NERO_FRAME_REQUEST) {
        uint8_t *sep = memchr(data, 0, size);
        if (sep == NULL)
            return mun_error(nero_protocol, "malformed request");
        const char *function = (const char *)data;
        struct romp in = mun_vec_init_borrow(sep + 1, size - (sep - data) - 1);
        struct romp out = {};
        unsigned i = mun_vec_find(&n->exported, !strcmp(function, _->name));
        if (i == n->exported.size)
            return mun_error(nero_not_exported, "%s", function), nero_write_response_error(n, rqid);
        if (n->exported.data[i].code(n, n->exported.data[i].data, &in, &out))
            return mun_vec_fini(&out), nero_write_response_error(n, rqid);
        if (nero_write_response(n, rqid, &out) MUN_RETHROW)
            return mun_vec_fini(&out), -1;
        return mun_vec_fini(&out), 0;
    }
    if (type == NERO_FRAME_RESPONSE || type == NERO_FRAME_RESPONSE_ERROR) {
        unsigned i = mun_vec_find(&n->queued, rqid == (*_)->id);
        if (i != n->queued.size) {
            struct nero_future *fut = n->queued.data[i];
            mun_vec_erase(&n->queued, i, 1);
            fut->rr = type == NERO_FRAME_RESPONSE_ERROR ? NERO_RETURN_ERROR : NERO_RETURN_OK;
            if (mun_vec_extend(&fut->response, data, size) || cone_event_emit(&fut->wake) MUN_RETHROW)
                return -1;
        }
        return 0;
    }
    return mun_error(nero_protocol, "unknown frame type %u", type);
}

// Call a remote function and wait for a response.
//
// Errors:
//     any: returned by peer;
//     any romp error: could not serialize the arguments, see `romp_encode`;
//     any romp error: could not deserialize the return value, see `romp_decode`;
//     `memory`: no space in write buffer;
//     `memory`: could not pause the coroutine;
//     `cancelled`: the channel was finalized before a response arrived.
//
static int nero_call_wait(struct nero *n, struct nero_future *fut, const char *function, va_list args) {
    struct romp enc = {};
    if (romp_encode_var(&enc, va_arg(args, const char *), args) MUN_RETHROW)
        return mun_vec_fini(&enc), -1;
    if (nero_write_request(n, fut->id, function, &enc) MUN_RETHROW)
        return mun_vec_fini(&enc), -1;
    mun_vec_fini(&enc);
    if (cone_wait(&fut->wake) MUN_RETHROW)
        return -1;
    if (fut->rr == NERO_RETURN_OK)
        return romp_decode_var(&fut->response, va_arg(args, const char *), args) MUN_RETHROW;
    if (fut->rr == NERO_RETURN_ERROR)
        return nero_restore_error(fut->response.data, fut->response.size, function) MUN_RETHROW;
    if (fut->rr == NERO_RETURN_CANCEL)
        return cone_cancel(cone);
    return mun_error(assert, "invalid nero_return_reason");
}

void nero_fini(struct nero *n) {
    for (unsigned i = 0; i < n->queued.size; i++)
        if (cone_event_emit(&n->queued.data[i]->wake))
            mun_error_show("could not wake coroutine due to", NULL);
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

int nero_run(struct nero *n) {
    uint8_t data[4096];
    for (ssize_t size; (size = read(n->fd, data, sizeof(data))); ) {
        if (size < 0)
            return errno != ECANCELED MUN_RETHROW_OS;
        if (mun_vec_extend(&n->rbuffer, data, size) MUN_RETHROW)
            return -1;
        while (n->rbuffer.size >= 8) {
            uint32_t size = romp_r4(n->rbuffer.data) & 0xFFFFFFl;
            if (size > NERO_MAX_FRAME_SIZE)
                return mun_error(nero_protocol, "received frame too big");
            if (n->rbuffer.size < size + 8)
                break;
            if (nero_on_frame(n, romp_r1(n->rbuffer.data), romp_r4(n->rbuffer.data + 4), n->rbuffer.data + 8, size) MUN_RETHROW)
                return -1;
            mun_vec_erase(&n->rbuffer, 0, size + 8);
        }
    }
    return 0;
}

int nero_call_var(struct nero *n, const char *function, va_list args) {
    struct nero_future fut = {.id = ++n->last_id, .rr = NERO_RETURN_CANCEL};
    struct nero_future *fp = &fut;
    if (mun_vec_append(&n->queued, &fp) MUN_RETHROW)
        return -1;
    if (nero_call_wait(n, &fut, function, args) MUN_RETHROW) {
        mun_vec_fini(&fut.response);
        unsigned i = mun_vec_find(&n->queued, *_ == &fut);
        if (i != n->queued.size)
            mun_vec_erase(&n->queued, i, 1);
        return -1;
    }
    mun_vec_fini(&fut.response);
    return 0;
}

int nero_call(struct nero *n, const char *function, ...) {
    va_list args;
    va_start(args, function);
    int ret = nero_call_var(n, function, args);
    va_end(args);
    return ret;
}
