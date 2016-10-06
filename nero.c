#include "nero.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cno/core.h"

struct nero_future
{
    unsigned id;
    struct cone_event *ev;
    struct romp_iovec *data;
};

enum
{
    NERO_FRAME_REQUEST = CNO_FRAME_UNKNOWN,
    NERO_FRAME_RESPONSE,
    NERO_FRAME_ERROR,
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

/* mun_extern("libcno") */
static int nero_on_write(struct nero *n, const char *data, size_t size) {
    if (!n->writer)
        if (!(n->writer = cone(&nero_writer, n)))
            return CNO_ERROR(NO_MEMORY, "could not start a writer coroutine");
    if (mun_vec_extend(&n->buffer, data, size))
        return CNO_ERROR(NO_MEMORY, "could not push data to buffer");
    return CNO_OK;
}

/* mun_extern("libcno") */
static int nero_on_frame(struct nero *n, const struct cno_frame_t *frame) {
    (void)n;
    switch (frame->type) {
        case NERO_FRAME_REQUEST:
            // ...
            return CNO_ERROR(NOT_IMPLEMENTED, "NERO_FRAME_REQUEST");
        case NERO_FRAME_RESPONSE:
            // ...
            return CNO_ERROR(NOT_IMPLEMENTED, "NERO_FRAME_RESPONSE");
        case NERO_FRAME_ERROR:
            // ...
            return CNO_ERROR(NOT_IMPLEMENTED, "NERO_FRAME_ERROR");
    }
    return CNO_OK;
}

/* mun_extern("libcno") */
static int nero_on_stream_start(struct nero *n, uint32_t stream) {
    return cno_write_reset(n->http, stream, CNO_RST_REFUSED_STREAM);
}

static int nero_restore_cno_error_impl(const char *file, const char *func, unsigned line) {
    const struct cno_error_t *e = cno_error();
    mun_error_at(mun_errno_nero_http2 | e->code, "nero_http2", e->traceback[0].file, "?? ",
                 e->traceback[0].line, "%s", e->text);
    for (const struct cno_traceback_t *tb = &e->traceback[1]; tb != e->traceback_end; tb++)
        mun_error_up_at(tb->file, "?? ", tb->line);
    return mun_error_up_at(file, func, line);
}

#define nero_restore_cno_error() nero_restore_cno_error_impl(__FILE__, __FUNCTION__, __LINE__)

int nero_init(struct nero *n) {
    n->http = malloc(sizeof(struct cno_connection_t));
    if (n->http == NULL)
        return mun_error(memory, "need %zu bytes", sizeof(struct cno_connection_t));

    int urandom = open("/dev/urandom", O_RDONLY | O_NONBLOCK);
    if (urandom < 0)
        return mun_error_os();
    int state = 0;
    do {
        char rnd1, rnd2;
        if (read(urandom, &rnd1, 1) != 1 || write(n->fd, &rnd1, 1) != 1 || read(n->fd, &rnd2, 1) != 1)
            return close(urandom), mun_error_os();
        state = (rnd1 > rnd2) - (rnd1 < rnd2);
    } while (state == 0);
    close(urandom);

    cno_connection_init(n->http, state == 1 ? CNO_SERVER : CNO_CLIENT);
    n->http->cb_data = n;
    n->http->on_write = (int(*)(void*, const char*, size_t)) &nero_on_write;
    n->http->on_frame = (int(*)(void*, const struct cno_frame_t*)) &nero_on_frame;
    n->http->on_stream_start = (int(*)(void*, uint32_t)) &nero_on_stream_start;
    if (cno_connection_made(n->http, CNO_HTTP2))
        return nero_fini(n), nero_restore_cno_error();
    return mun_ok;
}

int nero_run(struct nero *n) {
    char buf[2048];
    for (ssize_t rd; (rd = read(n->fd, buf, sizeof(buf))); ) {
        if (rd < 0)
            return mun_error_os();
        if (cno_connection_data_received(n->http, buf, rd))
            return nero_restore_cno_error();
    }
    if (n->http && cno_connection_lost(n->http))
        return nero_restore_cno_error();
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
    if (n->http) {
        cno_connection_reset(n->http);
        free(n->http);
        n->http = NULL;
    }
    close(n->fd);
}

int nero_stop(struct nero *n) {
    return !n->http ? mun_ok : cno_connection_stop(n->http) ? nero_restore_cno_error() : mun_ok;
}

int nero_call(struct nero *n, const char *service, const char *fn, ...) {
    if (!n->http)
        return mun_error(assert, "nero is not yet ready");
    va_list args;
    va_start(args, fn);
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
