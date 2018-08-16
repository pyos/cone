#include "cone.h"
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdatomic.h>

#if __clang__
#if __has_feature(address_sanitizer)
#define CONE_ASAN 1
#endif
#else
#if defined(address_sanitizer_enabled) || defined(__SANITIZE_ADDRESS__)
#define CONE_ASAN 1
#endif
#endif

#if CONE_CXX
#include <alloca.h>
extern const size_t cone_cxa_globals_size;
void cone_cxa_globals_save(void *);
void cone_cxa_globals_load(void *);
#endif

#if CONE_ASAN
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** old_bottom, size_t* old_size);
#endif

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
    struct cone_closure f; // XXX this can be packed into 1 pointer by abusing alignment to store function id
};

struct cone_event_schedule {
    struct mun_vec(struct cone_closure) now;
    struct mun_vec(struct cone_event_at) later;
};

static void cone_event_schedule_fini(struct cone_event_schedule *ev) {
    mun_vec_fini(&ev->now);
    mun_vec_fini(&ev->later);
}

static int cone_event_schedule_add(struct cone_event_schedule *ev, mun_usec at, struct cone_closure f) mun_throws(memory) {
    if (at == 0)
        return mun_vec_append(&ev->now, &f);
    return mun_vec_insert(&ev->later, mun_vec_bisect(&ev->later, at < _->at), &((struct cone_event_at){at, f}));
}

static void cone_event_schedule_del(struct cone_event_schedule *ev, mun_usec at, struct cone_closure f) {
    for (size_t i = mun_vec_bisect(&ev->later, at < _->at); i-- && ev->later.data[i].at == at; )
        if (ev->later.data[i].f.code == f.code && ev->later.data[i].f.data == f.data)
            return mun_vec_erase(&ev->later, i, 1);
}

static mun_usec cone_event_schedule_emit(struct cone_event_schedule *ev, size_t limit) {
    while (1) {
        mun_usec now = mun_usec_monotonic();
        size_t more = 0;
        while (more < ev->later.size && ev->later.data[more].at <= now)
            more++;
        if (mun_vec_extend(&ev->now, &ev->later.data->f, more) MUN_RETHROW)
            return -1;
        mun_vec_erase(&ev->later, 0, more);
        if (!ev->now.size)
            return ev->later.size ? ev->later.data->at - now : MUN_USEC_MAX;
        do {
            if (!limit--)
                return 0;
            struct cone_closure f = ev->now.data[0];
            mun_vec_erase(&ev->now, 0, 1);
            if (f.code(f.data) MUN_RETHROW)
                return -1;
        } while (ev->now.size);
    }
}

struct cone_event_fd {
    int fd;
    int write;
    struct cone_closure f;
    struct cone_event_fd *link;
};

struct cone_event_io {
    int poller;
    int selfpipe[2];
    cone_atom pinged;
    struct cone_event ping;
    struct cone_event_fd ping_ev;
    struct cone_event_fd *fds[127];
};

static void cone_event_io_fini(struct cone_event_io *set) {
    mun_vec_fini(&set->ping);
    if (set->poller >= 0)
        close(set->poller);
    if (set->selfpipe[0] >= 0)
        close(set->selfpipe[0]), close(set->selfpipe[1]);
}

static void cone_event_io_ping(struct cone_event_io *set) {
    if (!atomic_fetch_add(&set->pinged, 1))
        write(set->selfpipe[1], "", 1);
}

static int cone_event_io_on_ping(struct cone_event_io *set) {
    char buf[32];
    read(set->selfpipe[0], buf, 32);  // never yields
    atomic_store_explicit(&set->pinged, 0, memory_order_release);
    return cone_wake(&set->ping, (size_t)-1) MUN_RETHROW;
}

static struct cone_event_fd **cone_event_io_bucket(struct cone_event_io *set, int fd) {
    struct cone_event_fd **b = &set->fds[fd % (sizeof(set->fds) / sizeof(set->fds[0]))];
    while (*b && (*b)->fd != fd) b = &(*b)->link;
    return b;
}

static int cone_event_io_add(struct cone_event_io *set, struct cone_event_fd *st) mun_throws(memory, assert) {
    struct cone_event_fd **b = cone_event_io_bucket(set, st->fd);
    st->link = *b;
    *b = st;
    #if CONE_EVNOTIFIER == 1
        struct epoll_event ev = {0, {.ptr = st}};
        for (struct cone_event_fd *e = st; e && e->fd == st->fd; e = e->link)
            ev.events |= e->write ? EPOLLOUT : EPOLLIN|EPOLLRDHUP;
        if (epoll_ctl(set->poller, st->link ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, st->fd, &ev) MUN_RETHROW_OS)
            return *b = st->link, -1;
    #elif CONE_EVNOTIFIER == 2
        struct kevent ev = {st->fd, st->write ? EVFILT_WRITE : EVFILT_READ, EV_ADD|EV_UDATA_SPECIFIC, 0, 0, st};
        if (kevent(set->poller, &ev, 1, NULL, 0, NULL) MUN_RETHROW_OS)
            return *b = st->link, -1;
    #endif
    return 0;
}

static void cone_event_io_del(struct cone_event_io *set, struct cone_event_fd *st) {
    struct cone_event_fd **b = cone_event_io_bucket(set, st->fd);
    struct cone_event_fd **c = b;
    while (*c && (*c)->fd == st->fd && *c != st) c = &(*c)->link;
    if (*c != st)
        return;
    *c = st->link;
    #if CONE_EVNOTIFIER == 1
        struct epoll_event ev = {0, {.ptr = *b}};
        for (struct cone_event_fd *e = *b; e && e->fd == st->fd; e = e->link)
            ev.events |= e->write ? EPOLLOUT : EPOLLIN|EPOLLRDHUP;
        mun_assert(!(epoll_ctl(set->poller, ev.events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL, st->fd, &ev) MUN_RETHROW_OS));
    #elif CONE_EVNOTIFIER == 2
        struct kevent ev = {st->fd, st->write ? EVFILT_WRITE : EVFILT_READ, EV_DELETE|EV_UDATA_SPECIFIC, 0, 0, st};
        mun_assert(!(kevent(set->poller, &ev, 1, NULL, 0, NULL) MUN_RETHROW_OS));
    #endif
}

static int cone_event_io_init(struct cone_event_io *set) mun_throws(memory) {
    set->selfpipe[0] = set->selfpipe[1] = set->poller = -1;
    if (pipe(set->selfpipe) MUN_RETHROW_OS)
        return -1;
    #if CONE_EVNOTIFIER == 1
        if ((set->poller = epoll_create1(0)) < 0 MUN_RETHROW_OS)
            return cone_event_io_fini(set), -1;
    #elif CONE_EVNOTIFIER == 2
        if ((set->poller = kqueue()) < 0 MUN_RETHROW_OS)
            return cone_event_io_fini(set), -1;
    #endif
    set->ping_ev.fd = set->selfpipe[0];
    set->ping_ev.f = cone_bind(&cone_event_io_on_ping, set);
    if (cone_event_io_add(set, &set->ping_ev) MUN_RETHROW_OS)
        return cone_event_io_fini(set), -1;
    return 0;
}

static int cone_event_io_emit(struct cone_event_io *set, mun_usec timeout) {
    if (timeout < 0 MUN_RETHROW)
        return -1;
    if (timeout > 60000000ll)
        timeout = 60000000ll;
    #if CONE_EVNOTIFIER == 1
        struct epoll_event evs[64];
        int got = epoll_wait(set->poller, evs, 64, timeout / 1000ul);
        if (got < 0)
            return errno != EINTR MUN_RETHROW_OS;
        for (int i = 0; i < got; i++) {
            for (struct cone_event_fd *st = evs[i].data.ptr, *e = st; e && e->fd == st->fd; e = e->link) {
                int flags = e->write ? EPOLLOUT : EPOLLIN|EPOLLRDHUP;
                if (evs[i].events & (flags|EPOLLERR|EPOLLHUP) && e->f.code(e->f.data) MUN_RETHROW)
                    return -1;
            }
        }
    #elif CONE_EVNOTIFIER == 2
        struct kevent evs[64];
        struct timespec ns = {timeout / 1000000ull, timeout % 1000000ull * 1000};
        int got = kevent(set->poller, NULL, 0, evs, 64, &ns);
        if (got < 0)
            return errno != EINTR MUN_RETHROW_OS;
        for (int i = 0; i < got; i++) {
            struct cone_event_fd *e = evs[i].udata;
            if (e->f.code(e->f.data) MUN_RETHROW)
                return -1;
        }
    #else
        fd_set fds[2] = {};
        int max_fd = 0;
        for (size_t i = 0; i < sizeof(set->fds) / sizeof(*set->fds); i++) {
            for (struct cone_event_fd *e = set->fds[i]; e; e = e->link) {
                if (max_fd <= e->fd)
                    max_fd = e->fd + 1;
                FD_SET(e->fd, &fds[e->write]);
            }
        }
        struct timeval us = {timeout / 1000000ull, timeout % 1000000ull};
        if (select(max_fd, &fds[0], &fds[1], NULL, &us) < 0)
            return errno != EINTR MUN_RETHROW_OS;
        for (size_t i = 0; i < sizeof(set->fds) / sizeof(*set->fds); i++)
            for (struct cone_event_fd *e = set->fds[i]; e; e = e->link)
                if (FD_ISSET(e->fd, &fds[e->write]) && e->f.code(e->f.data) MUN_RETHROW)
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
        if (cone_event_io_emit(&loop->io, cone_event_schedule_emit(&loop->at, 256)) MUN_RETHROW)
            return -1;
    return 0;
}

static void cone_loop_inc(struct cone_loop *loop) {
    atomic_fetch_add_explicit(&loop->active, 1, memory_order_relaxed);
}

static void cone_loop_dec(struct cone_loop *loop) {
    if (atomic_fetch_sub_explicit(&loop->active, 1, memory_order_acq_rel) == 1)
        cone_event_io_ping(&loop->io);
}

enum {
    CONE_FLAG_LAST_REF  = 0x01,
    CONE_FLAG_SCHEDULED = 0x02,
    CONE_FLAG_RUNNING   = 0x04,
    CONE_FLAG_FINISHED  = 0x08,
    CONE_FLAG_FAILED    = 0x10,
    CONE_FLAG_CANCELLED = 0x20,
    CONE_FLAG_TIMED_OUT = 0x40,
};

struct cone {
    cone_atom flags;
    void **rsp;
    struct cone_loop *loop;
    struct cone_closure body;
    struct cone_event done;
    #if CONE_ASAN
        const void * target_stack;
        size_t target_stack_size;
    #endif
    struct mun_error error;
    _Alignas(max_align_t) char stack[];
};

_Thread_local struct cone * cone = NULL;

static void cone_switch(struct cone *c) {
    c->flags ^= CONE_FLAG_RUNNING;
    #if CONE_CXX
        #if CONE_ASAN && __clang__
            // XXX a bug in LLVM causes it to ignore the clobbering of %rbx, which is used as a stack pointer
            //     instead of %rsp when asan is enabled and there is an alloca.
            char cxa_globals[64];
        #else
            void * cxa_globals = alloca(cone_cxa_globals_size);
        #endif
        cone_cxa_globals_save(cxa_globals);
    #endif
    #if CONE_ASAN
        void * fake_stack = NULL;
        __sanitizer_start_switch_fiber(c->flags & CONE_FLAG_FINISHED ? NULL : &fake_stack, c->target_stack, c->target_stack_size);
    #endif
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
      : "rdx", "rbx", "rsi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "cc", "memory");
    // code from here on only runs when switching back into the event loop or an already running coroutine
    // (when switching into a coroutine for the first time, `ret` jumps right into `cone_body`.)
    #if CONE_ASAN
        __sanitizer_finish_switch_fiber(fake_stack, &c->target_stack, &c->target_stack_size);
    #endif
    #if CONE_CXX
        cone_cxa_globals_load(cxa_globals);
    #endif
}

static int cone_run(struct cone *c) mun_nothrow {
    c->flags &= ~CONE_FLAG_SCHEDULED;
    if (c->flags & CONE_FLAG_FINISHED)
        return 0;
    struct mun_error *ep = mun_set_error_storage(&c->error);
    struct cone *prev = cone;
    cone_switch(cone = c);
    mun_set_error_storage(ep);
    cone = prev;
    return 0;
}

static int cone_schedule(struct cone *c) mun_throws(memory) {
    if (atomic_fetch_or(&c->flags, CONE_FLAG_SCHEDULED) & (CONE_FLAG_SCHEDULED | CONE_FLAG_FINISHED))
        return 0;
    return cone_event_schedule_add(&c->loop->at, 0, cone_bind(&cone_run, c)) MUN_RETHROW;
}

static int cone_ensure_running(struct cone *c) mun_throws(cancelled) {
    int state = atomic_fetch_and(&c->flags, ~CONE_FLAG_CANCELLED & ~CONE_FLAG_TIMED_OUT);
    return state & CONE_FLAG_CANCELLED ? mun_error(cancelled, " ")
         : state & CONE_FLAG_TIMED_OUT ? mun_error(timeout, " ") : 0;
}

#define cone_pause(ev_add, ev_del, ...) do {                          \
    if (cone_ensure_running(cone) || ev_add(__VA_ARGS__) MUN_RETHROW) \
        return -1;                                                    \
    cone_switch(cone);                                                \
    int cancelled = cone_ensure_running(cone) MUN_RETHROW;            \
    ev_del(__VA_ARGS__);                                              \
    return cancelled;                                                 \
} while (0)

static void cone_event_unsub(struct cone_event *ev, struct cone **c) {
    size_t i = mun_vec_find(ev, *_ == *c);
    if (i != ev->size)
        mun_vec_erase(ev, i, 1);
}

int cone_wait(struct cone_event *ev, const cone_atom *uptr, unsigned u) {
    // TODO thread-safety
    if (*uptr != u)
        return mun_error(retry, " ");
    cone_pause(mun_vec_append, cone_event_unsub, ev, &(struct cone *){cone});
}

int cone_wake(struct cone_event *ev, size_t n) {
    // TODO thread-safety
    for (; n-- && ev->size; mun_vec_erase(ev, 0, 1))
        if (cone_schedule(ev->data[0]) MUN_RETHROW)
            return -1;
    return 0;
}

int cone_iowait(int fd, int write) {
    struct cone_event_fd ev = {fd, write, cone_bind(&cone_schedule, cone), NULL};
    cone_pause(cone_event_io_add, cone_event_io_del, &cone->loop->io, &ev);
}

int cone_sleep_until(mun_usec t) {
    cone_pause(cone_event_schedule_add, cone_event_schedule_del, &cone->loop->at, t, cone_bind(&cone_schedule, cone));
}

int cone_yield(void) {
    cone_event_io_ping(&cone->loop->io);
    return cone_wait(&cone->loop->io.ping, &cone->loop->io.pinged, 1) MUN_RETHROW;
}

int cone_drop(struct cone *c) {
    if (c && (atomic_fetch_xor(&c->flags, CONE_FLAG_LAST_REF) & CONE_FLAG_LAST_REF)) {
        if (c->flags & CONE_FLAG_FAILED && c->error.code != mun_errno_cancelled)
            mun_error_show("cone destroyed with", &c->error);
        mun_vec_fini(&c->done);
        free(c);
    }
    return !c MUN_RETHROW;
}

int cone_cowait(struct cone *c, int flags) {
    if (c == cone)
        return mun_error(deadlock, "coroutine waiting on itself");
    for (unsigned f; !((f = c->flags) & CONE_FLAG_FINISHED); )
        if (cone_wait(&c->done, &c->flags, f) < 0 && mun_last_error()->code != mun_errno_retry MUN_RETHROW)
            return -1;
    if (!(flags & CONE_NORETHROW) && atomic_fetch_and(&c->flags, ~CONE_FLAG_FAILED) & CONE_FLAG_FAILED)
        return *mun_last_error() = c->error, mun_error_up(MUN_CURRENT_FRAME);
    return 0;
}

int cone_cancel(struct cone *c) {
    c->flags |= CONE_FLAG_CANCELLED;
    return cone_schedule(c) MUN_RETHROW;
}

static int cone_timeout(struct cone *c) {
    c->flags |= CONE_FLAG_TIMED_OUT;
    return cone_schedule(c) MUN_RETHROW;
}

int cone_deadline(struct cone *c, mun_usec t) {
    return cone_event_schedule_add(&c->loop->at, t, cone_bind(&cone_timeout, c)) MUN_RETHROW;
}

void cone_complete(struct cone *c, mun_usec t) {
    cone_event_schedule_del(&c->loop->at, t, cone_bind(&cone_timeout, c));
}

static void cone_body(struct cone *c) {
    #if CONE_ASAN
        __sanitizer_finish_switch_fiber(NULL, &c->target_stack, &c->target_stack_size);
    #endif
    #if CONE_CXX
        cone_cxa_globals_load(NULL);
    #endif
    c->flags |= (c->body.code(c->body.data) ? CONE_FLAG_FAILED : 0) | CONE_FLAG_FINISHED;
    mun_assert(!cone_wake(&c->done, (size_t)-1));
    mun_assert(!cone_event_schedule_add(&c->loop->at, 0, cone_bind(&cone_drop, c)));
    cone_loop_dec(c->loop);
    cone_switch(c);
    abort();
}

static struct cone *cone_spawn_on(struct cone_loop *loop, size_t size, struct cone_closure body) {
    size &= ~(size_t)(_Alignof(max_align_t) - 1);
    struct cone *c = (struct cone *)malloc(sizeof(struct cone) + size);
    if (c == NULL)
        return mun_error(memory, "no space for a stack"), NULL;
    c->flags = CONE_FLAG_SCHEDULED;
    c->loop = loop;
    c->body = body;
    c->done = (struct cone_event){};
    #if CONE_ASAN
        c->target_stack = c->stack;
        c->target_stack_size = size;
    #endif
    c->rsp = (void **)&c->stack[size] - 4;
    c->rsp[0] = c;                  // %rdi: first argument
    c->rsp[1] = NULL;               // %rbp: nothing; there's no previous frame yet
    c->rsp[2] = (void*)&cone_body;  // %rip: code to execute;
    c->rsp[3] = NULL;               // return address: nothing; same as for %rbp
    if (cone_event_schedule_add(&c->loop->at, 0, cone_bind(&cone_run, c)) MUN_RETHROW) {
        c->flags ^= CONE_FLAG_LAST_REF;
        return cone_drop(c), NULL;
    }
    return cone_loop_inc(c->loop), c;
}

static struct cone_loop cone_main_loop = {};

struct cone *cone_spawn(size_t size, struct cone_closure body) {
    return cone_spawn_on(cone ? cone->loop : &cone_main_loop, size, body);
}

int cone_loop(size_t size, struct cone_closure body) {
    struct cone_loop loop = {};
    if (cone_loop_init(&loop) MUN_RETHROW)
        return -1;
    struct cone *c = cone_spawn_on(&loop, size, body);
    if (c == NULL || cone_loop_run(&loop) MUN_RETHROW)
        return cone_drop(c), cone_loop_fini(&loop), -1;
    return cone_loop_fini(&loop), cone_join(c, 0) MUN_RETHROW;
}

static int cone_main_run(struct cone_loop *loop) {
    mun_assert(!cone_loop_run(loop));
    return 0;
}

const cone_atom * cone_count(void) {
    return cone ? &cone->loop->active : NULL;
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
        cone_loop_dec(&cone_main_loop);
        cone_switch(c);
        cone_drop(c);
        mun_assert(cone_event_schedule_emit(&cone_main_loop.at, (size_t)-1) >= 0);
        cone_loop_fini(&cone_main_loop);
    }
}
