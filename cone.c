#include "cone.h"
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdatomic.h>

#if !defined(CONE_EVNOTIFIER) && __linux__
#define CONE_EVNOTIFIER 1  // epoll
#elif !defined(CONE_EVNOTIFIER) && __APPLE__
#define CONE_EVNOTIFIER 2  // kqueue
#endif

#if !((__linux__ || __APPLE__) && __x86_64__)
_Static_assert(0, "stack switching is only supported on x86-64 UNIX");
#endif

#if CONE_EVNOTIFIER == 1
#include <sys/epoll.h>
#elif CONE_EVNOTIFIER == 2
#include <sys/event.h>
#else
#include <sys/select.h>
#endif

int cone_unblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) MUN_RETHROW_OS;
}

struct cone_event_at {
    mun_usec at;
    struct cone_closure f;
};

struct cone_event_schedule {
    struct mun_vec(struct cone_event_at) cs;
};

static void cone_event_schedule_fini(struct cone_event_schedule *ev) {
    mun_vec_fini(&ev->cs);
}

static int cone_event_schedule_add(struct cone_event_schedule *ev, mun_usec at, struct cone_closure f) mun_throws(memory) {
    return mun_vec_insert(&ev->cs, mun_vec_bisect(&ev->cs, at < _->at), &((struct cone_event_at){at, f}));
}

static void cone_event_schedule_del(struct cone_event_schedule *ev, mun_usec at, struct cone_closure f) {
    for (size_t i = mun_vec_bisect(&ev->cs, at < _->at); i-- && ev->cs.data[i].at == at; )
        if (ev->cs.data[i].f.code == f.code && ev->cs.data[i].f.data == f.data)
            return mun_vec_erase(&ev->cs, i, 1);
}

static mun_usec cone_event_schedule_emit(struct cone_event_schedule *ev) {
    while (ev->cs.size) {
        mun_usec now = mun_usec_monotonic();
        struct cone_event_at c = ev->cs.data[0];
        if (c.at > now)
            return c.at - now;
        mun_vec_erase(&ev->cs, 0, 1);
        if (c.f.code(c.f.data) MUN_RETHROW)
            return -1;
    }
    return MUN_USEC_MAX;
}

struct cone_event_fd {
    int fd;
    struct cone_closure cbs[2];
    struct cone_event_fd *link;
};

struct cone_event_io {
    int poller;
    int selfpipe[2];
    cone_atom pinged;
    struct cone_event ping;
    struct cone_event_fd *fds[127];
};

static void cone_event_io_fini(struct cone_event_io *set) {
    mun_vec_fini(&set->ping);
    for (size_t i = 0; i < sizeof(set->fds) / sizeof(*set->fds); i++)
        for (struct cone_event_fd *c; (c = set->fds[i]) != NULL; free(c))
            set->fds[i] = c->link;
    if (set->poller >= 0)
        close(set->poller);
    if (set->selfpipe[0] >= 0)
        close(set->selfpipe[0]), close(set->selfpipe[1]);
}

static void cone_event_io_ping(struct cone_event_io *set) {
    if (atomic_compare_exchange_strong(&set->pinged, &(unsigned){0}, 1))
        write(set->selfpipe[1], "", 1);
}

static int cone_event_io_on_ping(struct cone_event_io *set) {
    read(set->selfpipe[0], &(char[32]){}, 32);  // never yields
    atomic_store_explicit(&set->pinged, 0, memory_order_release);
    return cone_wake(&set->ping, (size_t)-1) MUN_RETHROW;
}

static struct cone_event_fd **cone_event_io_bucket(struct cone_event_io *set, int fd) {
    struct cone_event_fd **b = &set->fds[fd % (sizeof(set->fds) / sizeof(set->fds[0]))];
    while (*b && (*b)->fd != fd) b = &(*b)->link;
    return b;
}

#if CONE_EVNOTIFIER
static int cone_event_io_add_native(struct cone_event_io *set, int fd, int first, int write, void *data) {
    #if CONE_EVNOTIFIER == 1
        int flags = !first ? EPOLLIN|EPOLLOUT : write ? EPOLLOUT : EPOLLIN;
        struct epoll_event params = {EPOLLRDHUP|EPOLLHUP|flags, {.ptr = data}};
        return epoll_ctl((set)->poller, EPOLL_CTL_ADD, fd, &params) MUN_RETHROW_OS;
    #elif CONE_EVNOTIFIER == 2
        (void)first;
        struct kevent ev = {fd, write ? EVFILT_WRITE : EVFILT_READ, EV_ADD, 0, 0, data};
        return kevent(set->poller, &ev, 1, NULL, 0, NULL) MUN_RETHROW_OS;
    #endif
}

static int cone_event_io_del_native(struct cone_event_io *set, int fd, int last, int write, void *data) {
    #if CONE_EVNOTIFIER == 1
        if (!last)
            return cone_event_io_add_native(set, fd, 1, !write, data);
        return epoll_ctl(set->poller, EPOLL_CTL_DEL, fd, NULL) MUN_RETHROW_OS;
    #elif CONE_EVNOTIFIER == 2
        (void)last;
        struct kevent ev = {fd, write ? EVFILT_WRITE : EVFILT_READ, EV_DELETE, 0, 0, data};
        return kevent(set->poller, &ev, 1, NULL, 0, NULL) MUN_RETHROW_OS;
    #endif
}
#else
#define cone_event_io_add_native(...) 0
#define cone_event_io_del_native(...) 0
#endif

static int cone_event_io_add(struct cone_event_io *set, int fd, int write, struct cone_closure f) mun_throws(memory, assert) {
    struct cone_event_fd **b = cone_event_io_bucket(set, fd);
    if (*b == NULL) {
        if ((*b = malloc(sizeof(struct cone_event_fd))) == NULL)
            return mun_error(memory, "-");
        **b = (struct cone_event_fd){.fd = fd};
        if (cone_event_io_add_native(set, fd, 1, write, *b))
            return free(*b), *b = NULL, -1;
    } else {
        if ((*b)->cbs[write].code)
            return mun_error(assert, "two readers/writers on one file descriptor");
        if (cone_event_io_add_native(set, fd, 0, write, *b))
            return -1;
    }
    (*b)->cbs[write] = f;
    return 0;
}

static void cone_event_io_del(struct cone_event_io *set, int fd, int write, struct cone_closure f) {
    struct cone_event_fd **b = cone_event_io_bucket(set, fd), *e = *b;
    if (e == NULL || e->cbs[write].code != f.code || e->cbs[write].data != f.data)
        return;
    e->cbs[write] = (struct cone_closure){};
    if (e->cbs[!write].code == NULL) {
        mun_assert(!cone_event_io_del_native(set, fd, 1, write, e));
        *b = e->link;
        free(e);
    } else
        mun_assert(!cone_event_io_del_native(set, fd, 0, write, e));
}

static int cone_event_io_init(struct cone_event_io *set) mun_throws(memory) {
    set->selfpipe[0] = set->selfpipe[1] = set->poller = -1;
    #if CONE_EVNOTIFIER == 1
        if ((set->poller = epoll_create1(0)) < 0 MUN_RETHROW_OS)
            return -1;
    #elif CONE_EVNOTIFIER == 2
        if ((set->poller = kqueue()) < 0 MUN_RETHROW_OS)
            return -1;
    #endif
    if (pipe(set->selfpipe) || cone_event_io_add(set, set->selfpipe[0], 0, cone_bind(&cone_event_io_on_ping, set)) MUN_RETHROW_OS)
        return cone_event_io_fini(set), -1;
    return 0;
}

static int cone_event_io_emit(struct cone_event_io *set, mun_usec timeout) {
    if (timeout < 0 MUN_RETHROW)
        return -1;
    if (timeout > 60000000ll)
        timeout = 60000000ll;
    #if CONE_EVNOTIFIER == 1
        struct epoll_event evs[32];
        int got = epoll_wait(set->poller, evs, 32, timeout / 1000ul);
        if (got < 0)
            return errno != EINTR MUN_RETHROW_OS;
        for (int i = 0; i < got; i++) {
            struct cone_event_fd *e = evs[i].data.ptr;
            if (evs[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP))
                if (e->cbs[0].code && e->cbs[0].code(e->cbs[0].data) MUN_RETHROW)
                    return -1;
            if (evs[i].events & (EPOLLOUT | EPOLLERR | EPOLLHUP))
                if (e->cbs[1].code && e->cbs[1].code(e->cbs[1].data) MUN_RETHROW)
                    return -1;
        }
    #elif CONE_EVNOTIFIER == 2
        struct kevent evs[32];
        struct timespec ns = {timeout / 1000000ull, timeout % 1000000ull * 1000};
        int got = kevent(set->poller, NULL, 0, evs, 32, &ns);
        if (got < 0)
            return errno != EINTR MUN_RETHROW_OS;
        for (int i = 0; i < got; i++) {
            struct cone_event_fd *e = evs[i].udata;
            if (evs[i].filter == EVFILT_READ)
                if (e->cbs[0].code && e->cbs[0].code(e->cbs[0].data) MUN_RETHROW)
                    return -1;
            if (evs[i].filter == EVFILT_WRITE)
                if (e->cbs[1].code && e->cbs[1].code(e->cbs[1].data) MUN_RETHROW)
                    return -1;
        }
    #else
        fd_set fds[2] = {};
        int max_fd = 0;
        for (size_t i = 0; i < sizeof(set->fds) / sizeof(*set->fds); i++) {
            for (struct cone_event_fd *e = set->fds[i]; e; e = e->link) {
                if (max_fd <= e->fd)
                    max_fd = e->fd + 1;
                for (int i = 0; i < 2; i++)
                    if (e->cbs[i].code)
                        FD_SET(e->fd, &fds[i]);
            }
        }
        struct timeval us = {timeout / 1000000ull, timeout % 1000000ull};
        if (select(max_fd, &fds[0], &fds[1], NULL, &us) < 0)
            return errno != EINTR MUN_RETHROW_OS;
        for (size_t i = 0; i < sizeof(set->fds) / sizeof(*set->fds); i++)
            for (struct cone_event_fd *e = set->fds[i]; e; e = e->link)
                for (int i = 0; i < 2; i++)
                    if (FD_ISSET(e->fd, &fds[i]) && e->cbs[i].code && e->cbs[i].code(e->cbs[i].data) MUN_RETHROW)
                        return -1;
    #endif
    return 0;
}

struct cone_loop {
    cone_atom active;
    struct cone_event_io io;
    struct cone_event_schedule at;
};

static int cone_loop_init(struct cone_loop *loop) mun_throws(memory) {
    return cone_event_io_init(&loop->io);
}

static void cone_loop_fini(struct cone_loop *loop) {
    cone_event_io_fini(&loop->io);
    cone_event_schedule_fini(&loop->at);
}

static int cone_loop_run(struct cone_loop *loop) {
    while (atomic_load_explicit(&loop->active, memory_order_acquire))
        if (cone_event_io_emit(&loop->io, cone_event_schedule_emit(&loop->at)) MUN_RETHROW)
            return -1;
    return 0;
}

static void cone_loop_inc(struct cone_loop *loop) {
    atomic_fetch_add_explicit(&loop->active, 1, memory_order_release);
}

static void cone_loop_dec(struct cone_loop *loop) {
    if (atomic_fetch_sub_explicit(&loop->active, 1, memory_order_release) == 1)
        cone_event_io_ping(&loop->io);
}

enum {
    // lowest bits are either 0x2 (running and not detached) or 0x1 (finished xor detached)
    CONE_FLAG_SCHEDULED = 0x04,
    CONE_FLAG_RUNNING   = 0x08,
    CONE_FLAG_FINISHED  = 0x10,
    CONE_FLAG_FAILED    = 0x20,
    CONE_FLAG_CANCELLED = 0x40,
    CONE_FLAG_JOINED    = 0x80,
};

struct cone {
    cone_atom flags;
    struct cone_loop *loop;
    struct cone_closure body;
    struct cone_event done;
    struct mun_error error;
    void **rsp;
    _Alignas(max_align_t) char stack[];
};

_Thread_local struct cone * volatile cone = NULL;

static void cone_switch(struct cone *c) {
    c->flags ^= CONE_FLAG_RUNNING;
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
}

static int cone_run(struct cone *c) mun_nothrow {
    if (atomic_fetch_and(&c->flags, ~CONE_FLAG_SCHEDULED) & CONE_FLAG_FINISHED)
        return 0;
    struct cone* prev = cone;
    cone_switch(cone = c);
    cone = prev;
    return 0;
}

static int cone_schedule(struct cone *c) mun_throws(memory) {
    if (!(atomic_fetch_or(&c->flags, CONE_FLAG_SCHEDULED) & CONE_FLAG_SCHEDULED))
        if (cone_event_schedule_add(&c->loop->at, mun_usec_monotonic(), cone_bind(&cone_run, c)) MUN_RETHROW)
            return -1;
    return 0;
}

static int cone_ensure_running(struct cone *c) mun_throws(cancelled) {
    if (atomic_fetch_and(&c->flags, ~CONE_FLAG_CANCELLED) & CONE_FLAG_CANCELLED)
        return mun_error(cancelled, " ");
    return 0;
}

#define cone_pause(always_unsub, ev_add, ev_del, ...) do { \
    if (ev_add(__VA_ARGS__) MUN_RETHROW)                   \
        return -1;                                         \
    cone_switch(cone);                                     \
    int cancelled = cone_ensure_running(cone);             \
    if (always_unsub || cancelled)                         \
        ev_del(__VA_ARGS__);                               \
    return cancelled;                                      \
} while (0)

static void cone_unschedule(struct cone_event *ev, struct cone **c) {
    size_t i = mun_vec_find(ev, *_ == *c);
    if (i != ev->size)
        mun_vec_erase(ev, i, 1);
}

int cone_wait(struct cone_event *ev, cone_atom *uptr, unsigned u) {
    // TODO actual atomicity of this function w.r.t. modifications of `*uptr`.
    if (atomic_load(uptr) != u)
        return 1;
    cone_pause(0, mun_vec_append, cone_unschedule, ev, &(struct cone *){cone});
}

int cone_wake(struct cone_event *ev, size_t n) {
    for (; n-- && ev->size; mun_vec_erase(ev, 0, 1))
        if (cone_schedule(ev->data[0]) MUN_RETHROW)
            return -1;
    return 0;
}

int cone_iowait(int fd, int write) {
    cone_pause(1, cone_event_io_add, cone_event_io_del, &cone->loop->io, fd, write, cone_bind(&cone_schedule, cone));
}

int cone_sleep(mun_usec delay) {
    mun_usec at = mun_usec_monotonic() + delay;
    cone_pause(0, cone_event_schedule_add, cone_event_schedule_del, &cone->loop->at, at, cone_bind(&cone_schedule, cone));
}

int cone_yield(void) {
    cone_event_io_ping(&cone->loop->io);
    return cone_wait(&cone->loop->io.ping, &cone->loop->io.pinged, 1) MUN_RETHROW;
}

int cone_drop(struct cone *c) {
    if (c && !(--c->flags & 0x3)) {
        if ((c->flags & (CONE_FLAG_FAILED | CONE_FLAG_JOINED)) == CONE_FLAG_FAILED)
            if (c->error.code != mun_errno_cancelled)
                mun_error_show("cone destroyed with", &c->error);
        mun_vec_fini(&c->done);
        free(c);
    }
    return !c MUN_RETHROW;
}

int cone_cowait(struct cone *c, int flags) {
    struct mun_error saved = *mun_last_error();
    for (unsigned f; !((f = c->flags) & CONE_FLAG_FINISHED); )
        if (cone_wait(&c->done, &c->flags, f) < 0 MUN_RETHROW)
            return -1;
    if (!(flags & CONE_NORETHROW) && atomic_fetch_or(&c->flags, CONE_FLAG_JOINED) & CONE_FLAG_FAILED)
        return *mun_last_error() = c->error, -1;
    return *mun_last_error() = saved, 0;
}

int cone_cancel(struct cone *c) {
    c->flags |= CONE_FLAG_CANCELLED;
    return cone_schedule(c) MUN_RETHROW;
}

static void cone_body(struct cone *c) {
    if (cone_ensure_running(c) || c->body.code(c->body.data)) {
        c->error = *mun_last_error();
        c->flags |= CONE_FLAG_FAILED;
    }
    c->flags |= CONE_FLAG_FINISHED;
    mun_assert(!cone_event_schedule_add(&c->loop->at, mun_usec_monotonic(), cone_bind(&cone_drop, c)));
    mun_assert(!cone_wake(&c->done, (size_t)-1));
    cone_loop_dec(c->loop);
    cone_switch(c);
    abort();
}

static struct cone_loop cone_main_loop = {};

struct cone *cone_spawn(size_t size, struct cone_closure body) {
    size &= ~(size_t)(_Alignof(max_align_t) - 1);
    struct cone *c = (struct cone *)malloc(sizeof(struct cone) + size);
    if (c == NULL)
        return mun_error(memory, "no space for a stack"), NULL;
    c->flags = 1;
    c->loop = cone ? cone->loop : &cone_main_loop;
    c->body = body;
    c->done = (struct cone_event){};
    c->rsp = (void **)&c->stack[size] - 4;
    c->rsp[0] = c;                  // %rdi: first argument
    c->rsp[1] = NULL;               // %rbp: nothing; there's no previous frame yet
    c->rsp[2] = (void*)&cone_body;  // %rip: code to execute;
    c->rsp[3] = NULL;               // return address: nothing; same as for %rbp
    if (cone_schedule(c) MUN_RETHROW)
        return cone_drop(c), NULL;
    c->flags++;
    return cone_loop_inc(c->loop), c;
}

static int cone_main_run(struct cone_loop *loop) {
    mun_assert(!cone_loop_run(loop));
    return 0;
}

static void __attribute__((constructor)) cone_main_init(void) {
    mun_assert(!cone_loop_init(&cone_main_loop));
    struct cone *c = cone(&cone_main_run, &cone_main_loop);
    mun_assert(c != NULL);
    cone_switch(c);
}

static void __attribute__((destructor)) cone_main_fini(void) {
    struct cone *c = cone;
    if (c) {
        struct cone_loop *loop = c->loop;
        cone_loop_dec(loop);
        cone_switch(c);
        cone_drop(c);
        mun_assert(cone_event_schedule_emit(&loop->at) >= 0);
        cone_loop_fini(loop);
    }
}
