#pragma once
#if !defined(COROUTINE_X86_64_SYSV_CTX) && defined(__linux__) && defined(__x86_64__)
#define COROUTINE_X86_64_SYSV_CTX 1
#endif

#include "evloop.h"

#include <stdio.h>
#include <string.h>
#if !COROUTINE_X86_64_SYSV_CTX
#include <ucontext.h>
#endif

struct coro;

struct co_context
{
    struct co_callback body;
#if COROUTINE_X86_64_SYSV_CTX
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
    struct co_callback body;
    struct co_context context;
    struct coro *prev;
    struct coro *next;
    char stack[];
};

extern _Thread_local struct coro * volatile coro_current;

static inline void
co_context_enter(struct co_context *ctx) {
#if COROUTINE_X86_64_SYSV_CTX
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
#if COROUTINE_X86_64_SYSV_CTX
    co_context_enter(ctx);
#else
    swapcontext(&ctx->inner, &ctx->outer);
#endif
}

static inline void __attribute__((noreturn))
co_context_inner_code(struct co_context *ctx) {
    ctx->body.function(ctx->body.data);
    co_context_leave(ctx);
    abort();
}

static inline void
co_context_init(struct co_context *ctx, char *stack, size_t size, struct co_callback body) {
    ctx->body = body;
#if COROUTINE_X86_64_SYSV_CTX
    ctx->regs[0] = 0;
    ctx->regs[1] = stack + size - 8;
    ctx->regs[2] = &co_context_inner_code;
    ctx->regs[3] = ctx;
    memset((char*)ctx + size - 8, 0, 8);
#else
    getcontext(&ctx->inner);
    ctx->inner.uc_stack.ss_sp = stack;
    ctx->inner.uc_stack.ss_size = size;
    makecontext(&ctx->inner, (void(*)())&co_context_inner_code, 1, ctx);
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
    struct co_event_scheduler now = co_event_schedule_after(&c->loop->base.sched, co_u128_value(0));
    return co_event_schedule_connect(&now, co_callback_bind(&coro_run, c));
}

static inline int
coro_pause(struct coro *c, struct co_event *ev) {
 // int interruptible = ev->disconnect != NULL;
    if (ev->connect(ev, co_callback_bind(&coro_schedule, c)))
        return -1;
    co_context_leave(&c->context);
    return 0;
}

static inline void
coro_join(struct coro *c) {
    if (c->loop)
        coro_pause(coro_current, &c->done.as_event);
}

static inline void
coro_fini(struct coro *c) {
    co_event_vec_fini(&c->done);
}

static inline int
coro_inner_code(struct coro *c) {
    if (c->body.function(c->body.data))
        return -1;
    struct co_event_scheduler now = co_event_schedule_after(&c->loop->base.sched, co_u128_value(0));
    return co_event_vec_move(&c->done, &now.as_event);
}

static inline int
coro_init(struct coro *c, struct co_coro_loop *loop, size_t size, struct co_callback body) {
    *c = (struct coro){.refcount = 1, .loop = loop, .body = body};
    co_event_vec_init(&c->done);
    co_context_init(&c->context, c->stack, size - sizeof(struct coro), co_callback_bind(&coro_inner_code, c));
    if (coro_schedule(c)) {
        coro_fini(c);
        return -1;
    }
    return 0;
}

static inline struct coro *
coro_incref(struct coro *c) {
    c->refcount++;
    return c;
}

static inline void
coro_decref(struct coro *c) {
    if (--c->refcount == 0) {
        coro_fini(c);
        free(c);
    }
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
coro_alloc(struct co_coro_loop *loop, size_t size, struct co_callback body) {
    if (size == 0)
        size = 65536;
    size &= ~(size_t)15;
    struct coro *c = (struct coro *)malloc(size);
    if (c == NULL)
        goto err_alloc;
    if (coro_init(c, loop, size, body))
        goto err_init;
    if (co_event_vec_connect(&c->done, co_callback_bind(&coro_deschedule, c)))
        goto err_connect;
    if ((c->next = loop->active))
        c->next->prev = c;
    loop->active = c;
    return coro_incref(c);

err_connect:
    coro_fini(c);
err_init:
    free(c);
err_alloc:
    return NULL;
}

static inline struct coro *
coro_spawn(struct co_callback body, size_t size) {
    return coro_alloc(coro_current->loop, size, body);
}

static inline int
coro_main(struct co_callback body) {
    struct co_coro_loop loop;
    if (co_coro_loop_init(&loop))
        goto err_init;
    struct coro *c = coro_alloc(&loop, 0, body);
    if (c == NULL)
        goto err_coro;
    coro_decref(c);
    while (loop.active) {
        if (co_event_vec_connect(&loop.active->done, co_callback_bind(&co_loop_stop, &loop.base)))
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
