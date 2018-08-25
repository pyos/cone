#include "cone.h"
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <stdatomic.h>

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
#define CONE_ASAN 1
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** old_bottom, size_t* old_size);
#endif

// Mach-O requires some weird link-time magic to properly support weak symbols,
// and also other non-ELF formats probably don't support them at all, so provide a switch.
#if CONE_CXX
struct /*__cxxabiv1::, dunno what the mangled name is, doesn't matter */__cxa_eh_globals {
    void* caughtExceptions;
    unsigned int uncaughtExceptions;
};

struct __cxa_eh_globals *__cxa_get_globals();
#endif

#ifndef CONE_EVNOTIFIER
#define CONE_EVNOTIFIER (__linux__ ? 1 : __FreeBSD__ || __APPLE__ ? 2 : 0)
#endif

#if !__x86_64__
// There used to be a `sigaltstack`-based implementation, but it was crap.
_Static_assert(0, "stack switching is only supported on x86-64 UNIX");
#endif

#if CONE_EVNOTIFIER == 1
#include <sys/epoll.h>
#elif CONE_EVNOTIFIER == 2
#include <sys/event.h>
#else
#include <sys/select.h>
#endif

// FIXME: why is this here and not somewhere in the vicinity of cold.c?
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
    struct cone_event yield;
};

static void cone_event_schedule_fini(struct cone_event_schedule *ev) {
    mun_vec_fini(&ev->now);
    mun_vec_fini(&ev->later);
    mun_vec_fini(&ev->yield);
}

static int cone_event_schedule_add(struct cone_event_schedule *ev, mun_usec at, struct cone_closure f) {
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
    if (cone_wake(&ev->yield, (size_t)-1) MUN_RETHROW)
        return -1;
    while (1) {
        mun_usec now = mun_usec_monotonic();
        size_t more = 0;
        while (more < ev->later.size && ev->later.data[more].at <= now)
            more++;
        if (more && mun_vec_extend(&ev->now, &ev->later.data->f, more) MUN_RETHROW)
            return -1;
        mun_vec_erase(&ev->later, 0, more);
        if (!ev->now.size)
            return ev->yield.size ? 0 : ev->later.size ? ev->later.data->at - now : MUN_USEC_MAX;
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
    #if CONE_EVNOTIFIER != 2
        struct cone_event_fd *fds[127];
    #endif
};

static void cone_event_io_fini(struct cone_event_io *set) {
    if (set->poller >= 0)
        close(set->poller);
}

static int cone_event_io_mod(struct cone_event_io *set, struct cone_event_fd *st, int add) {
    #if CONE_EVNOTIFIER == 2
        uint16_t flags = (add ? EV_ADD : EV_DELETE)|EV_UDATA_SPECIFIC;
        struct kevent ev = {st->fd, st->write ? EVFILT_WRITE : EVFILT_READ, flags, 0, 0, st};
        return kevent(set->poller, &ev, 1, NULL, 0, NULL) && errno != ENOENT MUN_RETHROW_OS;
    #else
        struct cone_event_fd **b = &set->fds[st->fd % (sizeof(set->fds) / sizeof(set->fds[0]))];
        while (*b && (*b)->fd != st->fd)
            b = &(*b)->link;
        struct cone_event_fd **c = b;
        if (add) {
            st->link = *c;
            *c = st;
        } else {
            while (*c && (*c)->fd == st->fd && *c != st)
                c = &(*c)->link;
            if (*c != st)
                return 0;
            *c = st->link;
        }
        #if CONE_EVNOTIFIER == 1
            struct epoll_event ev = {0, {.ptr = *b}};
            for (struct cone_event_fd *e = *b; e && e->fd == st->fd; e = e->link)
                ev.events |= e->write ? EPOLLOUT : EPOLLIN|EPOLLRDHUP;
            int op = add && !st->link ? EPOLL_CTL_ADD : !add && !ev.events ? EPOLL_CTL_DEL : EPOLL_CTL_MOD;
            if (epoll_ctl(set->poller, op, st->fd, &ev) MUN_RETHROW_OS)
                return *c = (add ? st->link : st), -1;
        #endif
        return 0;
    #endif
}

static int cone_event_io_add(struct cone_event_io *set, struct cone_event_fd *st) {
    return cone_event_io_mod(set, st, 1) MUN_RETHROW;
}

static void cone_event_io_del(struct cone_event_io *set, struct cone_event_fd *st) {
    mun_cant_fail(cone_event_io_mod(set, st, 0) MUN_RETHROW);
}

static int cone_event_io_init(struct cone_event_io *set) {
    (void)set;
    #if CONE_EVNOTIFIER == 1
        if ((set->poller = epoll_create1(EPOLL_CLOEXEC)) < 0 MUN_RETHROW_OS)
            return cone_event_io_fini(set), -1;
    #elif CONE_EVNOTIFIER == 2
        if ((set->poller = kqueue()) < 0 || fcntl(set->poller, F_SETFD, FD_CLOEXEC) < 0 MUN_RETHROW_OS)
            return cone_event_io_fini(set), -1;
    #endif
    return 0;
}

static int cone_event_io_emit(struct cone_event_io *set, mun_usec timeout) {
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
                FD_SET(e->fd, &fds[!!e->write]);
            }
        }
        struct timeval us = {timeout / 1000000ull, timeout % 1000000ull};
        if (select(max_fd, &fds[0], &fds[1], NULL, &us) < 0)
            return errno != EINTR MUN_RETHROW_OS;
        for (size_t i = 0; i < sizeof(set->fds) / sizeof(*set->fds); i++)
            for (struct cone_event_fd *e = set->fds[i]; e; e = e->link)
                if (FD_ISSET(e->fd, &fds[!!e->write]) && e->f.code(e->f.data) MUN_RETHROW)
                    return -1;
    #endif
    return 0;
}

struct cone_loop {
    cone_atom active;
    struct cone_event_io io;
    struct cone_event_schedule at;
};

static int cone_loop_init(struct cone_loop *loop) {
    return cone_event_io_init(&loop->io);
}

static void cone_loop_fini(struct cone_loop *loop) {
    cone_event_io_fini(&loop->io);
    cone_event_schedule_fini(&loop->at);
}

static int cone_loop_run(struct cone_loop *loop) {
    while (1) {
        mun_usec next = cone_event_schedule_emit(&loop->at, 256);
        if (next == MUN_USEC_MAX && !atomic_load_explicit(&loop->active, memory_order_acquire))
            return 0;
        if (next < 0 || cone_event_io_emit(&loop->io, next) MUN_RETHROW)
            return -1;
    }
}

enum {
    CONE_FLAG_LAST_REF  = 0x01,
    CONE_FLAG_SCHEDULED = 0x02,
    CONE_FLAG_FINISHED  = 0x08,
    CONE_FLAG_FAILED    = 0x10,
    CONE_FLAG_CANCELLED = 0x20,
    CONE_FLAG_TIMED_OUT = 0x40,
    CONE_FLAG_JOINED    = 0x80,
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
    #if CONE_CXX
        struct __cxa_eh_globals cxa_globals = *__cxa_get_globals();
    #endif
    #if CONE_ASAN
        void * fake_stack = NULL;
        __sanitizer_start_switch_fiber(&fake_stack, c->target_stack, c->target_stack_size);
    #endif
    __asm__("jmp  %=0f       \n"
    "%=1:"  "push %%rbp      \n"
            "push %%rdi      \n"
            "mov  %%rsp, %0  \n" // `xchg` is implicitly `lock`ed; 3 `mov`s are faster
            "mov  %1, %%rsp  \n" // (the third is inserted by the compiler to load c->rsp)
            "pop  %%rdi      \n" // FIXME prone to incur cache misses
            "pop  %%rbp      \n"
            "ret             \n" // jumps into `cone_body` if this is a new coroutine
    "%=0:"  "call %=1b       \n"
      : "=m"(c->rsp) : "c"(c->rsp)
      : "rax", "rdx", "rbx", "rsi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "cc", "memory");
    #if CONE_ASAN
        __sanitizer_finish_switch_fiber(fake_stack, &c->target_stack, &c->target_stack_size);
    #endif
    #if CONE_CXX
        *__cxa_get_globals() = cxa_globals;
    #endif
}

static int cone_drop_ex(struct cone *c) {
    return cone_drop(c), 0;
}

static void __attribute__((noreturn)) cone_body(struct cone *c) {
    #if CONE_ASAN
        __sanitizer_finish_switch_fiber(NULL, &c->target_stack, &c->target_stack_size);
    #endif
    #if CONE_CXX
        *__cxa_get_globals() = (struct __cxa_eh_globals){ NULL, 0 };
    #endif
    c->flags |= (c->body.code(c->body.data) ? CONE_FLAG_FAILED : 0) | CONE_FLAG_FINISHED;
    // 1. `cone_drop` may deallocate the current stack, so we have to switch before calling it.
    // 2. the callback is scheduled to run as early as possible to minimize the probability
    //    of `c` getting evicted from CPU caches.
    // 3. inserting at 0 here is cheap because `cone_run(c)` was recently popped off the same
    //    vector, so there's guaranteed to be some empty space at the beginning.
    mun_cant_fail(mun_vec_insert(&c->loop->at.now, 0, &cone_bind(&cone_drop_ex, c)) MUN_RETHROW);
    mun_cant_fail(cone_wake(&c->done, (size_t)-1) MUN_RETHROW);
    atomic_fetch_sub_explicit(&c->loop->active, 1, memory_order_acq_rel);
    #if CONE_ASAN
        __sanitizer_start_switch_fiber(NULL, c->target_stack, c->target_stack_size);
    #endif
    __asm__("mov  %0, %%rsp  \n"
            "pop  %%rdi      \n"
            "pop  %%rbp      \n"
            "ret             \n" :: "r"(c->rsp));
    __builtin_unreachable();
}

static int cone_run(struct cone *c) {
    struct mun_error *ep = mun_set_error_storage(&c->error); // FIXME prone to incur cache misses
    struct cone *prev = cone;
    cone_switch(cone = c);
    // Only after switching back; else there's a use-after-free on `cone_cancel(cone)`
    // followed by terminating without ever preempting if the coroutine is detached.
    c->flags &= ~CONE_FLAG_SCHEDULED;
    mun_set_error_storage(ep);
    cone = prev;
    return 0;
}

static struct cone *cone_spawn_on(struct cone_loop *loop, size_t size, struct cone_closure body) {
    size &= ~(size_t)(_Alignof(max_align_t) - 1);
    struct cone *c = (struct cone *)malloc(sizeof(struct cone) + size);
    if (c == NULL)
        return (void)mun_error(memory, "no space for a stack"), NULL;
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
    if (cone_event_schedule_add(&loop->at, 0, cone_bind(&cone_run, c)) MUN_RETHROW) {
        c->flags ^= CONE_FLAG_LAST_REF;
        return cone_drop(c), NULL;
    }
    atomic_fetch_add_explicit(&loop->active, 1, memory_order_relaxed);
    return c;
}

struct cone *cone_spawn(size_t size, struct cone_closure body) {
    return cone_spawn_on(cone->loop, size, body);
}

void cone_drop(struct cone *c) {
    if (c && (atomic_fetch_xor(&c->flags, CONE_FLAG_LAST_REF) & CONE_FLAG_LAST_REF)) {
        if ((c->flags & (CONE_FLAG_FAILED | CONE_FLAG_JOINED)) == CONE_FLAG_FAILED)
            if (c->error.code != mun_errno_cancelled)
                mun_error_show("cone destroyed with", &c->error);
        mun_vec_fini(&c->done);
        free(c);
    }
}

static int cone_schedule(struct cone *c) {
    if (atomic_fetch_or(&c->flags, CONE_FLAG_SCHEDULED) & (CONE_FLAG_SCHEDULED | CONE_FLAG_FINISHED))
        return 0;
    return cone_event_schedule_add(&c->loop->at, 0, cone_bind(&cone_run, c)) MUN_RETHROW;
}

static int cone_ensure_running(struct cone *c) {
    int state = atomic_fetch_and(&c->flags, ~CONE_FLAG_CANCELLED & ~CONE_FLAG_TIMED_OUT);
    return state & CONE_FLAG_CANCELLED ? mun_error(cancelled, " ")
         : state & CONE_FLAG_TIMED_OUT ? mun_error(timeout, " ") : 0;
}

#define cone_pause(ev_add, ev_del, ...) do {                          \
    if (cone_ensure_running(cone) || ev_add(__VA_ARGS__) MUN_RETHROW) \
        return -1;                                                    \
    cone_switch(cone);                                                \
    if (cone_ensure_running(cone) MUN_RETHROW)                        \
        return ev_del(__VA_ARGS__), -1;                               \
    return 0;                                                         \
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

struct cone_iowait_tmp {
    struct cone *c;
    struct cone_event_fd ev;
};

static int cone_iowake(struct cone_iowait_tmp *st) {
    cone_event_io_del(&st->c->loop->io, &st->ev);
    return cone_schedule(st->c);
}

int cone_iowait(int fd, int write) {
    struct cone_iowait_tmp st = {cone, {fd, write, cone_bind(&cone_iowake, &st), NULL}};
    cone_pause(cone_event_io_add, cone_event_io_del, &cone->loop->io, &st.ev);
}

int cone_sleep_until(mun_usec t) {
    cone_pause(cone_event_schedule_add, cone_event_schedule_del, &cone->loop->at, t, cone_bind(&cone_schedule, cone));
}

int cone_yield(void) {
    cone_atom fake = 0;
    return cone_wait(&cone->loop->at.yield, &fake, 0) MUN_RETHROW;
}

int cone_cowait(struct cone *c, int norethrow) {
    if (c == cone) // maybe detect more complicated deadlocks too?..
        return mun_error(deadlock, "coroutine waiting on itself");
    for (unsigned f; !((f = c->flags) & CONE_FLAG_FINISHED); )
        if (cone_wait(&c->done, &c->flags, f) && mun_errno != EAGAIN MUN_RETHROW)
            return -1;
    if (!norethrow && atomic_fetch_or(&c->flags, CONE_FLAG_JOINED) & CONE_FLAG_FAILED)
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

const cone_atom * cone_count(void) {
    return cone ? &cone->loop->active : NULL;
}

int cone_loop(size_t size, struct cone_closure body) {
    struct cone_loop loop = {};
    if (cone_loop_init(&loop) MUN_RETHROW)
        return -1;
    struct cone *c = cone_spawn_on(&loop, size, body);
    // FIXME if `cone_loop_run` fails, things get ugly and coroutines get leaked. in theory,
    //       `memory` errors (the only kind possible right now) can be handled by freeing
    //       some up and calling `cone_loop_run` again, though this function can't do that.
    if (c == NULL || cone_loop_run(&loop) MUN_RETHROW)
        return cone_drop(c), cone_loop_fini(&loop), -1;
    return cone_loop_fini(&loop), cone_join(c, 0) MUN_RETHROW;
}

static int cone_main_run(struct cone_loop *loop) {
    return mun_cant_fail(cone_loop_run(loop) MUN_RETHROW);
}

static struct cone_loop cone_main_loop = {};

static void __attribute__((constructor)) cone_main_init(void) {
    mun_cant_fail(cone_loop_init(&cone_main_loop) MUN_RETHROW);
    struct cone *c = cone_spawn_on(&cone_main_loop, CONE_DEFAULT_STACK, cone_bind(&cone_main_run, &cone_main_loop));
    mun_cant_fail(c == NULL MUN_RETHROW);
    cone_switch(c); // the loop will then switch back because the coroutine is scheduled to run
}

static void __attribute__((destructor)) cone_main_fini(void) {
    mun_assert(!cone || cone->loop->active == 1,
        "main() returned, but %u more coroutine(s) are still alive. They may attempt to use "
        "destroyed global data. main() should join all coroutines it spawns.", cone->loop->active - 1);
}
