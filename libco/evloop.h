#pragma once
#include "events.h"

#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdatomic.h>

static inline int
setnonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags == -1 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct co_loop
{
    volatile atomic_bool running, pinged;
    int ping_r, ping_w;
    struct co_fd_set io;
    struct co_event_vec on_ping;
    struct co_event_vec on_exit;
    struct co_event_schedule sched;
};

static inline int
co_loop_consume_ping(struct co_loop *loop) {
    ssize_t rd = read(loop->ping_r, &rd, sizeof(rd));  // never yields
    loop->pinged = 0;
    struct co_fd_duplex *ev = co_fd_duplex(&loop->io, loop->ping_r);
    if (ev == NULL)
        return -1;
    co_event_fd_connect(&ev->read, co_callback_bind(&co_loop_consume_ping, loop));
    struct co_event_scheduler now = co_event_schedule_after(&loop->sched, co_u128_value(0));
    return co_event_scheduler_connect(&now, co_callback_bind(&co_event_vec_emit, &loop->on_ping));
}

static inline int
co_loop_fini(struct co_loop *loop) {
    int ret = co_event_vec_emit(&loop->on_exit);
    close(loop->ping_r);
    close(loop->ping_w);
    co_fd_set_fini(&loop->io);
    co_event_vec_fini(&loop->on_ping);
    co_event_vec_fini(&loop->on_exit);
    co_event_schedule_fini(&loop->sched);
    return ret;
}

static inline int
co_loop_init(struct co_loop *loop) {
    int fd[2];
    if (pipe2(fd, O_NONBLOCK))
        return -1;

    *loop = (struct co_loop){.ping_r = fd[0], .ping_w = fd[1]};
    atomic_init(&loop->running, false);
    atomic_init(&loop->pinged, false);
    co_fd_set_init(&loop->io);
    setnonblocking(loop->ping_w);

    struct co_fd_duplex *ev = co_fd_duplex(&loop->io, loop->ping_r);
    if (ev == NULL) {
        co_loop_fini(loop);
        return -1;
    }
    co_event_fd_connect(&ev->read, co_callback_bind(&co_loop_consume_ping, loop));
    return 0;
}

static inline int
co_loop_run(struct co_loop *loop) {
    assert("aio::evloop::run must not call itself" && !atomic_load(&loop->running));
    for (atomic_store(&loop->running, true); atomic_load(&loop->running); ) {
        struct co_nsec_offset timeout = co_event_schedule_emit(&loop->sched);
        if (co_u128_eq(timeout, CO_U128_MAX))
            return -1;
        errno = 0;
        if (co_fd_set_emit(&loop->io, timeout) && errno != EINTR)
            return -1;
    }
    return 0;
}

static inline int
co_loop_ping(struct co_loop *loop) {
    bool expect = false;
    if (!atomic_compare_exchange_strong(&loop->pinged, &expect, true) || write(loop->ping_w, "", 1) == 1)
        return 0;
    atomic_store(&loop->pinged, false);
    return -1;
}

static inline int
co_loop_stop(struct co_loop *loop) {
    bool expect = true;
    if (atomic_compare_exchange_strong(&loop->running, &expect, false))
        return co_loop_ping(loop);
    return -1;
}
