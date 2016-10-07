#include "nero.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct nero_future
{
    unsigned id, *errmarker;
    struct cone_event *ev;
    struct romp_iovec *data;
};

static int nero_writer(struct nero *n) {
    char local[4096];
    while (n->wrbuffer.size) {
        size_t size = n->wrbuffer.size > sizeof(local) ? sizeof(local) : n->wrbuffer.size;
        size_t remaining = size;
        const char *data = memcpy(local, n->wrbuffer.data, remaining);
        for (ssize_t wr; remaining; data += wr, remaining -= wr) {
            if ((wr = write(n->fd, data, remaining)) < 0)
                return mun_error_os();
        }
        mun_vec_erase(&n->wrbuffer, 0, size);
    }
    cone_decref(n->writer);
    n->writer = NULL;
    return mun_ok;
}

static int nero_on_write(struct nero *n, const uint8_t *data, size_t size) {
    if (!n->writer)
        if (!(n->writer = cone(&nero_writer, n)))
            return mun_error_up();
    if (mun_vec_extend(&n->wrbuffer, data, size))
        return mun_error_up();
    return mun_ok;
}

enum
{
    NERO_FRAME_REQUEST,
    NERO_FRAME_RESPONSE,
    NERO_FRAME_ERROR,
};

// nero_frame ::= {size : uint32_t} {rqid : uint32_t} {type : uint8_t} {data(type) : uint8_t}[size]
static int nero_write_header(struct nero *n, uint8_t type, uint32_t rqid, size_t size) {
    if (size > NERO_MAX_FRAME_SIZE)
        return mun_error(nero_overflow, "frame too big");
    uint8_t header[9] = { size>>24, size>>16, size>>8, size, rqid>>24, rqid>>16, rqid>>8, rqid, type};
    return nero_on_write(n, header, 9);
}

// data(NERO_FRAME_REQUEST) ::= {name_len : uint32_t} {name : uint8_t}[name_len] '\0' {args : uint8_t}[]
static int nero_write_request(struct nero *n, uint32_t rqid, const char *function, struct romp_iovec *args) {
    uint32_t nlen = strlen(function);
    uint8_t nlenenc[4] = {nlen>>24, nlen>>16, nlen>>8, nlen};
    if (nero_write_header(n, NERO_FRAME_REQUEST, rqid, 5 + nlen + args->size)
     || nero_on_write(n, nlenenc, 4)
     || nero_on_write(n, (const uint8_t*)function, nlen + 1)
     || nero_on_write(n, args->data, args->size))
        return mun_error_up();
    return mun_ok;
}

// data(NERO_FRAME_RESPONSE) ::= {args : uint8_t}[]
static int nero_write_response(struct nero *n, uint32_t rqid, struct romp_iovec *values) {
    if (nero_write_header(n, NERO_FRAME_RESPONSE, rqid, values->size) || nero_on_write(n, values->data, values->size))
        return mun_error_up();
    return mun_ok;
}

// data(NERO_FRAME_ERROR) ::= {code : uint32_t} {name_len : uint32_t} '\0' {name : uint8_t}[name_len] {text : uint8_t}[] '\0'
static int nero_write_error(struct nero *n, uint32_t rqid) {
    const struct mun_error *err = mun_last_error();
    uint32_t code = err->code;
    uint32_t nlen = strlen(err->name);
    size_t len = 10 + nlen + strlen(err->text);
    uint8_t buf[len];
    memcpy(buf, (uint8_t[]){code>>24, code>>16, code>>8, code, nlen>>24, nlen>>16, nlen>>8, nlen}, 8);
    memcpy(&buf[8], err->name, nlen + 1);
    memcpy(&buf[8 + nlen + 1], err->text, len - nlen - 9);
    if (nero_write_header(n, NERO_FRAME_ERROR, rqid, len) || nero_on_write(n, buf, len))
        return mun_error_up();
    return mun_ok;
}

static int nero_on_frame(struct nero *n, uint8_t type, uint32_t rqid, const uint8_t *data, size_t size) {
    if (type == NERO_FRAME_REQUEST) {
        if (size < 5)
            return mun_error(nero_protocol, "malformed request");
        uint32_t nlen = data[0]<<24 | data[1]<<16 | data[2]<<8 | data[3];
        if (size < 5 + nlen || data[4 + nlen] != 0)
            return mun_error(nero_protocol, "malformed request");
        const char *function = (const char *)&data[4];
        for (unsigned i = 0; i < n->exported.size; i++) {
            if (!strcmp(function, n->exported.data[i].name)) {
                struct romp_iovec in = mun_vec_init_borrow(&data[5 + nlen], size - 5 - nlen);
                struct romp_iovec out = {};
                if (n->exported.data[i].code(n->exported.data[i].data, &in, &out))
                    return mun_vec_fini(&out), mun_error_up();
                if (nero_write_response(n, rqid, &out))
                    return mun_vec_fini(&out), mun_error_up();
                return mun_vec_fini(&out), mun_ok;
            }
        }
        return mun_error(nero_not_exported, "%s", function);
    } else if (type == NERO_FRAME_RESPONSE || type == NERO_FRAME_ERROR) {
        for (unsigned i = 0; i < n->queued.size; i++) {
            if (n->queued.data[i].id == rqid) {
                struct nero_future fut = n->queued.data[i];
                mun_vec_erase(&n->queued, i, 1);
                if (!fut.ev)
                    break;
                *fut.errmarker = type == NERO_FRAME_ERROR;
                if (mun_vec_extend(fut.data, data, size))
                    return mun_error_up();
                if (cone_event_emit(fut.ev))
                    return mun_error_up();
                break;
            }
        }
        return mun_ok;
    }
    return mun_error(nero_protocol, "unknown frame type %u", type);
}

int nero_run(struct nero *n) {
    uint8_t data[2048];
    for (ssize_t size; (size = read(n->fd, data, sizeof(data))); ) {
        if (size < 0) {
            if (errno == ECANCELED)
                return mun_ok;
            return mun_error_os();
        }
        if (n->skip >= size) {
            n->skip -= size;
            continue;
        }
        if (mun_vec_extend(&n->rdbuffer, data + n->skip, size - n->skip))
            return mun_error_up();
        n->skip = 0;
        while (1) {
            if (n->rdbuffer.size < 9)
                break;
            uint32_t size = n->rdbuffer.data[0]<<24 | n->rdbuffer.data[1]<<16 | n->rdbuffer.data[2]<<8 | n->rdbuffer.data[3];
            uint32_t rqid = n->rdbuffer.data[4]<<24 | n->rdbuffer.data[5]<<16 | n->rdbuffer.data[6]<<8 | n->rdbuffer.data[7];
            uint8_t type = n->rdbuffer.data[8];
            if (size > NERO_MAX_FRAME_SIZE) {
                mun_error(nero_protocol, "received frame too big");
                if (nero_write_error(n, rqid))
                    return mun_error_up();
                n->skip = n->rdbuffer.size > size ? 0 : size - n->rdbuffer.size;
                mun_vec_erase(&n->rdbuffer, 0, n->rdbuffer.size > size ? size : n->rdbuffer.size);
                continue;
            }
            if (n->rdbuffer.size < size + 9)
                break;
            if (nero_on_frame(n, type, rqid, n->rdbuffer.data + 9, size))
                if (nero_write_error(n, rqid))
                    return mun_error_up();
            mun_vec_erase(&n->rdbuffer, 0, size + 9);
        }
    }
    return mun_ok;
}

void nero_fini(struct nero *n) {
    for (unsigned i = 0; i < n->queued.size; i++)
        if (cone_event_emit(n->queued.data[i].ev))
            mun_error_show("could not wake coroutine due to", NULL);
    mun_vec_fini(&n->rdbuffer);
    mun_vec_fini(&n->wrbuffer);
    mun_vec_fini(&n->queued);
    mun_vec_fini(&n->exported);
    if (n->writer) {
        cone_cancel(n->writer);
        cone_decref(n->writer);
    }
    close(n->fd);
}

int nero_call(struct nero *n, const char *function, ...) {
    va_list args;
    va_start(args, function);

    uint32_t rqid = n->next_req++;
    struct romp_iovec data = {};
    if (romp_vencode(&data, va_arg(args, const char *), args))
        return va_end(args), mun_vec_fini(&data), mun_error_up();
    if (nero_write_request(n, rqid, function, &data))
        return va_end(args), mun_vec_fini(&data), mun_error_up();
    mun_vec_fini(&data);

    unsigned err = 0;
    struct cone_event ev = {};
    struct nero_future fut = {rqid, &err, &ev, &data};
    if (mun_vec_append(&n->queued, &fut))
        return va_end(args), mun_error_up();
    if (cone_wait(&ev)) {
        for (unsigned i = 0; i < n->queued.size; i++)
            if (n->queued.data[i].ev == &ev)
                return va_end(args), mun_vec_erase(&n->queued, i, 1), mun_error_up();
        return va_end(args), mun_vec_fini(&data), mun_error_up();
    }

    if (err) {
        va_end(args);
        if (data.size < 10 || data.data[data.size - 1] != 0)
            return mun_vec_fini(&data), mun_error(nero_protocol, "malformed error return");
        uint32_t code = data.data[0]<<24 | data.data[1]<<16 | data.data[2]<<8 | data.data[3];
        uint32_t nlen = data.data[4]<<24 | data.data[5]<<16 | data.data[6]<<8 | data.data[7];
        if (data.size + 10 < nlen || data.data[8 + nlen] != 0)
            return mun_vec_fini(&data), mun_error(nero_protocol, "malformed error return");
        return mun_error_at(code, (const char *)&data.data[8], __FILE__, __FUNCTION__, __LINE__, "%s",
                                  (const char *)&data.data[9 + nlen]);
    }

    struct romp_iovec ret = data;
    if (romp_vdecode(&ret, va_arg(args, const char *), args))
        return va_end(args), mun_vec_fini(&data), mun_error_up();
    return va_end(args), mun_vec_fini(&data), mun_ok;
}
