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
    unsigned id;
    struct cone_event *ev;
    struct romp_iovec *data;
};

static int nero_writer(struct nero *n) {
    char local[4096];
    while (n->buffer.size) {
        size_t size = n->buffer.size > sizeof(local) ? sizeof(local) : n->buffer.size;
        size_t remaining = size;
        const char *data = memcpy(local, n->buffer.data, remaining);
        for (ssize_t wr; remaining; data += wr, remaining -= wr) {
            if ((wr = write(n->fd, data, remaining)) < 0)
                return mun_error_os();
        }
        mun_vec_erase(&n->buffer, 0, size);
    }
    cone_decref(n->writer);
    n->writer = NULL;
    return mun_ok;
}

static int nero_on_write(struct nero *n, const char *data, size_t size) {
    if (!n->writer)
        if (!(n->writer = cone(&nero_writer, n)))
            return mun_error_up();
    if (mun_vec_extend(&n->buffer, data, size))
        return mun_error_up();
    return mun_ok;
}

enum
{
    NERO_FRAME_REQUEST,
    NERO_FRAME_RESPONSE,
    NERO_FRAME_ERROR,
};

static int nero_on_frame(struct nero *n, unsigned type, const uint8_t *data, size_t size) {
    (void)n;
    (void)data;
    (void)size;
    switch (type) {
        case NERO_FRAME_REQUEST:
            // ...
            return mun_error(not_implemented, "NERO_FRAME_REQUEST");
        case NERO_FRAME_RESPONSE:
            // ...
            return mun_error(not_implemented, "NERO_FRAME_RESPONSE");
        case NERO_FRAME_ERROR:
            // ...
            return mun_error(not_implemented, "NERO_FRAME_ERROR");
    }
    return mun_ok;
}

static int nero_on_read(struct nero *n, const uint8_t *data, size_t size) {
    if (n->skip >= size) {
        n->skip -= size;
        return mun_ok;
    }
    if (mun_vec_extend(&n->buffer, data + n->skip, size - n->skip))
        return mun_error_up();
    n->skip = 0;
    while (1) {
        if (n->buffer.size < 5)
            return mun_ok;
        uint32_t size = n->buffer.data[0]<<24 | n->buffer.data[1]<<16 | n->buffer.data[2]<<8 | n->buffer.data[3];
        uint8_t type = n->buffer.data[4];
        if (size > NERO_MAX_FRAME_SIZE) {
            // TODO send an error
            n->skip = n->buffer.size > size ? 0 : size - n->buffer.size;
            mun_vec_erase(&n->buffer, 0, n->buffer.size > size ? size : n->buffer.size);
            continue;
        }
        if (n->buffer.size < size + 5)
            return mun_ok;
        if (nero_on_frame(n, type, n->buffer.data + 5, size))
            return mun_error_up();
        mun_vec_erase(&n->buffer, 0, size + 5);
    }
}

int nero_run(struct nero *n) {
    uint8_t buf[2048];
    for (ssize_t rd; (rd = read(n->fd, buf, sizeof(buf))); ) {
        if (rd < 0)
            return mun_error_os();
        if (nero_on_read(n, buf, rd))
            return mun_error_up();
    }
    return mun_ok;
}

void nero_fini(struct nero *n) {
    mun_vec_fini(&n->buffer);
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
    (void)n;
/*
    struct cone_event ev = {};
    struct romp_iovec data = {};
    struct nero_awaiting req = { .id = n->next_req++, .ev = &ev, .data = &data };

    struct romp_iovec request = {};
    struct mun_vec service_enc = mun_vec_init_str(service);
    struct mun_vec fn_enc = mun_vec_init_str(fn);
    if (romp_encode(&data, "u1 u4 vi1 vi1", 0, req.id, &service_enc, &fn_enc))
        return mun_vec_fini(&data), mun_error_up();
    if (romp_vencode(&data, va_arg(args, const char *), args))
        return mun_vec_fini(&data), mun_error_up();

    if (n->written == n->queued.size)
        if (cone_decref(cone(&nero_writer, n)))
            return mun_vec_fini(&data), mun_error_up();
    if (mun_vec_append(&n->queued, &req))
        return mun_vec_fini(&data), mun_error_up();
    if (cone_wait(&ev))
        return mun_vec_fini(&ev), mun_vec_fini(&data), mun_error_up();
    mun_vec_fini(&ev);

    unsigned rid = 0;
    if (romp_decode(&data, "u4", &rid))
        return mun_vec_fini(&data), mun_error_up();
    if (romp_vdecode(&data, va_arg(args, const char *), args))
        return mun_vec_fini(&data), mun_error_up();
    if (n->next_req < rid)
        n->next_req = rid + 1;
    return mun_vec_fini(&data), mun_ok;
*/
    va_end(args);
    return mun_error(not_implemented, "nero_call");
}
