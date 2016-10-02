#pragma once
#include "evloop.h"

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
    void *volatile *rsp;
#else
    int inside;
    ucontext_t inner;
    ucontext_t outer;
#endif
    char stack[];
};

extern _Thread_local struct coro * volatile coro_current;

static inline int
coro_switch(struct coro *c) {
#if COROUTINE_XCHG_RSP
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
    return (c->inside ^= 1) ? swapcontext(&c->outer, &c->inner) : swapcontext(&c->inner, &c->outer);
#endif
}

static inline void __attribute__((noreturn))
coro_body(struct coro *c) {
    if (co_event_emit(&c->body))
        abort();
    for (size_t i = 0; i < c->done.slots.size; i++)
        if (co_event_schedule_connect(&c->loop->sched, CO_U128(0), c->done.slots.data[i]))
            abort();
    co_event_vec_fini(&c->done);
    c->loop = NULL;
    coro_switch(c);
    abort();
}

static inline int
coro_run(struct coro *c) {
    struct coro* preempted = coro_current;
    int ret = coro_switch(coro_current = c);
    coro_current = preempted;
    return ret;
}

static inline int
coro_schedule(struct coro *c) {
    return co_event_schedule_connect(&c->loop->sched, CO_U128(0), co_bind(&coro_run, c));
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
        co_event_vec_fini(&c->done);
        free(c);
    }
    return 0;
}

#define __coro_pause(evconn, ...) {                                                     \
    struct coro *c = coro_current;                                                      \
    return !c || evconn(__VA_ARGS__, co_bind(&coro_schedule, c)) ? -1 : coro_switch(c); \
}

static inline int
coro_wait(struct co_event_vec *ev) __coro_pause(co_event_vec_connect, ev);

static inline int
coro_iowait(int fd, int write) __coro_pause(co_event_fd_connect, &coro_current->loop->io, fd, write);

static inline int
coro_sleep(struct co_nsec delay) __coro_pause(co_event_schedule_connect, &coro_current->loop->sched, delay);

static inline int
coro_join(struct coro *c) {
    int ret = c->loop ? coro_wait(&c->done) : 0;
    coro_decref(c);
    return ret;
}

static inline struct coro *
coro_spawn(struct co_loop *loop, size_t size, struct co_closure body) {
    size &= ~(size_t)15;
    if (size < sizeof(struct coro))
        size = COROUTINE_DEFAULT_STACK;
    struct coro *c = (struct coro *)malloc(size);
    if (c == NULL)
        return NULL;
    *c = (struct coro){.refcount = 1, .loop = loop, .body = body};
#if COROUTINE_XCHG_RSP
    c->rsp = (void *volatile *)(c->stack + size - sizeof(struct coro)) - 4;
    c->rsp[0] = c;                  // %rdi
    c->rsp[1] = NULL;               // %rbp
    c->rsp[2] = (void*)&coro_body;  // %rip
    c->rsp[3] = NULL;               // return address
#else
    getcontext(&c->inner);
    c->inside = 0;
    c->inner.uc_stack.ss_sp = c->stack;
    c->inner.uc_stack.ss_size = size - sizeof(struct coro);
    makecontext(&c->inner, (void(*)(void))&coro_body, 1, c);
#endif
    if (coro_schedule(c) ||
        co_event_vec_connect(&c->done, co_bind(&co_loop_dec, loop)) ||
        co_event_vec_connect(&c->done, co_bind(&coro_decref, c))) {
        coro_decref(c);
        return NULL;
    }
    co_loop_inc(loop);
    return coro_incref(c);
}

#define coro(f, arg) coro_spawn(coro_current->loop, 0, co_bind(f, arg))

static inline int
coro_main(struct co_closure body) {
    struct co_loop loop = {};
    int err = co_loop_init(&loop)
           || coro_decref(coro_spawn(&loop, 0, body))
           || co_loop_run(&loop);
    return co_loop_fini(&loop) || err ? -1 : 0;
}
