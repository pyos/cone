#pragma once
#include "ev-loop.h"

#if !defined(CONE_XCHG_RSP) && __linux__ && __x86_64__
#define CONE_XCHG_RSP 1
#endif

#ifndef CONE_DEFAULT_STACK
#define CONE_DEFAULT_STACK 65536
#endif

#if !CONE_XCHG_RSP
#include <ucontext.h>
#endif

struct cone
{
    int refcount;
    struct cone_loop *loop;
    struct cone_closure body;
    struct cone_event_vec done;
#if CONE_XCHG_RSP
    void **rsp;
#else
    ucontext_t inner;
    ucontext_t outer;
    bool inside;
#endif
    char stack[];
};

extern _Thread_local struct cone * volatile cone;

static inline int
cone_switch(struct cone *c) {
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
    return (c->inside ^= 1) ? swapcontext(&c->outer, &c->inner) : swapcontext(&c->inner, &c->outer);
#endif
}

static inline void __attribute__((noreturn))
cone_body(struct cone *c) {
    if (cone_event_emit(&c->body))
        abort();
    for (size_t i = 0; i < c->done.slots.size; i++)
        if (cone_event_schedule_connect(&c->loop->at, CONE_U128(0), c->done.slots.data[i]))
            abort();
    c->loop = NULL;
    cone_switch(c);
    abort();
}

static inline int
cone_run(struct cone *c) {
    struct cone* preempted = cone;
    int ret = cone_switch(cone = c);
    cone = preempted;
    return ret;
}

static inline int
cone_schedule(struct cone *c) {
    return cone_event_schedule_connect(&c->loop->at, CONE_U128(0), cone_bind(&cone_run, c));
}

static inline struct cone *
cone_incref(struct cone *c) {
    c->refcount++;
    return c;
}

static inline int
cone_decref(struct cone *c) {
    if (c && --c->refcount == 0) {
        cone_event_vec_fini(&c->done);
        free(c);
    }
    return c ? 0 : -1;
}

#define __cone_pause(evconn, ...) {                                                       \
    struct cone *c = cone;                                                                \
    return !c || evconn(__VA_ARGS__, cone_bind(&cone_schedule, c)) ? -1 : cone_switch(c); \
}

static inline int
cone_wait(struct cone_event_vec *ev) __cone_pause(cone_event_vec_connect, ev);

static inline int
cone_iowait(int fd, int write) __cone_pause(cone_event_fd_connect, &cone->loop->io, fd, write);

static inline int
cone_sleep(struct cone_nsec delay) __cone_pause(cone_event_schedule_connect, &cone->loop->at, delay);

static inline int
cone_join(struct cone *c) {
    int ret = c->loop ? cone_wait(&c->done) : 0;
    cone_decref(c);
    return ret;
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
cone_main(struct cone_closure body) {
    struct cone_loop loop = {};
    int err = cone_loop_init(&loop)
           || cone_decref(cone_spawn(&loop, 0, body))
           || cone_loop_run(&loop);
    return cone_loop_fini(&loop) || err ? -1 : 0;
}
