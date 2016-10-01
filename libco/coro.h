#pragma once
#include "evloop.h"

#include <stdio.h>
#include <string.h>

#if !defined(COROUTINE_XCHG_RSP) && defined(__linux__) && defined(__x86_64__)
#define COROUTINE_XCHG_RSP 1
#endif

#if !COROUTINE_XCHG_RSP
#include <ucontext.h>
#endif

#ifndef COROUTINE_DEFAULT_STACK
#define COROUTINE_DEFAULT_STACK 65536
#endif

struct coro
{
    int refcount;
    struct co_loop *loop;
    struct co_event_vec done;
    struct co_closure body;
#if COROUTINE_XCHG_RSP
    void *regs[4];
#else
    ucontext_t inner;
    ucontext_t outer;
#endif
    char stack[];
};

extern _Thread_local struct coro * volatile coro_current;

static inline void
coro_enter(struct coro *c) {
#if COROUTINE_XCHG_RSP
    __asm__ volatile (
        "lea LJMPRET%=(%%rip), %%rcx\n"
        #define XCHG(a, b, tmp) "mov " a ", " tmp " \n mov " b ", " a " \n mov " tmp ", " b "\n"
        XCHG("%%rbp",  "0(%0)", "%%r8")  // gcc complains about clobbering %rbp with -fno-omit-frame-pointer
        XCHG("%%rsp",  "8(%0)", "%%r9")
        XCHG("%%rcx", "16(%0)", "%%r10")  // `xchg` is implicitly `lock`ed => slower than 3 `mov`s
        XCHG("%%rdi", "24(%0)", "%%r11")
        #undef XCHG
        "jmp *%%rcx\n"
        "LJMPRET%=:"
      :
      : "a"(c->regs)
      : "rbx", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
        "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9",
        "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "cc" // not "memory" (so don't read `regs`)
    );
#else
    swapcontext(&c->outer, &c->inner);
#endif
}

static inline void
coro_leave(struct coro *c) {
#if COROUTINE_XCHG_RSP
    coro_enter(c);
#else
    swapcontext(&c->inner, &c->outer);
#endif
}

static inline void __attribute__((noreturn))
coro_inner_code(struct coro *c) {
    if (c->body.function(c->body.data))
        abort();
    for (size_t i = 0; i < c->done.slots.size; i++)
        if (co_event_schedule_connect(&c->loop->sched, CO_U128(0), c->done.slots.data[i]))
            abort();
    co_event_vec_fini(&c->done);
    coro_leave(c);
    abort();
}

static inline int
coro_run(struct coro *c) {
    struct coro* preempted = coro_current;
    coro_enter(coro_current = c);
    coro_current = preempted;
    return 0;
}

static inline int
coro_schedule(struct coro *c) {
    return co_event_schedule_connect(&c->loop->sched, CO_U128(0), co_bind(&coro_run, c));
}

static inline int
coro_init(struct coro *c, struct co_loop *loop, size_t size, struct co_closure body) {
    *c = (struct coro){.refcount = 1, .loop = loop, .body = body};
#if COROUTINE_XCHG_RSP
    c->regs[0] = 0;
    c->regs[1] = c->stack + size - sizeof(struct coro) - 8;
    c->regs[2] = &coro_inner_code;
    c->regs[3] = c;
    memset(c->regs[1], 0, 8);
#else
    getcontext(&c->inner);
    c->inner.uc_stack.ss_sp = c->stack;
    c->inner.uc_stack.ss_size = size - sizeof(struct coro);
    makecontext(&c->inner, (void(*)(void))&coro_inner_code, 1, c);
#endif
    return coro_schedule(c);
}

static inline void
coro_fini(struct coro *c) {
    co_event_vec_fini(&c->done);
}

static inline struct coro *
coro_incref(struct coro *c) {
    c->refcount++;
    return c;
}

static inline int
coro_decref(struct coro *c) {
    if (c == NULL)
        return -1;
    if (--c->refcount == 0) {
        coro_fini(c);
        free(c);
    }
    return 0;
}

#define __coro_pause(type, ...) {                                           \
    if (co_event_##type##_connect(__VA_ARGS__, co_bind(&coro_schedule, c))) \
        return -1;                                                          \
    coro_leave(c);                                                          \
    return 0;                                                               \
}

static inline int
coro_wait(struct coro *c, struct co_event_vec *ev) __coro_pause(vec, ev);

static inline int
coro_iowait(struct coro *c, int fd, int write) __coro_pause(fd, &c->loop->io, fd, write);

static inline int
coro_sleep(struct coro *c, struct co_nsec delay) __coro_pause(schedule, &c->loop->sched, delay);

static inline int
coro_join(struct coro *c) {
    int ret = c->loop ? coro_wait(coro_current, &c->done) : 0;
    coro_decref(c);
    return ret;
}

static inline int
coro_dealloc(struct coro *c) {
    if (--c->loop->active == 0)
        co_loop_stop(c->loop);
    return coro_decref(c);
}

static inline struct coro *
coro_alloc(struct co_loop *loop, size_t size, struct co_closure body) {
    size &= ~(size_t)15;
    if (size < sizeof(struct coro))
        size = COROUTINE_DEFAULT_STACK;
    struct coro *c = (struct coro *)malloc(size);
    if (c == NULL)
        return NULL;
    if (coro_init(c, loop, size, body))
        return free(c), NULL;
    if (co_event_vec_connect(&c->done, co_bind(&coro_dealloc, c)))
        return coro_decref(c), NULL;
    loop->active++;
    return coro_incref(c);
}

#define coro(f, arg) coro_alloc(coro_current->loop, 0, co_bind(f, arg))

static inline int
coro_main(struct co_closure body) {
    struct co_loop loop = {};
    int err = co_loop_init(&loop)
           || coro_decref(coro_alloc(&loop, 0, body))
           || co_loop_run(&loop);
    return co_loop_fini(&loop) || err ? -1 : 0;
}
