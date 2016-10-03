#pragma once
#include "ev.h"
#include "ev-fd.h"
#include "ev-sched.h"

#include <fcntl.h>
#include <stdbool.h>
#include <stdatomic.h>

static inline int
setnonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags == -1 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct cone_loop
{
    int selfpipe[2];
    volatile atomic_uint active;
    volatile atomic_bool pinged;
    struct cone_event_fd io;
    struct cone_event_vec on_ping;
    struct cone_event_vec on_exit;
    struct cone_event_schedule at;
};

static inline int
cone_loop_consume_ping(struct cone_loop *loop) {
    ssize_t rd = read(loop->selfpipe[0], &rd, sizeof(rd));  // never yields
    atomic_store_explicit(&loop->pinged, false, memory_order_release);
    if (cone_event_fd_connect(&loop->io, loop->selfpipe[0], 0, cone_bind(&cone_loop_consume_ping, loop)))
        return -1;
    return cone_event_schedule_connect(&loop->at, CONE_U128(0), cone_bind(&cone_event_vec_emit, &loop->on_ping));
}

static inline int
cone_loop_fini(struct cone_loop *loop) {
    int ret = cone_event_vec_emit(&loop->on_exit);
    close(loop->selfpipe[0]);
    close(loop->selfpipe[1]);
    cone_event_fd_fini(&loop->io);
    cone_event_vec_fini(&loop->on_ping);
    cone_event_vec_fini(&loop->on_exit);
    cone_event_schedule_fini(&loop->at);
    return ret;
}

static inline int
cone_loop_init(struct cone_loop *loop) {
#ifdef _GNU_SOURCE
    if (pipe2(loop->selfpipe, O_NONBLOCK))
        return -1;
#else
    if (pipe(loop->selfpipe))
        return -1;
    if (setnonblocking(loop->selfpipe[0]) || setnonblocking(loop->selfpipe[1]))
        return cone_loop_fini(loop), -1;
#endif
    atomic_init(&loop->active, 0);
    atomic_init(&loop->pinged, false);
    cone_event_fd_init(&loop->io);
    if (cone_event_fd_connect(&loop->io, loop->selfpipe[0], 0, cone_bind(&cone_loop_consume_ping, loop)))
        return cone_loop_fini(loop), -1;
    return 0;
}

static inline int
cone_loop_run(struct cone_loop *loop) {
    while (atomic_load_explicit(&loop->active, memory_order_acquire))
        if (cone_event_fd_emit(&loop->io, cone_event_schedule_emit(&loop->at)))
            return -1;
    return 0;
}

static inline int
cone_loop_ping(struct cone_loop *loop) {
    bool expect = false;
    if (!atomic_compare_exchange_strong_explicit(&loop->pinged, &expect, true, memory_order_acq_rel, memory_order_relaxed))
        return 0;
    if (write(loop->selfpipe[1], "", 1) == 1)
        return 0;
    atomic_store(&loop->pinged, false);
    return -1;
}

static inline void
cone_loop_inc(struct cone_loop *loop) {
    atomic_fetch_add_explicit(&loop->active, 1, memory_order_release);
}

static inline int
cone_loop_dec(struct cone_loop *loop) {
    return atomic_fetch_sub_explicit(&loop->active, 1, memory_order_release) == 1 ? cone_loop_ping(loop) : 0;
}
