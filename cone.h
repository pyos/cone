#pragma once
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

#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdatomic.h>
#if CONE_EPOLL
#include <sys/epoll.h>
#else
#include <sys/select.h>
#endif
#if !CONE_XCHG_RSP
#include <ucontext.h>
#endif

static inline int
setnonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags == -1 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

struct cone_u128 { uint64_t H, L; };

#define CONE_U128(x) ((struct cone_u128){0, (uint64_t)x})
#define CONE_U128_MAX ((struct cone_u128){UINT64_MAX, UINT64_MAX})

#define cone_nsec cone_u128

#define cone_vec_impl(T) { \
    T* data;               \
    unsigned size;         \
    unsigned cap;          \
    unsigned shift;        \
}

struct cone_vec cone_vec_impl(char);

#define cone_vec(T) {            \
    union {                      \
        struct cone_vec decay;   \
        struct cone_vec_impl(T); \
    };                           \
}

struct cone_closure
{
    int (*code)(void*);
    void *data;
};

struct cone_call_at
{
    struct cone_closure f;
    struct cone_nsec time;
};

struct cone_event_vec { struct cone_vec(struct cone_closure) slots; };

struct cone_event_schedule { struct cone_vec(struct cone_call_at) queue; };

struct cone_event_fd_sub
{
    int fd;
    struct cone_closure cbs[2];
    struct cone_event_fd_sub *link;
};

struct cone_event_fd
{
    int epoll;
    struct cone_event_fd_sub *fds[127];
};

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

enum cone_flags
{
    CONE_SCHEDULED = 0x1,
    CONE_RUNNING   = 0x2,
    CONE_FINISHED  = 0x4,
    CONE_CANCELLED = 0x8,
};

struct cone
{
    int refcount, flags;
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

static inline struct cone_u128
cone_u128_add(struct cone_u128 a, struct cone_u128 b) {
    return (struct cone_u128){.H = a.H + b.H + (a.L + b.L < a.L), .L = a.L + b.L};
}

static inline struct cone_u128
cone_u128_sub(struct cone_u128 a, struct cone_u128 b) {
    return (struct cone_u128){.H = a.H - b.H - (a.L < b.L), .L = a.L - b.L};
}

static inline struct cone_u128
cone_u128_mul(struct cone_u128 a, uint32_t b) {
    return (struct cone_u128){.H = a.H * b + (((a.L >> 32) * b) >> 32), .L = a.L * b};
}

static inline struct cone_u128
cone_u128_div(struct cone_u128 a, uint32_t b) {
    uint64_t r = (a.H % b) << 32 | a.L >> 32;
    return (struct cone_u128){.H = a.H / b, .L = (r / b) << 32 | (((r % b) << 32 | (a.L & UINT32_MAX)) / b)};
}

static inline double
cone_u128_to_double(struct cone_u128 x) { return (double)x.H * (1ull << 63) * 2 + x.L; }

static inline int
cone_u128_eq(struct cone_u128 a, struct cone_u128 b) { return a.H == b.H && a.L == b.L; }

static inline int
cone_u128_lt(struct cone_u128 a, struct cone_u128 b) { return a.H < b.H || (a.H == b.H && a.L < b.L); }

static inline int
cone_u128_gt(struct cone_u128 a, struct cone_u128 b) { return a.H > b.H || (a.H == b.H && a.L > b.L); }

static inline struct cone_nsec
cone_nsec_from_timespec(struct timespec val) {
    return cone_u128_add(cone_u128_mul(CONE_U128(val.tv_sec), 1000000000ull), CONE_U128(val.tv_nsec));
}

static inline struct cone_nsec
cone_nsec_monotonic() {
    struct timespec val;
    return clock_gettime(CLOCK_MONOTONIC, &val) ? CONE_U128_MAX : cone_nsec_from_timespec(val);
}

static inline void
cone_vec_fini_s(size_t stride, struct cone_vec *vec) {
    free(vec->data - vec->shift * stride);
    *vec = (struct cone_vec){};
}

static inline void
cone_vec_shift_s(size_t stride, struct cone_vec *vec, size_t start, int offset) {
    if (start < vec->size)
        memmove(vec->data + (start + offset) * stride, vec->data + start * stride, (vec->size - start) * stride);
    vec->size += offset;
}

static inline int
cone_vec_reserve_s(size_t stride, struct cone_vec *vec, size_t elems) {
    if (vec->size + elems <= vec->cap)
        return 0;
    if (vec->shift) {
        vec->data -= vec->shift * stride;
        vec->size += vec->shift;
        vec->cap  += vec->shift;
        cone_vec_shift_s(stride, vec, vec->shift, -(int)vec->shift);
        vec->shift = 0;
        if (vec->size + elems <= vec->cap)
            return 0;
    }
    size_t ncap = vec->cap + (elems > vec->cap ? elems : vec->cap);
    void *r = realloc(vec->data, stride * ncap);
    if (r == NULL)
        return -1;
    vec->data = (char*) r;
    vec->cap = ncap;
    return 0;
}

static inline int
cone_vec_splice_s(size_t stride, struct cone_vec *vec, size_t i, const void *restrict elems, size_t n) {
    if (cone_vec_reserve_s(stride, vec, n))
        return -1;
    cone_vec_shift_s(stride, vec, i, n);
    memcpy(vec->data + i * stride, elems, stride);
    return 0;
}

static inline void
cone_vec_erase_s(size_t stride, struct cone_vec *vec, size_t i, size_t n) {
    if (i + n == vec->size)
        vec->size -= n;
    else if (i == 0) {
        vec->data  += n * stride;
        vec->size  -= n;
        vec->cap   -= n;
        vec->shift += n;
    } else
        cone_vec_shift_s(stride, vec, i + n, -(int)n);
}

#define cone_vec_strided(vec)              (size_t)sizeof(*(vec)->data), &(vec)->decay
#define cone_vec_fini(vec)                 cone_vec_fini_s(cone_vec_strided(vec))
#define cone_vec_shift(vec, start, offset) cone_vec_shift_s(cone_vec_strided(vec), start, offset)
#define cone_vec_reserve(vec, elems)       cone_vec_reserve_s(cone_vec_strided(vec), elems)
#define cone_vec_splice(vec, i, elems, n)  cone_vec_splice_s(cone_vec_strided(vec), i, elems, n)
#define cone_vec_extend(vec, elems, n)     cone_vec_splice(vec, (vec)->size, elems, n)
#define cone_vec_insert(vec, i, elem)      cone_vec_splice(vec, i, elem, 1)
#define cone_vec_append(vec, elem)         cone_vec_extend(vec, elem, 1)
#define cone_vec_erase(vec, i, n)          cone_vec_erase_s(cone_vec_strided(vec), i, n)

static inline int
cone_event_emit(struct cone_closure *ev) {
    struct cone_closure cb = *ev;
    *ev = (struct cone_closure){};
    return cb.code && cb.code(cb.data);
}

static inline void
cone_event_vec_fini(struct cone_event_vec *ev) { cone_vec_fini(&ev->slots); }

static inline int
cone_event_vec_connect(struct cone_event_vec *ev, struct cone_closure cb) {
    return cone_vec_append(&ev->slots, &cb);
}

static inline int
cone_event_vec_emit(struct cone_event_vec *ev) {
    while (ev->slots.size) {
        if (cone_event_emit(&ev->slots.data[0]))
            return -1;  // TODO not fail
        cone_vec_erase(&ev->slots, 0, 1);
    }
    return 0;
}

static inline void
cone_event_schedule_fini(struct cone_event_schedule *ev) { cone_vec_fini(&ev->queue); }

static inline struct cone_nsec
cone_event_schedule_emit(struct cone_event_schedule *ev) {
    while (ev->queue.size) {
        struct cone_nsec now = cone_nsec_monotonic();
        struct cone_call_at next = ev->queue.data[0];
        if (cone_u128_gt(next.time, now))
            return cone_u128_sub(next.time, now);
        cone_vec_erase(&ev->queue, 0, 1);
        if (cone_event_emit(&next.f))
            return CONE_U128(0);  // TODO not fail
    }
    return CONE_U128_MAX;
}

static inline int
cone_event_schedule_connect(struct cone_event_schedule *ev, struct cone_nsec delay, struct cone_closure cb) {
    struct cone_call_at r = {cb, cone_u128_add(cone_nsec_monotonic(), delay)};
    size_t left = 0, right = ev->queue.size;
    while (left != right) {
        size_t mid = (right + left) / 2;
        if (cone_u128_lt(r.time, ev->queue.data[mid].time))
            right = mid;
        else
            left = mid + 1;
    }
    return cone_vec_insert(&ev->queue, left, &r);
}

static inline int
cone_event_fd_init(struct cone_event_fd *set) {
#if CONE_EPOLL
    return (set->epoll = epoll_create1(0)) < 0 ? -1 : 0;
#else
    set->epoll = -1;
    return 0;
#endif
}

static inline void
cone_event_fd_fini(struct cone_event_fd *set) {
    for (size_t i = 0; i < sizeof(set->fds) / sizeof(set->fds[0]); i++)
        for (struct cone_event_fd_sub *c; (c = set->fds[i]) != NULL; free(c))
            set->fds[i] = c->link;
    if (set->epoll >= 0)
        close(set->epoll);
}

static inline struct cone_event_fd_sub **
cone_event_fd_bucket(struct cone_event_fd *set, int fd) {
    struct cone_event_fd_sub **b = &set->fds[fd % (sizeof(set->fds) / sizeof(set->fds[0]))];
    while (*b && (*b)->fd != fd) b = &(*b)->link;
    return b;
}

static inline struct cone_event_fd_sub *
cone_event_fd_open(struct cone_event_fd *set, int fd) {
    struct cone_event_fd_sub **b = cone_event_fd_bucket(set, fd);
    if (!*b && (*b = (struct cone_event_fd_sub *) calloc(1, sizeof(struct cone_event_fd_sub)))) {
        (*b)->fd = fd;
    #if CONE_EPOLL
        struct epoll_event params = {EPOLLRDHUP|EPOLLHUP|EPOLLET|EPOLLIN|EPOLLOUT, {.ptr = *b}};
        if (epoll_ctl(set->epoll, EPOLL_CTL_ADD, fd, &params))
            free(*b), *b = NULL;
    #endif
    }
    return *b;
}

static inline void
cone_event_fd_close(struct cone_event_fd *set, struct cone_event_fd_sub *ev) {
#if CONE_EPOLL
    epoll_ctl(set->epoll, EPOLL_CTL_DEL, ev->fd, NULL);
#endif
    *cone_event_fd_bucket(set, ev->fd) = ev->link;
    free(ev);
}

static inline int
cone_event_fd_connect(struct cone_event_fd *set, int fd, int write, struct cone_closure cb) {
    struct cone_event_fd_sub *ev = cone_event_fd_open(set, fd);
    if (ev == NULL || ev->cbs[write].code)
        return -1;
    ev->cbs[write] = cb;
    return 0;
}

static inline int
cone_event_fd_emit(struct cone_event_fd *set, struct cone_nsec timeout) {
    if (cone_u128_eq(timeout, CONE_U128(0)))
        return -1;
    if (cone_u128_gt(timeout, CONE_U128(CONE_IO_TIMEOUT)))
        timeout = CONE_U128(CONE_IO_TIMEOUT);
#if CONE_EPOLL
    struct epoll_event evs[32];
    int got = epoll_wait(set->epoll, evs, 32, cone_u128_div(timeout, 1000000ul).L);
    if (got < 0)
        return errno == EINTR ? 0 : -1;
    for (size_t i = 0; i < (size_t)got; i++) {
        struct cone_event_fd_sub *c = (struct cone_event_fd_sub*)evs[i].data.ptr;
        if ((evs[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP)) && cone_event_emit(&c->cbs[0]))
            return -1;  // TODO not fail
        if ((evs[i].events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) && cone_event_emit(&c->cbs[1]))
            return -1;  // TODO not fail
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
    struct timeval us = {cone_u128_div(timeout, 1000000000ull).L, timeout.L % 1000000000ull / 1000};
    if (select(max_fd, &fds[0], &fds[1], NULL, &us) < 0)
        return errno == EINTR ? 0 : -1;
    for (size_t i = 0; i < sizeof(set->fds) / sizeof(set->fds[0]); i++)
        for (struct cone_event_fd_sub *c = set->fds[i]; c; c = c->link)
            for (int i = 0; i < 2; i++)
                if (FD_ISSET(c->fd, &fds[i]) && cone_event_emit(&c->cbs[i]))
                    return -1;  // TODO not fail
#endif
    return 0;
}

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

static inline int
cone_switch(struct cone *c) {
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
    return 0;
#else
    return c->flags & CONE_RUNNING ? swapcontext(&c->outer, &c->inner) : swapcontext(&c->inner, &c->outer);
#endif
}

static inline int
cone_run(struct cone *c) {
    struct cone* preempted = cone;
    c->flags &= ~CONE_SCHEDULED;
    int ret = cone_switch(cone = c);
    cone = preempted;
    return ret;
}

static inline int
cone_schedule(struct cone *c) {
    if (cone_event_schedule_connect(&c->loop->at, CONE_U128(0), cone_bind(&cone_run, c)))
        return -1;
    c->flags |= CONE_SCHEDULED;
    return 0;
}

#define cone_pause(connect, ...) { return !cone || connect(__VA_ARGS__, cone_bind(&cone_schedule, cone)) ? -1 : cone_switch(cone); }

static inline int
cone_wait(struct cone_event_vec *ev) cone_pause(cone_event_vec_connect, ev)

static inline int
cone_iowait(int fd, int write) cone_pause(cone_event_fd_connect, &cone->loop->io, fd, write)

static inline int
cone_sleep(struct cone_nsec delay) cone_pause(cone_event_schedule_connect, &cone->loop->at, delay)

static inline struct cone *
cone_incref(struct cone *c) { c->refcount++; return c; }

static inline int
cone_decref(struct cone *c) {
    if (c && --c->refcount == 0) {
        cone_event_vec_fini(&c->done);
        free(c);
    }
    return c ? 0 : -1;
}

static inline int
cone_join(struct cone *c) {
    int ret = c->flags & CONE_FINISHED ? 0 : cone_wait(&c->done);
    cone_decref(c);
    return ret;
}

static inline void __attribute__((noreturn))
cone_body(struct cone *c) {
    if (cone_event_emit(&c->body))
        abort();
    for (size_t i = 0; i < c->done.slots.size; i++)
        if (cone_event_schedule_connect(&c->loop->at, CONE_U128(0), c->done.slots.data[i]))
            abort();
    c->flags |= CONE_FINISHED;
    cone_switch(c);
    abort();
}

static inline struct cone *
cone_spawn(struct cone_loop *loop, size_t size, struct cone_closure body) {
    size &= ~(size_t)15;
    if (size < sizeof(struct cone))
        size = CONE_DEFAULT_STACK;
    struct cone *c = (struct cone *)malloc(size);
    if (c == NULL)
        return NULL;
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
        return NULL;
    }
    cone_loop_inc(loop);
    return cone_incref(c);
}

#define cone(f, arg) cone_spawn(cone->loop, 0, cone_bind(f, arg))

static inline int
cone_main(size_t stksz, struct cone_closure body) {
    struct cone_loop loop = {};
    int err = cone_loop_init(&loop)
           || cone_decref(cone_spawn(&loop, stksz, body))
           || cone_loop_run(&loop);
    return cone_loop_fini(&loop) || err ? -1 : 0;
}
