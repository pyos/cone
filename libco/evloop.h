#pragma once
#include "events/fd.h"
#include "events/time.h"
#include "events/vec.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdatomic.h>

static inline int
setnonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags == -1 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct co_loop
{
    volatile atomic_size_t active;
    volatile atomic_bool pinged;
    int ping_r, ping_w;
    struct co_event_fd io;
    struct co_event_vec on_ping;
    struct co_event_vec on_exit;
    struct co_event_schedule sched;
};

static inline int
co_loop_consume_ping(struct co_loop *loop) {
    ssize_t rd = read(loop->ping_r, &rd, sizeof(rd));  // never yields
    loop->pinged = 0;
    if (co_event_fd_connect(&loop->io, loop->ping_r, 0, co_bind(&co_loop_consume_ping, loop)))
        return -1;
    return co_event_schedule_connect(&loop->sched, CO_U128(0), co_bind(&co_event_vec_emit, &loop->on_ping));
}

static inline int
co_loop_fini(struct co_loop *loop) {
    int ret = co_event_vec_emit(&loop->on_exit);
    close(loop->ping_r);
    close(loop->ping_w);
    co_event_fd_fini(&loop->io);
    co_event_vec_fini(&loop->on_ping);
    co_event_vec_fini(&loop->on_exit);
    co_event_schedule_fini(&loop->sched);
    return ret;
}

static inline int
co_loop_init(struct co_loop *loop) {
    int fd[2];
#ifdef _GNU_SOURCE
    if (pipe2(fd, O_NONBLOCK))
        return -1;
#else
    if (pipe(fd))
        return -1;
    if (setnonblocking(fd[0]) || setnonblocking(fd[1]))
        return close(fd[0]), close(fd[1]), -1;
#endif
    *loop = (struct co_loop){.ping_r = fd[0], .ping_w = fd[1]};
    atomic_init(&loop->active, 0);
    atomic_init(&loop->pinged, false);
    co_event_fd_init(&loop->io);
    setnonblocking(loop->ping_w);
    if (co_event_fd_connect(&loop->io, loop->ping_r, 0, co_bind(&co_loop_consume_ping, loop)))
        return co_loop_fini(loop), -1;
    return 0;
}

static inline int
co_loop_run(struct co_loop *loop) {
    while (atomic_load_explicit(&loop->active, memory_order_acquire))
        if (co_event_fd_emit(&loop->io, co_event_schedule_emit(&loop->sched)))
            return -1;
    return 0;
}

static inline int
co_loop_ping(struct co_loop *loop) {
    bool expect = false;
    if (!atomic_compare_exchange_strong_explicit(&loop->pinged, &expect, true, memory_order_acq_rel, memory_order_relaxed))
        return 0;
    if (write(loop->ping_w, "", 1) == 1)
        return 0;
    atomic_store(&loop->pinged, false);
    return -1;
}

static inline void
co_loop_inc(struct co_loop *loop) {
    atomic_fetch_add_explicit(&loop->active, 1, memory_order_release);
}

static inline int
co_loop_dec(struct co_loop *loop) {
    if (atomic_fetch_sub_explicit(&loop->active, 1, memory_order_release) == 1)
        return co_loop_ping(loop);
    return 0;
}
