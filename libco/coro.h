#pragma once
#include "evloop.h"

#include <stdio.h>
#include <assert.h>
#include <string.h>

#if !defined(COROUTINE_XCHG_RSP) && defined(__linux__) && defined(__x86_64__)
#define COROUTINE_XCHG_RSP 1
#endif

#ifndef COROUTINE_DEFAULT_STACK
#define COROUTINE_DEFAULT_STACK 65536
#endif

#if !COROUTINE_XCHG_RSP
#include <ucontext.h>
#endif

struct coro;

struct co_context
{
#if COROUTINE_XCHG_RSP
    void *regs[4];
#else
    ucontext_t inner;
    ucontext_t outer;
#endif
};

struct co_coro_loop
{
    struct co_loop base;
    struct coro *active;
};

struct coro
{
    int refcount;
    struct co_coro_loop *loop;
    struct co_event_vec done;
    struct co_closure body;
    struct co_context context;
    struct coro *prev;
    struct coro *next;
    char stack[];
};

extern _Thread_local struct coro * volatile coro_current;

static inline void
co_context_enter(struct co_context *ctx) {
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
      : "a"(ctx->regs)
      : "rbx", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
        "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9",
        "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "cc" // not "memory" (so don't read `regs`)
    );
#else
    swapcontext(&ctx->outer, &ctx->inner);
#endif
}

static inline void
co_context_leave(struct co_context *ctx) {
#if COROUTINE_XCHG_RSP
    co_context_enter(ctx);
#else
    swapcontext(&ctx->inner, &ctx->outer);
#endif
}

static inline void
co_context_init(struct co_context *ctx, char *stack, size_t size, struct co_closure body) {
#if COROUTINE_XCHG_RSP
    ctx->regs[0] = 0;
    ctx->regs[1] = stack + size - 8;
    ctx->regs[2] = body.function;
    ctx->regs[3] = body.data;
    memset((char*)ctx + size - 8, 0, 8);
#else
    getcontext(&ctx->inner);
    ctx->inner.uc_stack.ss_sp = stack;
    ctx->inner.uc_stack.ss_size = size;
    makecontext(&ctx->inner, (void(*)())body.function, 1, body.data);
#endif
}

static inline int
co_coro_loop_init(struct co_coro_loop *loop) {
    *loop = (struct co_coro_loop){};
    return co_loop_init(&loop->base);
}

static inline int
co_coro_loop_fini(struct co_coro_loop *loop) {
    assert(loop->active == NULL);
    return co_loop_fini(&loop->base);
}

static inline int
coro_run(struct coro *c) {
    struct coro* preempted = coro_current;
    coro_current = c;
    co_context_enter(&c->context);
    coro_current = preempted;
    return 0;
}

static inline int
coro_schedule(struct coro *c) {
    struct co_event_scheduler now = co_event_schedule_after(&c->loop->base.sched, CO_U128(0));
    return co_event_scheduler_connect(&now, co_bind(&coro_run, c));
}

#define __coro_pausable_with(t)                                              \
    static inline int                                                        \
    coro_pause_##t(struct coro *c, struct co_event_##t *ev) {                \
        if (co_event_##t##_connect(ev, co_bind(&coro_schedule, c))) \
            return -1;                                                       \
        co_context_leave(&(c)->context);                                     \
        return 0;                                                            \
    }
__coro_pausable_with(fd);
__coro_pausable_with(vec);
__coro_pausable_with(scheduler);
#undef __coro_pausable_with

static inline void
coro_join(struct coro *c) {
    if (c->loop)
        coro_pause_vec(coro_current, &c->done);
}

static inline void
coro_fini(struct coro *c) {
    co_event_vec_fini(&c->done);
}

static inline void __attribute__((noreturn))
coro_inner_code(struct coro *c) {
    if (c->body.function(c->body.data))
        goto terminate;
    struct co_event_scheduler now = co_event_schedule_after(&c->loop->base.sched, CO_U128(0));
    for (size_t i = 0; i < c->done.slots.size; i++)
        if (co_event_scheduler_connect(&now, c->done.slots.data[i]))
            goto terminate;
    co_vec_fini(&c->done.slots);
    co_context_leave(&c->context);
terminate:
    assert("coroutine body must not fail" && 0);
    abort();
}

static inline int
coro_init(struct coro *c, struct co_coro_loop *loop, size_t size, struct co_closure body) {
    *c = (struct coro){.refcount = 1, .loop = loop, .body = body};
    co_context_init(&c->context, c->stack, size - sizeof(struct coro), co_bind(&coro_inner_code, c));
    return coro_schedule(c);
}

static inline struct coro *
coro_incref(struct coro *c) {
    c->refcount++;
    return c;
}

static inline void
coro_decref(struct coro *c) {
    if (--c->refcount)
        return;
    coro_fini(c);
    free(c);
}

static inline int
coro_deschedule(struct coro *c) {
    if (c->prev)
        c->prev->next = c->next;
    else
        c->loop->active = c->next;
    if (c->next)
        c->next->prev = c->prev;
    c->prev = c->next = NULL;
    coro_decref(c);
    return 0;
}

static inline struct coro *
coro_alloc(struct co_coro_loop *loop, size_t size, struct co_closure body) {
    size &= ~(size_t)15;
    if (size < sizeof(struct coro))
        size = COROUTINE_DEFAULT_STACK;
    struct coro *c = (struct coro *)malloc(size);
    if (c == NULL)
        return NULL;
    if (coro_init(c, loop, size, body))
        return free(c), NULL;
    if (co_event_vec_connect(&c->done, co_bind(&coro_deschedule, c)))
        return coro_decref(c), NULL;
    if ((c->next = loop->active))
        c->next->prev = c;
    loop->active = coro_incref(c);
    return c;
}

static inline struct coro *
coro_spawn(struct co_closure body, size_t size) {
    return coro_alloc(coro_current->loop, size, body);
}

#define coro(f, arg) coro_spawn(co_bind(f, arg), 0)

static inline int
coro_main(struct co_closure body) {
    struct co_coro_loop loop;
    if (co_coro_loop_init(&loop))
        goto err_init;
    struct coro *c = coro_alloc(&loop, 0, body);
    if (c == NULL)
        goto err_coro;
    coro_decref(c);
    while (loop.active) {
        if (co_event_vec_connect(&loop.active->done, co_bind(&co_loop_stop, &loop.base)))
            goto err_coro;
        if (co_loop_run(&loop.base))
            goto err_coro;
    }
    return 0;

err_coro:
    co_coro_loop_fini(&loop);
err_init:
    return -1;
}
