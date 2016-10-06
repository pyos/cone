#include "cone.h"
#if !defined(CONE_EPOLL) && __linux__
#    define CONE_EPOLL 1
#endif
#if !defined(CONE_XCHG_RSP) && __linux__ && __x86_64__
#    define CONE_XCHG_RSP 1
#endif
#ifndef CONE_DEFAULT_STACK
#    define CONE_DEFAULT_STACK 65536
#endif

#include <fcntl.h>
#include <unistd.h>
#include <stdatomic.h>
#if CONE_EPOLL
#    include <sys/epoll.h>
#else
#    include <sys/select.h>
#endif
#if !CONE_XCHG_RSP
#    include <ucontext.h>
#endif

static int cone_event_emit(struct cone_closure *ev) {
    struct cone_closure cb = *ev;
    *ev = (struct cone_closure){};
    return cb.code && cb.code(cb.data);
}

static int cone_cond_connect(struct cone_cond *ev, struct cone_closure cb) {
    return mun_vec_append(ev, &cb);
}

static void cone_cond_disconnect(struct cone_cond *ev, struct cone_closure cb) {
    for (unsigned i = 0; i < ev->size; i++)
        if (ev->data[i].code == cb.code && ev->data[i].data == cb.data)
            return (void)mun_vec_erase(ev, i, 1);
}

int cone_notify(struct cone_cond *ev) {
    while (ev->size) {
        if (cone_event_emit(&ev->data[0]))
            return mun_error_up();  // TODO not fail
        mun_vec_erase(ev, 0, 1);
    }
    return mun_ok;
}

struct cone_scheduled
{
    struct cone_closure f;
    mun_nsec time;
};

struct cone_event_schedule mun_vec(struct cone_scheduled);

static int cone_event_schedule_connect(struct cone_event_schedule *ev, mun_nsec delay, struct cone_closure cb) {
    struct cone_scheduled r = {cb, mun_u128_add(mun_nsec_monotonic(), delay)};
    size_t left = 0, right = ev->size;
    while (left != right) {
        size_t mid = (right + left) / 2;
        if (mun_u128_lt(r.time, ev->data[mid].time))
            right = mid;
        else
            left = mid + 1;
    }
    return mun_vec_insert(ev, left, &r);
}

static mun_nsec cone_event_schedule_emit(struct cone_event_schedule *ev) {
    while (ev->size) {
        mun_nsec now = mun_nsec_monotonic();
        struct cone_scheduled next = ev->data[0];
        if (mun_u128_gt(next.time, now))
            return mun_u128_sub(next.time, now);
        mun_vec_erase(ev, 0, 1);
        if (cone_event_emit(&next.f))
            return mun_error_up(), (mun_u128){};  // TODO not fail
    }
    return MUN_U128_MAX;
}

struct cone_ioclosure
{
    int fd;
    struct cone_closure fs[2];
    struct cone_ioclosure *link;
};

struct cone_event_fd
{
    int epoll;
    struct cone_ioclosure *fds[127];
};

static int cone_event_fd_init(struct cone_event_fd *set) {
#if CONE_EPOLL
    return (set->epoll = epoll_create1(0)) < 0 ? mun_error_os() : mun_ok;
#else
    set->epoll = -1;
    return mun_ok;
#endif
}

static void cone_event_fd_fini(struct cone_event_fd *set) {
    for (size_t i = 0; i < sizeof(set->fds) / sizeof(set->fds[0]); i++)
        for (struct cone_ioclosure *c; (c = set->fds[i]) != NULL; free(c))
            set->fds[i] = c->link;
    if (set->epoll >= 0)
        close(set->epoll);
}

static struct cone_ioclosure **cone_event_fd_bucket(struct cone_event_fd *set, int fd) {
    struct cone_ioclosure **b = &set->fds[fd % (sizeof(set->fds) / sizeof(set->fds[0]))];
    while (*b && (*b)->fd != fd) b = &(*b)->link;
    return b;
}

static struct cone_ioclosure *cone_event_fd_open(struct cone_event_fd *set, int fd) {
    struct cone_ioclosure **b = cone_event_fd_bucket(set, fd);
    if (*b != NULL)
        return *b;
    if ((*b = (struct cone_ioclosure *) calloc(1, sizeof(struct cone_ioclosure))) == NULL)
        return mun_error(memory, "-"), NULL;
    (*b)->fd = fd;
#if CONE_EPOLL
    struct epoll_event params = {EPOLLRDHUP|EPOLLHUP|EPOLLET|EPOLLIN|EPOLLOUT, {.ptr = *b}};
    if (epoll_ctl(set->epoll, EPOLL_CTL_ADD, fd, &params))
        mun_error_os(), free(*b), *b = NULL;
#endif
    return *b;
}

static void cone_event_fd_close(struct cone_event_fd *set, struct cone_ioclosure *ev) {
#if CONE_EPOLL
    epoll_ctl(set->epoll, EPOLL_CTL_DEL, ev->fd, NULL);
#endif
    *cone_event_fd_bucket(set, ev->fd) = ev->link;
    free(ev);
}

static int cone_event_fd_connect(struct cone_event_fd *set, int fd, int write, struct cone_closure cb) {
    struct cone_ioclosure *ev = cone_event_fd_open(set, fd);
    if (ev == NULL)
        return mun_error_up();
    if (ev->fs[write].code)
        return mun_error(assert, "two readers/writers on one file descriptor");
    ev->fs[write] = cb;
    return 0;
}

static void cone_event_fd_disconnect(struct cone_event_fd *set, int fd, int write, struct cone_closure cb) {
    struct cone_ioclosure *ev = *cone_event_fd_bucket(set, fd);
    if (ev == NULL)
        return;
    if (ev->fs[write].code != cb.code || ev->fs[write].data != cb.data)
        return;
    ev->fs[write] = (struct cone_closure){};
    if (ev->fs[!write].code == NULL)
        cone_event_fd_close(set, ev);
}

static int cone_event_fd_emit(struct cone_event_fd *set, mun_nsec timeout) {
    if (mun_u128_eq(timeout, (mun_u128){}))
        return mun_error_up();
    if (mun_u128_gt(timeout, (mun_u128){0, 60000000000ull}))
        timeout = (mun_u128){0, 60000000000ull};
#if CONE_EPOLL
    struct epoll_event evs[32];
    int got = epoll_wait(set->epoll, evs, 32, mun_u128_div(timeout, 1000000ul).L);
    if (got < 0)
        return errno == EINTR ? mun_ok : mun_error_os();
    for (size_t i = 0; i < (size_t)got; i++) {
        struct cone_ioclosure *c = (struct cone_ioclosure*)evs[i].data.ptr;
        if ((evs[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP)) && cone_event_emit(&c->fs[0]))
            return mun_error_up();  // TODO not fail
        if ((evs[i].events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) && cone_event_emit(&c->fs[1]))
            return mun_error_up();  // TODO not fail
        if (c->fs[0].code == NULL && c->fs[1].code == NULL)
            cone_event_fd_close(set, c);
    }
#else
    fd_set fds[2] = {};
    int max_fd = 0;
    for (size_t i = 0; i < sizeof(set->fds) / sizeof(set->fds[0]); i++) {
        for (struct cone_ioclosure *c = set->fds[i], *next = NULL; c; c = next) {
            next = c->link;
            if (c->fs[0].code == NULL && c->fs[1].code == NULL) {
                cone_event_fd_close(set, c);
                continue;
            }
            if (max_fd <= c->fd)
                max_fd = c->fd + 1;
            for (int i = 0; i < 2; i++)
                if (c->fs[i].code)
                    FD_SET(c->fd, &fds[i]);
        }
    }
    struct timeval us = {mun_u128_div(timeout, 1000000000ull).L, timeout.L % 1000000000ull / 1000};
    if (select(max_fd, &fds[0], &fds[1], NULL, &us) < 0)
        return errno == EINTR ? mun_ok : mun_error_os();
    for (size_t i = 0; i < sizeof(set->fds) / sizeof(set->fds[0]); i++)
        for (struct cone_ioclosure *c = set->fds[i]; c; c = c->link)
            for (int i = 0; i < 2; i++)
                if (FD_ISSET(c->fd, &fds[i]) && cone_event_emit(&c->fs[i]))
                    return mun_error_up();  // TODO not fail
#endif
    return mun_ok;
}

struct cone_loop
{
    int selfpipe[2];
    volatile _Atomic unsigned active;
    volatile _Atomic _Bool pinged;
    struct cone_cond ping;
    struct cone_event_fd io;
    struct cone_event_schedule at;
};

int cone_unblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) ? mun_error_os() : mun_ok;
}

static int cone_loop_consume_ping(struct cone_loop *loop) {
    ssize_t rd = read(loop->selfpipe[0], &rd, sizeof(rd));  // never yields
    atomic_store_explicit(&loop->pinged, 0, memory_order_release);
    if (cone_event_fd_connect(&loop->io, loop->selfpipe[0], 0, cone_bind(&cone_loop_consume_ping, loop)))
        return mun_error_up();
    return cone_event_schedule_connect(&loop->at, (mun_u128){}, cone_bind(&cone_notify, &loop->ping));
}

static void cone_loop_fini(struct cone_loop *loop) {
    close(loop->selfpipe[0]);
    close(loop->selfpipe[1]);
    cone_event_fd_fini(&loop->io);
    mun_vec_fini(&loop->ping);
    mun_vec_fini(&loop->at);
}

static int cone_loop_init(struct cone_loop *loop) {
#ifdef _GNU_SOURCE
    if (pipe2(loop->selfpipe, O_NONBLOCK))
        return mun_error_os();
#else
    if (pipe(loop->selfpipe))
        return mun_error_os();
    if (cone_unblock(loop->selfpipe[0]) || cone_unblock(loop->selfpipe[1]))
        return cone_loop_fini(loop), mun_error_up();
#endif
    atomic_init(&loop->active, 0);
    atomic_init(&loop->pinged, 0);
    if (cone_event_fd_init(&loop->io) ||
        cone_event_fd_connect(&loop->io, loop->selfpipe[0], 0, cone_bind(&cone_loop_consume_ping, loop)))
        return cone_loop_fini(loop), mun_error_up();
    return mun_ok;
}

static int cone_loop_run(struct cone_loop *loop) {
    while (atomic_load_explicit(&loop->active, memory_order_acquire))
        if (cone_event_fd_emit(&loop->io, cone_event_schedule_emit(&loop->at)))
            return mun_error_up();
    return mun_ok;
}

static int cone_loop_ping(struct cone_loop *loop) {
    _Bool expect = 0;
    if (!atomic_compare_exchange_strong_explicit(&loop->pinged, &expect, 1, memory_order_acq_rel, memory_order_relaxed))
        return mun_ok;
    if (write(loop->selfpipe[1], "", 1) == 1)
        return mun_ok;
    atomic_store(&loop->pinged, 0);
    return mun_error_os();
}

static void cone_loop_inc(struct cone_loop *loop) {
    atomic_fetch_add_explicit(&loop->active, 1, memory_order_release);
}

static int cone_loop_dec(struct cone_loop *loop) {
    return atomic_fetch_sub_explicit(&loop->active, 1, memory_order_release) == 1 ? cone_loop_ping(loop) : mun_ok;
}

enum
{
    CONE_FLAG_RUNNING         = 0x01,
    CONE_FLAG_FINISHED        = 0x02,
    CONE_FLAG_FAILED          = 0x04,
    CONE_FLAG_RETHROWN        = 0x08,
    CONE_FLAG_SCHEDULED       = 0x10,
    CONE_FLAG_CANCELLED       = 0x20,
    CONE_FLAG_UNINTERRUPTIBLE = 0x40,
    CONE_FLAG_CTX_A = !CONE_XCHG_RSP << 15,
};

struct cone
{
    unsigned refcount, flags;
    struct cone_loop *loop;
    struct cone_closure body;
    struct cone_cond done;
    struct mun_error error;
#if CONE_XCHG_RSP
    void **rsp;
#else
    ucontext_t ctxa, ctxb;
#endif
    char stack[];
};

_Thread_local struct cone * volatile cone;

static int cone_switch(struct cone *c) {
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
    if ((c->flags ^= CONE_FLAG_CTX_A) & CONE_FLAG_CTX_A ? swapcontext(&c->ctxb, &c->ctxa) : swapcontext(&c->ctxa, &c->ctxb))
        return mun_error_os();
#endif
    return mun_ok;
}

static int cone_run(struct cone *c) {
    c->flags &= ~CONE_FLAG_SCHEDULED;
    c->flags |= CONE_FLAG_RUNNING;
    struct cone* preempted = cone;
    int ret = cone_switch(cone = c);
    cone = preempted;
    c->flags &= ~CONE_FLAG_RUNNING;
    return ret;
}

static int cone_schedule(struct cone *c) {
    if (!(c->flags & CONE_FLAG_SCHEDULED))
        if (cone_event_schedule_connect(&c->loop->at, (mun_u128){}, cone_bind(&cone_run, c)))
            return mun_error_up();
    c->flags |= CONE_FLAG_SCHEDULED;
    return mun_ok;
}

#define cone_pause(connect, ...) (connect(__VA_ARGS__, cone_bind(&cone_schedule, cone)) || cone_switch(cone))

int cone_wait(struct cone_cond *ev) {
    if (cone_pause(cone_cond_connect, ev))
        return mun_error_up();
    if (cone->flags & CONE_FLAG_CANCELLED) {
        cone_cond_disconnect(ev, cone_bind(&cone_schedule, cone));
        cone->flags &= ~CONE_FLAG_CANCELLED;
        errno = ECANCELED;
        return mun_error(cancelled, "cone_cancel");
    }
    return mun_ok;
}

int cone_iowait(int fd, int write) {
    if (cone_pause(cone_event_fd_connect, &cone->loop->io, fd, write))
        return mun_error_up();
    if (cone->flags & CONE_FLAG_CANCELLED) {
        cone_event_fd_disconnect(&cone->loop->io, fd, write, cone_bind(&cone_schedule, cone));
        cone->flags &= ~CONE_FLAG_CANCELLED;
        errno = ECANCELED;
        return mun_error(cancelled, "cone_cancel");
    }
    return mun_ok;
}

int cone_sleep(mun_nsec delay) {
    cone->flags |= CONE_FLAG_UNINTERRUPTIBLE;
    int err = cone_pause(cone_event_schedule_connect, &cone->loop->at, delay);
    cone->flags &= ~CONE_FLAG_UNINTERRUPTIBLE;
    if (err) return mun_error_up();

    if (cone->flags & CONE_FLAG_CANCELLED) {
        cone->flags &= ~CONE_FLAG_CANCELLED;
        errno = ECANCELED;
        return mun_error(cancelled, "cone_cancel");
    }
    return mun_ok;
}

int cone_yield(void) {
    return cone_loop_ping(cone->loop) ? -1 : cone_wait(&cone->loop->ping);
}

void cone_incref(struct cone *c) {
    c->refcount++;
}

int cone_decref(struct cone *c) {
    if (c && --c->refcount == 0) {
        if (c->flags & CONE_FLAG_FAILED && !(c->flags & CONE_FLAG_RETHROWN))
            mun_error_show("cone: uncaught", &c->error);
        mun_vec_fini(&c->done);
        free(c);
    }
    return c ? mun_ok : mun_error_up();
}

int cone_join(struct cone *c) {
    int ret = c->flags & CONE_FLAG_FINISHED ? 0 : cone_wait(&c->done);
    if (!ret && c->flags & CONE_FLAG_FAILED) {
        ret = mun_error_restore(&c->error);
        c->flags |= CONE_FLAG_RETHROWN;
    }
    cone_decref(c);
    return ret;
}

int cone_cancel(struct cone *c) {
    if (c->flags & CONE_FLAG_FINISHED)
        return mun_ok;
    c->flags |= CONE_FLAG_CANCELLED;
    if (c->flags & CONE_FLAG_RUNNING)
        return mun_error(cancelled, "self-cancel");
    return c->flags & CONE_FLAG_UNINTERRUPTIBLE ? mun_ok : cone_schedule(c);
}

static __attribute__((noreturn)) void cone_body(struct cone *c) {
    if (cone_event_emit(&c->body)) {
        c->error = *mun_last_error();
        c->flags |= CONE_FLAG_FAILED;
    }
    c->flags |= CONE_FLAG_FINISHED;
    for (size_t i = 0; i < c->done.size; i++)
        if (cone_event_schedule_connect(&c->loop->at, (mun_u128){}, c->done.data[i]))
            mun_error_show("cone: fatal", NULL), abort();
    cone_switch(c);
    abort();
}

static struct cone *cone_spawn_on(struct cone_loop *loop, size_t size, struct cone_closure body) {
    size &= ~(size_t)15;
    if (size < sizeof(struct cone))
        size = CONE_DEFAULT_STACK;
    struct cone *c = (struct cone *)malloc(size);
    if (c == NULL)
        return mun_error(memory, "-"), NULL;
    *c = (struct cone){.refcount = 1, .loop = loop, .body = body};
#if CONE_XCHG_RSP
    c->rsp = (void **)(c->stack + size - sizeof(struct cone)) - 4;
    c->rsp[0] = c;                  // %rdi
    c->rsp[1] = NULL;               // %rbp
    c->rsp[2] = (void*)&cone_body;  // %rip
    c->rsp[3] = NULL;               // return address
#else
    getcontext(&c->ctxa);
    c->ctxa.uc_stack.ss_sp = c->stack;
    c->ctxa.uc_stack.ss_size = size - sizeof(struct cone);
    makecontext(&c->ctxa, (void(*)(void))&cone_body, 1, c);
#endif
    if (cone_cond_connect(&c->done, cone_bind(&cone_loop_dec, loop))
     || cone_cond_connect(&c->done, cone_bind(&cone_decref, c))
     || cone_schedule(c)) {
        cone_decref(c);
        return mun_error_up(), NULL;
    }
    cone_loop_inc(loop);
    return cone_incref(c), c;
}

struct cone *cone_spawn(size_t size, struct cone_closure body) {
    return cone_spawn_on(cone->loop, size, body);
}

int cone_root(size_t stksz, struct cone_closure body) {
    struct cone_loop loop = {};
    if (cone_loop_init(&loop))
        return mun_error_up();
    struct cone *c = cone_spawn_on(&loop, stksz, body);
    if (c == NULL)
        return cone_loop_fini(&loop), mun_error_up();
    if (cone_loop_run(&loop))
        return cone_decref(c), cone_loop_fini(&loop), mun_error_up();
    if (cone_join(c))
        return cone_loop_fini(&loop), mun_error_up();
    cone_loop_fini(&loop);
    return mun_ok;
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
        mun_error_show("cone:main", NULL);
    return c.retcode == -1 ? 1 : c.retcode;
}
