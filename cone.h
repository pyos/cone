#pragma once
/*
 * cone / coroutines
 *        --     --
 */
#include "cot.h"

#include <fcntl.h>
#include <unistd.h>
#include <stdatomic.h>

#if !defined(CONE_EPOLL) && __linux__
#define CONE_EPOLL 1
#endif

#if !defined(CONE_XCHG_RSP) && __linux__ && __x86_64__
#define CONE_XCHG_RSP 1
#endif

#ifndef CONE_IO_TIMEOUT
#define CONE_IO_TIMEOUT 30000000000ull
#endif

#ifndef CONE_DEFAULT_STACK
#define CONE_DEFAULT_STACK 65536
#endif

#if CONE_EPOLL
#include <sys/epoll.h>
#else
#include <sys/select.h>
#endif
#if !CONE_XCHG_RSP
#include <ucontext.h>
#endif

enum cone_flags {
    CONE_SCHEDULED = 0x1,
    CONE_RUNNING   = 0x2,
    CONE_FINISHED  = 0x4,
    CONE_CANCELLED = 0x8,
};

struct cone_closure {
    int (*code)(void*);
    void *data;
};

struct cone_call_at {
    struct cone_closure f;
    cot_nsec time;
};

struct cone_event_vec cot_vec(struct cone_closure);

struct cone_event_schedule cot_vec(struct cone_call_at);

struct cone_event_fd_sub {
    int fd;
    struct cone_closure cbs[2];
    struct cone_event_fd_sub *link;
};

struct cone_event_fd {
    int epoll;
    struct cone_event_fd_sub *fds[127];
};

struct cone_loop {
    int selfpipe[2];
    volatile atomic_uint active;
    volatile atomic_bool pinged;
    struct cone_event_fd io;
    struct cone_event_vec on_ping;
    struct cone_event_vec on_exit;
    struct cone_event_schedule at;
};

struct cone {
    int refcount;
    enum cone_flags flags;
    struct cone_loop *loop;
    struct cone_closure body;
    struct cone_event_vec done;
#if CONE_XCHG_RSP
    void **rsp;
#else
    ucontext_t inner;
    ucontext_t outer;
#endif
    char stack[];
};

extern _Thread_local struct cone * volatile cone;

#define cone_bind(f, data) ((struct cone_closure){(int(*)(void*))f, data})

static inline int cone_event_emit(struct cone_closure *ev) {
    struct cone_closure cb = *ev;
    *ev = (struct cone_closure){};
    return cb.code && cb.code(cb.data);
}

static inline void cone_event_vec_fini(struct cone_event_vec *ev) {
    cot_vec_fini(ev);
}

static inline int cone_event_vec_connect(struct cone_event_vec *ev, struct cone_closure cb) {
    return cot_vec_append(ev, &cb);
}

static inline int cone_event_vec_emit(struct cone_event_vec *ev) {
    while (ev->size) {
        if (cone_event_emit(&ev->data[0]))
            return cot_error_up();  // TODO not fail
        cot_vec_erase(ev, 0, 1);
    }
    return cot_ok;
}

static inline void cone_event_schedule_fini(struct cone_event_schedule *ev) {
    cot_vec_fini(ev);
}

static inline cot_nsec cone_event_schedule_emit(struct cone_event_schedule *ev) {
    while (ev->size) {
        cot_nsec now = cot_nsec_monotonic();
        struct cone_call_at next = ev->data[0];
        if (cot_u128_gt(next.time, now))
            return cot_u128_sub(next.time, now);
        cot_vec_erase(ev, 0, 1);
        if (cone_event_emit(&next.f))
            return cot_error_up(), (cot_u128){};  // TODO not fail
    }
    return COT_U128_MAX;
}

static inline int cone_event_schedule_connect(struct cone_event_schedule *ev, cot_nsec delay, struct cone_closure cb) {
    struct cone_call_at r = {cb, cot_u128_add(cot_nsec_monotonic(), delay)};
    size_t left = 0, right = ev->size;
    while (left != right) {
        size_t mid = (right + left) / 2;
        if (cot_u128_lt(r.time, ev->data[mid].time))
            right = mid;
        else
            left = mid + 1;
    }
    return cot_vec_insert(ev, left, &r);
}

static inline int cone_event_fd_init(struct cone_event_fd *set) {
#if CONE_EPOLL
    return (set->epoll = epoll_create1(0)) < 0 ? cot_error_os() : cot_ok;
#else
    set->epoll = -1;
    return cot_ok;
#endif
}

static inline void cone_event_fd_fini(struct cone_event_fd *set) {
    for (size_t i = 0; i < sizeof(set->fds) / sizeof(set->fds[0]); i++)
        for (struct cone_event_fd_sub *c; (c = set->fds[i]) != NULL; free(c))
            set->fds[i] = c->link;
    if (set->epoll >= 0)
        close(set->epoll);
}

static inline struct cone_event_fd_sub **cone_event_fd_bucket(struct cone_event_fd *set, int fd) {
    struct cone_event_fd_sub **b = &set->fds[fd % (sizeof(set->fds) / sizeof(set->fds[0]))];
    while (*b && (*b)->fd != fd) b = &(*b)->link;
    return b;
}

static inline struct cone_event_fd_sub *cone_event_fd_open(struct cone_event_fd *set, int fd) {
    struct cone_event_fd_sub **b = cone_event_fd_bucket(set, fd);
    if (*b != NULL)
        return *b;
    if ((*b = (struct cone_event_fd_sub *) calloc(1, sizeof(struct cone_event_fd_sub))) == NULL)
        return cot_error(memory, "-"), NULL;
    (*b)->fd = fd;
#if CONE_EPOLL
    struct epoll_event params = {EPOLLRDHUP|EPOLLHUP|EPOLLET|EPOLLIN|EPOLLOUT, {.ptr = *b}};
    if (epoll_ctl(set->epoll, EPOLL_CTL_ADD, fd, &params))
        cot_error_os(), free(*b), *b = NULL;
#endif
    return *b;
}

static inline void cone_event_fd_close(struct cone_event_fd *set, struct cone_event_fd_sub *ev) {
#if CONE_EPOLL
    epoll_ctl(set->epoll, EPOLL_CTL_DEL, ev->fd, NULL);
#endif
    *cone_event_fd_bucket(set, ev->fd) = ev->link;
    free(ev);
}

static inline int cone_event_fd_connect(struct cone_event_fd *set, int fd, int write, struct cone_closure cb) {
    struct cone_event_fd_sub *ev = cone_event_fd_open(set, fd);
    if (ev == NULL)
        return cot_error_up();
    if (ev->cbs[write].code)
        return cot_error(assert, "two readers/writers on one file descriptor");
    ev->cbs[write] = cb;
    return 0;
}

static inline int cone_event_fd_emit(struct cone_event_fd *set, cot_nsec timeout) {
    if (cot_u128_eq(timeout, (cot_u128){}))
        return cot_error_up();
    if (cot_u128_gt(timeout, (cot_u128){0, CONE_IO_TIMEOUT}))
        timeout = (cot_u128){0, CONE_IO_TIMEOUT};
#if CONE_EPOLL
    struct epoll_event evs[32];
    int got = epoll_wait(set->epoll, evs, 32, cot_u128_div(timeout, 1000000ul).L);
    if (got < 0)
        return errno == EINTR ? cot_ok : cot_error_os();
    for (size_t i = 0; i < (size_t)got; i++) {
        struct cone_event_fd_sub *c = (struct cone_event_fd_sub*)evs[i].data.ptr;
        if ((evs[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP)) && cone_event_emit(&c->cbs[0]))
            return cot_error_up();  // TODO not fail
        if ((evs[i].events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) && cone_event_emit(&c->cbs[1]))
            return cot_error_up();  // TODO not fail
        if (c->cbs[0].code == NULL && c->cbs[1].code == NULL)
            cone_event_fd_close(set, c);
    }
#else
    fd_set fds[2] = {};
    int max_fd = 0;
    for (size_t i = 0; i < sizeof(set->fds) / sizeof(set->fds[0]); i++) {
        for (struct cone_event_fd_sub *c = set->fds[i], *next = NULL; c; c = next) {
            next = c->link;
            if (c->cbs[0].code == NULL && c->cbs[1].code == NULL) {
                cone_event_fd_close(set, c);
                continue;
            }
            if (max_fd <= c->fd)
                max_fd = c->fd + 1;
            for (int i = 0; i < 2; i++)
                if (c->cbs[i].code)
                    FD_SET(c->fd, &fds[i]);
        }
    }
    struct timeval us = {cot_u128_div(timeout, 1000000000ull).L, timeout.L % 1000000000ull / 1000};
    if (select(max_fd, &fds[0], &fds[1], NULL, &us) < 0)
        return errno == EINTR ? cot_ok : cot_error_os();
    for (size_t i = 0; i < sizeof(set->fds) / sizeof(set->fds[0]); i++)
        for (struct cone_event_fd_sub *c = set->fds[i]; c; c = c->link)
            for (int i = 0; i < 2; i++)
                if (FD_ISSET(c->fd, &fds[i]) && cone_event_emit(&c->cbs[i]))
                    return cot_error_up();  // TODO not fail
#endif
    return cot_ok;
}

static inline int cone_unblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) ? cot_error_os() : cot_ok;
}

static inline int cone_loop_consume_ping(struct cone_loop *loop) {
    ssize_t rd = read(loop->selfpipe[0], &rd, sizeof(rd));  // never yields
    atomic_store_explicit(&loop->pinged, 0, memory_order_release);
    if (cone_event_fd_connect(&loop->io, loop->selfpipe[0], 0, cone_bind(&cone_loop_consume_ping, loop)))
        return cot_error_up();
    return cone_event_schedule_connect(&loop->at, (cot_u128){}, cone_bind(&cone_event_vec_emit, &loop->on_ping));
}

static inline int cone_loop_fini(struct cone_loop *loop) {
    int ret = cone_event_vec_emit(&loop->on_exit);
    close(loop->selfpipe[0]);
    close(loop->selfpipe[1]);
    cone_event_fd_fini(&loop->io);
    cone_event_vec_fini(&loop->on_ping);
    cone_event_vec_fini(&loop->on_exit);
    cone_event_schedule_fini(&loop->at);
    return ret;
}

static inline int cone_loop_init(struct cone_loop *loop) {
#ifdef _GNU_SOURCE
    if (pipe2(loop->selfpipe, O_NONBLOCK))
        return cot_error_os();
#else
    if (pipe(loop->selfpipe))
        return cot_error_os();
    if (cone_unblock(loop->selfpipe[0]) || cone_unblock(loop->selfpipe[1]))
        return cone_loop_fini(loop), cot_error_up();
#endif
    atomic_init(&loop->active, 0);
    atomic_init(&loop->pinged, 0);
    cone_event_fd_init(&loop->io);
    if (cone_event_fd_connect(&loop->io, loop->selfpipe[0], 0, cone_bind(&cone_loop_consume_ping, loop)))
        return cone_loop_fini(loop), cot_error_up();
    return cot_ok;
}

static inline int cone_loop_run(struct cone_loop *loop) {
    while (atomic_load_explicit(&loop->active, memory_order_acquire))
        if (cone_event_fd_emit(&loop->io, cone_event_schedule_emit(&loop->at)))
            return cot_error_up();
    return cot_ok;
}

static inline int cone_loop_ping(struct cone_loop *loop) {
    _Bool expect = 0;
    if (!atomic_compare_exchange_strong_explicit(&loop->pinged, &expect, 1, memory_order_acq_rel, memory_order_relaxed))
        return cot_ok;
    if (write(loop->selfpipe[1], "", 1) == 1)
        return cot_ok;
    atomic_store(&loop->pinged, 0);
    return cot_error_os();
}

static inline void cone_loop_inc(struct cone_loop *loop) {
    atomic_fetch_add_explicit(&loop->active, 1, memory_order_release);
}

static inline int cone_loop_dec(struct cone_loop *loop) {
    return atomic_fetch_sub_explicit(&loop->active, 1, memory_order_release) == 1 ? cone_loop_ping(loop) : cot_ok;
}

static inline int cone_switch(struct cone *c) {
    c->flags ^= CONE_RUNNING;
#if CONE_XCHG_RSP
    __asm__ volatile (
               "jmp %=0f\n"
        "%=1:" "push %%rbp\n"
               "push %%rdi\n"
               "mov  %%rsp, (%%rax)\n"
               "mov  %%rcx, %%rsp\n"
               "pop  %%rdi\n"
               "pop  %%rbp\n"
               "ret\n"
        "%=0:" "call %=1b"
      :
      : "a"(&c->rsp), "c"(c->rsp)
      : "rbx", "rdx", "rsi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
        "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9",
        "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "cc"
    );
#else
    if (c->flags & CONE_RUNNING)
        swapcontext(&c->outer, &c->inner);
    else
        swapcontext(&c->inner, &c->outer);
#endif
    return cot_ok;
}

static inline int cone_run(struct cone *c) {
    struct cone* preempted = cone;
    c->flags &= ~CONE_SCHEDULED;
    int ret = cone_switch(cone = c);
    cone = preempted;
    return ret;
}

static inline int cone_schedule(struct cone *c) {
    if (cone_event_schedule_connect(&c->loop->at, (cot_u128){}, cone_bind(&cone_run, c)))
        return cot_error_up();
    c->flags |= CONE_SCHEDULED;
    return cot_ok;
}

#define cone_pause(connect, ...) { \
    return connect(__VA_ARGS__, cone_bind(&cone_schedule, cone)) ? cot_error_up() : cone_switch(cone); }

static inline int cone_wait(struct cone_event_vec *ev) cone_pause(cone_event_vec_connect, ev)

static inline int cone_iowait(int fd, int write) cone_pause(cone_event_fd_connect, &cone->loop->io, fd, write)

static inline int cone_sleep(cot_nsec delay) cone_pause(cone_event_schedule_connect, &cone->loop->at, delay)

static inline struct cone *cone_incref(struct cone *c) {
    c->refcount++;
    return c;
}

static inline int cone_decref(struct cone *c) {
    if (c && --c->refcount == 0) {
        cone_event_vec_fini(&c->done);
        free(c);
    }
    return c ? cot_ok : cot_error_up();
}

static inline int cone_join(struct cone *c) {
    int ret = c->flags & CONE_FINISHED ? 0 : cone_wait(&c->done);
    cone_decref(c);
    return ret;
}

static inline __attribute__((noreturn)) void cone_body(struct cone *c) {
    if (cone_event_emit(&c->body))
        abort();
    for (size_t i = 0; i < c->done.size; i++)
        if (cone_event_schedule_connect(&c->loop->at, (cot_u128){}, c->done.data[i]))
            abort();
    c->flags |= CONE_FINISHED;
    cone_switch(c);
    abort();
}

static inline struct cone *cone_spawn(struct cone_loop *loop, size_t size, struct cone_closure body) {
    size &= ~(size_t)15;
    if (size < sizeof(struct cone))
        size = CONE_DEFAULT_STACK;
    struct cone *c = (struct cone *)malloc(size);
    if (c == NULL)
        return cot_error(memory, "-"), NULL;
    *c = (struct cone){.refcount = 1, .loop = loop, .body = body};
#if CONE_XCHG_RSP
    c->rsp = (void **)(c->stack + size - sizeof(struct cone)) - 4;
    c->rsp[0] = c;                  // %rdi
    c->rsp[1] = NULL;               // %rbp
    c->rsp[2] = (void*)&cone_body;  // %rip
    c->rsp[3] = NULL;               // return address
#else
    getcontext(&c->inner);
    c->inside = 0;
    c->inner.uc_stack.ss_sp = c->stack;
    c->inner.uc_stack.ss_size = size - sizeof(struct cone);
    makecontext(&c->inner, (void(*)(void))&cone_body, 1, c);
#endif
    if (cone_event_vec_connect(&c->done, cone_bind(&cone_loop_dec, loop)) ||
        cone_event_vec_connect(&c->done, cone_bind(&cone_decref, c)) ||
        cone_schedule(c)) {
        cone_decref(c);
        return cot_error_up(), NULL;
    }
    cone_loop_inc(loop);
    return cone_incref(c);
}

#define cone(f, arg) cone_spawn(cone->loop, 0, cone_bind(f, arg))

static inline int cone_main(size_t stksz, struct cone_closure body) {
    struct cone_loop loop = {};
    int err = cone_loop_init(&loop)
           || cone_decref(cone_spawn(&loop, stksz, body))
           || cone_loop_run(&loop);
    return cone_loop_fini(&loop) || err ? cot_error_up() : cot_ok;
}
