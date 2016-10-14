#include "cone.h"
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdatomic.h>
#if CONE_EPOLL
#include <sys/epoll.h>
#else
#include <sys/select.h>
#endif
#if !CONE_XCHG_RSP
#include <setjmp.h>
#ifndef SA_ONSTACK
#define SA_ONSTACK 0x8000000
#endif
#endif

static int cone_event_add(struct cone_event *ev, struct cone_closure f) {
    return mun_vec_append(ev, &f);
}

static void cone_event_del(struct cone_event *ev, struct cone_closure f) {
    unsigned i = mun_vec_find(ev, _->code == f.code && _->data == f.data);
    if (i != ev->size)
        mun_vec_erase(ev, i, 1);
}

int cone_event_emit(struct cone_event *ev) {
    for (; ev->size; mun_vec_erase(ev, 0, 1))
        if (ev->data[0].code(ev->data[0].data) MUN_RETHROW)
            return -1;
    return 0;
}

struct cone_event_schedule mun_vec(struct cone_event_at { mun_usec at; struct cone_closure f; });

static int cone_event_schedule_add(struct cone_event_schedule *ev, mun_usec at, struct cone_closure f) {
    return mun_vec_insert(ev, mun_vec_bisect(ev, at < _->at), &((struct cone_event_at){at, f}));
}

static void cone_event_schedule_del(struct cone_event_schedule *ev, mun_usec at, struct cone_closure f) {
    for (size_t i = mun_vec_bisect(ev, at < _->at); i-- && ev->data[i].at == at; )
        if (ev->data[i].f.code == f.code && ev->data[i].f.data == f.data)
            return mun_vec_erase(ev, i, 1);
}

static mun_usec cone_event_schedule_emit(struct cone_event_schedule *ev) {
    for (; ev->size; mun_vec_erase(ev, 0, 1)) {
        mun_usec now = mun_usec_monotonic();
        if (ev->data[0].at > now)
            return ev->data[0].at - now;
        if (ev->data[0].f.code(ev->data[0].f.data) MUN_RETHROW)
            return 0;
    }
    return MUN_USEC_MAX;
}

struct cone_event_fd
{
    struct cone_closure cbs[2];
};

struct cone_event_io
{
    int epoll;
    struct mun_map(int, struct cone_event_fd) fds;
};

static int cone_event_io_init(struct cone_event_io *set) {
    *set = (struct cone_event_io){.epoll = -1};
#if CONE_EPOLL
    if ((set->epoll = epoll_create1(0)) < 0 MUN_RETHROW) return -1;
#endif
    return 0;
}

static void cone_event_io_fini(struct cone_event_io *set) {
    mun_map_fini(&set->fds);
    if (set->epoll >= 0)
        close(set->epoll);
}

static int cone_event_io_add(struct cone_event_io *set, int fd, int write, struct cone_closure f) {
    mun_map_type(&set->fds) *e = mun_map_insert3(&set->fds, fd, (struct cone_event_fd){});
    if (e == NULL MUN_RETHROW)
        return -1;
#if CONE_EPOLL
    if (mun_map_was_inserted(&set->fds, e)) {
        struct epoll_event params = {EPOLLRDHUP|EPOLLHUP|EPOLLET|EPOLLIN|EPOLLOUT, {.fd = fd}};
        if (epoll_ctl(set->epoll, EPOLL_CTL_ADD, fd, &params) MUN_RETHROW_OS)
            return mun_map_erase(&set->fds, &fd), -1;
    }
#endif
    if (e->b.cbs[write].code)
        return mun_error(assert, "two readers/writers on one file descriptor");
    e->b.cbs[write] = f;
    return 0;
}

static void cone_event_io_del(struct cone_event_io *set, int fd, int write, struct cone_closure f) {
    mun_map_type(&set->fds) *e = mun_map_find(&set->fds, &fd);
    if (e == NULL || e->b.cbs[write].code != f.code || e->b.cbs[write].data != f.data)
        return;
    e->b.cbs[write] = (struct cone_closure){};
    if (e->b.cbs[!write].code == NULL) {
    #if CONE_EPOLL
        epoll_ctl(set->epoll, EPOLL_CTL_DEL, e->a, NULL);
    #endif
        mun_map_erase(&set->fds, &fd);
    }
}

static int cone_event_io_emit(struct cone_event_io *set, mun_usec timeout) {
    if (timeout == 0 MUN_RETHROW)
        return -1;
    if (timeout > 60000000ll)
        timeout = 60000000ll;
#if CONE_EPOLL
    struct epoll_event evs[32];
    int got = epoll_wait(set->epoll, evs, 32, timeout / 1000ul);
    if (got < 0)
        return errno != EINTR MUN_RETHROW_OS;
    for (int i = 0; i < got; i++) {
        mun_map_type(&set->fds) *e = mun_map_find(&set->fds, &evs[i].data.fd);
        if (evs[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP))
            if (e->b.cbs[0].code && e->b.cbs[0].code(e->b.cbs[0].data) MUN_RETHROW)
                return -1;
        if (evs[i].events & (EPOLLOUT | EPOLLERR | EPOLLHUP))
            if (e->b.cbs[1].code && e->b.cbs[1].code(e->b.cbs[1].data) MUN_RETHROW)
                return -1;
    }
#else
    fd_set fds[2] = {};
    int max_fd = 0;
    for mun_vec_iter(&set->fds.values, e) {
        if (max_fd <= e->a)
            max_fd = e->a + 1;
        for (int i = 0; i < 2; i++)
            if (e->b.cbs[i].code)
                FD_SET(e->a, &fds[i]);
    }
    struct timeval us = {timeout / 1000000ull, timeout % 1000000ull};
    if (select(max_fd, &fds[0], &fds[1], NULL, &us) < 0)
        return errno != EINTR MUN_RETHROW_OS;
    for mun_vec_iter(&set->fds.values, e)
        for (int i = 0; i < 2; i++)
            if (FD_ISSET(e->a, &fds[i]) && e->b.cbs[i].code && e->b.cbs[i].code(e->b.cbs[i].data) MUN_RETHROW)
                return -1;
#endif
    return 0;
}

struct cone_loop
{
    int selfpipe[2];
    volatile _Atomic unsigned active;
    volatile _Atomic _Bool pinged;
    struct cone_event ping;
    struct cone_event_io io;
    struct cone_event_schedule at;
};

int cone_unblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) MUN_RETHROW_OS;
}

static int cone_loop_consume_ping(struct cone_loop *loop) {
    ssize_t rd = read(loop->selfpipe[0], &rd, sizeof(rd));  // never yields
    atomic_store_explicit(&loop->pinged, 0, memory_order_release);
    return cone_event_schedule_add(&loop->at, mun_usec_monotonic(), cone_bind(&cone_event_emit, &loop->ping)) MUN_RETHROW;
}

static void cone_loop_fini(struct cone_loop *loop) {
    close(loop->selfpipe[0]);
    close(loop->selfpipe[1]);
    cone_event_io_fini(&loop->io);
    mun_vec_fini(&loop->ping);
    mun_vec_fini(&loop->at);
}

static int cone_loop_init(struct cone_loop *loop) {
    if (pipe(loop->selfpipe) MUN_RETHROW_OS)
        return -1;
    if (cone_unblock(loop->selfpipe[0]) || cone_unblock(loop->selfpipe[1]) MUN_RETHROW)
        return cone_loop_fini(loop), -1;
    atomic_init(&loop->active, 0);
    atomic_init(&loop->pinged, 0);
    if (cone_event_io_init(&loop->io) ||
        cone_event_io_add(&loop->io, loop->selfpipe[0], 0, cone_bind(&cone_loop_consume_ping, loop)) MUN_RETHROW)
        return cone_loop_fini(loop), -1;
    return 0;
}

static int cone_loop_run(struct cone_loop *loop) {
    while (atomic_load_explicit(&loop->active, memory_order_acquire))
        if (cone_event_io_emit(&loop->io, cone_event_schedule_emit(&loop->at)) MUN_RETHROW)
            return -1;
    return 0;
}

static int cone_loop_ping(struct cone_loop *loop) {
    _Bool expect = 0;
    if (atomic_compare_exchange_strong(&loop->pinged, &expect, 1))
        if (write(loop->selfpipe[1], "", 1) != 1 MUN_RETHROW_OS)
            return atomic_store(&loop->pinged, 0), -1;
    return 0;
}

static void cone_loop_inc(struct cone_loop *loop) {
    atomic_fetch_add_explicit(&loop->active, 1, memory_order_release);
}

static int cone_loop_dec(struct cone_loop *loop) {
    return atomic_fetch_sub_explicit(&loop->active, 1, memory_order_release) == 1 ? cone_loop_ping(loop) : 0;
}

enum
{
    CONE_FLAG_SCHEDULED = 0x01,
    CONE_FLAG_RUNNING   = 0x02,
    CONE_FLAG_FINISHED  = 0x04,
    CONE_FLAG_FAILED    = 0x08,
    CONE_FLAG_CANCELLED = 0x10,
    CONE_FLAG_RETHROWN  = 0x20,
};

struct cone
{
    unsigned refcount, flags;
    struct cone_loop *loop;
    struct cone_closure body;
    struct cone_event done;
    struct mun_error error;
#if CONE_XCHG_RSP
    void **rsp;
#else
    jmp_buf ctx[2];
#endif
    char stack[];
};

_Thread_local struct cone * volatile cone;

static int cone_switch(struct cone *c) {
    c->flags ^= CONE_FLAG_RUNNING;
#if CONE_XCHG_RSP
    __asm__(" jmp  %=0f       \n"
        "%=1: push %%rbp      \n"
        "     push %%rdi      \n"
        "     mov  %%rsp, (%0)\n"
        "     mov  %1, %%rsp  \n"
        "     pop  %%rdi      \n"
        "     pop  %%rbp      \n"
        "     ret             \n"
        "%=0: call %=1b       \n" :
      : "a"(&c->rsp), "c"(c->rsp)
      : "rbx", "rdx", "rsi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "cc",
        "xmm0",  "xmm1",  "xmm2",  "xmm3",  "xmm4",  "xmm5",  "xmm6",  "xmm7",
        "xmm8",  "xmm9",  "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
#else
    if (!setjmp(c->ctx[!!(c->flags & CONE_FLAG_RUNNING)]))
        longjmp(c->ctx[!(c->flags & CONE_FLAG_RUNNING)], 1);
#endif
    return 0;
}

static int cone_run(struct cone *c) {
    c->flags &= ~CONE_FLAG_SCHEDULED;
    struct cone* prev = cone;
    int ret = cone_switch(cone = c);
    cone = prev;
    return ret;
}

static int cone_schedule(struct cone *c) {
    if (!(c->flags & CONE_FLAG_SCHEDULED))
        if (cone_event_schedule_add(&c->loop->at, mun_usec_monotonic(), cone_bind(&cone_run, c)) MUN_RETHROW)
            return -1;
    c->flags |= CONE_FLAG_SCHEDULED;
    return 0;
}

#define cone_pause(always_unsub, ev_add, ev_del, ...) do {                                     \
    if (ev_add(__VA_ARGS__, cone_bind(&cone_schedule, cone)) || cone_switch(cone) MUN_RETHROW) \
        return -1;                                                                             \
    if (always_unsub || cone->flags & CONE_FLAG_CANCELLED)                                     \
        ev_del(__VA_ARGS__, cone_bind(&cone_schedule, cone));                                  \
    if (!(cone->flags & CONE_FLAG_CANCELLED))                                                  \
        return 0;                                                                              \
    cone->flags &= ~CONE_FLAG_CANCELLED;                                                       \
    return cone_cancel(cone);                                                                  \
} while (0)

int cone_wait(struct cone_event *ev) {
    cone_pause(0, cone_event_add, cone_event_del, ev);
}

int cone_iowait(int fd, int write) {
    cone_pause(1, cone_event_io_add, cone_event_io_del, &cone->loop->io, fd, write);
}

int cone_sleep(mun_usec delay) {
    mun_usec at = mun_usec_monotonic() + delay;
    cone_pause(0, cone_event_schedule_add, cone_event_schedule_del, &cone->loop->at, at);
}

int cone_yield(void) {
    return cone_loop_ping(cone->loop) || cone_wait(&cone->loop->ping) MUN_RETHROW;
}

void cone_incref(struct cone *c) {
    c->refcount++;
}

int cone_decref(struct cone *c) {
    if (c && --c->refcount == 0) {
        if ((c->flags & (CONE_FLAG_FAILED | CONE_FLAG_RETHROWN)) == CONE_FLAG_FAILED)
            if (c->error.code != mun_errno_cancelled)
                mun_error_show("cone destroyed with", &c->error);
        mun_vec_fini(&c->done);
        free(c);
    }
    return !c MUN_RETHROW;
}

int cone_join(struct cone *c) {
    int ret = !(c->flags & CONE_FLAG_FINISHED) && cone_wait(&c->done) MUN_RETHROW;
    if (!ret && c->flags & CONE_FLAG_FAILED) {
        *mun_last_error() = c->error;
        c->flags |= CONE_FLAG_RETHROWN;
        ret = -1;
    }
    cone_decref(c);
    return ret;
}

int cone_cancel(struct cone *c) {
    if (c->flags & CONE_FLAG_RUNNING)
        return errno = ECANCELED, mun_error(cancelled, "self-cancel");
    if (c->flags & CONE_FLAG_FINISHED)
        return 0;
    c->flags |= CONE_FLAG_CANCELLED;
    return cone_schedule(c) MUN_RETHROW;
}

static __attribute__((noreturn)) void cone_body(struct cone *c) {
    if (c->flags & CONE_FLAG_CANCELLED ? cone_cancel(c) : c->body.code(c->body.data)) {
        c->error = *mun_last_error();
        c->flags |= CONE_FLAG_FAILED;
    }
    c->flags |= CONE_FLAG_FINISHED;
    for mun_vec_iter(&c->done, cb)
        if (cone_event_schedule_add(&c->loop->at, mun_usec_monotonic(), *cb))
            mun_error_show("cone fatal", NULL), abort();
    cone_loop_dec(c->loop);
    cone_switch(c);
    abort();
}

#if !CONE_XCHG_RSP
static _Thread_local struct cone *volatile cone_sigctx;
static void cone_sigstackswitch() {
    struct cone *c = cone_sigctx;
    if (setjmp(c->ctx[0]))
        cone_body(c);
}
#endif

static struct cone *cone_spawn_on(struct cone_loop *loop, size_t size, struct cone_closure body) {
    size &= ~(size_t)15;
    if (size < sizeof(struct cone) + MINSIGSTKSZ)
        size = CONE_DEFAULT_STACK;
    struct cone *c = (struct cone *)malloc(size);
    if (c == NULL)
        return mun_error(memory, "no space for stack"), NULL;
    c->refcount = 1;
    c->flags = 0;
    c->loop = loop;
    c->body = body;
    c->done = (struct cone_event){};
#if CONE_XCHG_RSP
    c->rsp = (void **)(c->stack + size - sizeof(struct cone)) - 4;
    c->rsp[0] = c;                  // %rdi
    c->rsp[1] = NULL;               // %rbp
    c->rsp[2] = (void*)&cone_body;  // %rip
    c->rsp[3] = NULL;               // return address
#else
    stack_t old_stk, new_stk = {.ss_sp = c->stack, .ss_size = size - sizeof(struct cone)};
    struct sigaction old_act, new_act;
    new_act.sa_handler = &cone_sigstackswitch;
    new_act.sa_flags = SA_ONSTACK;
    sigemptyset(&new_act.sa_mask);
    sigaltstack(&new_stk, &old_stk);
    sigaction(SIGUSR1, &new_act, &old_act);
    cone_sigctx = c;
    raise(SIGUSR1);
    sigaction(SIGUSR1, &old_act, NULL);
    sigaltstack(&old_stk, NULL);
#endif
    if (cone_event_add(&c->done, cone_bind(&cone_decref, c)) || cone_schedule(c) MUN_RETHROW)
        return cone_decref(c), NULL;
    return cone_loop_inc(loop), cone_incref(c), c;
}

struct cone *cone_spawn(size_t size, struct cone_closure body) {
    return cone_spawn_on(cone->loop, size, body);
}

int cone_root(size_t stksz, struct cone_closure body) {
    struct cone_loop loop = {};
    if (cone_loop_init(&loop) MUN_RETHROW)
        return -1;
    struct cone *c = cone_spawn_on(&loop, stksz, body);
    if (c == NULL || cone_loop_run(&loop) MUN_RETHROW)
        // TODO somehow convey the fact that we're absolutely screwed if cone_loop_run fails
        return cone_decref(c), cone_loop_fini(&loop), -1;
    return cone_loop_fini(&loop), cone_join(c) MUN_RETHROW;
}

struct cone_main
{
    int retcode;
    int argc;
    const char **argv;
};

extern int comain(int argc, const char **argv);

static int cone_main(struct cone_main *c) {
    c->retcode = comain(c->argc, c->argv);
    return 0;
}

extern int main(int argc, const char **argv) {
    struct cone_main c = {1, argc, argv};
    if (cone_root(0, cone_bind(&cone_main, &c)) || c.retcode == -1)
        return mun_error_show("comain exited with", NULL), 1;
    return c.retcode;
}
