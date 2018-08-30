#include "cone.h"
#include <fcntl.h>
#include <sched.h>
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
#include <fcntl.h>
#include <sys/event.h>
#else
#include <sys/select.h>
#endif

static inline void cone_lock(void **a) {
    while (atomic_exchange((volatile _Atomic(uintptr_t) *)a, 1))
        sched_yield();
}

static inline void cone_unlock(void **a) {
    atomic_store_explicit((volatile _Atomic(uintptr_t) *)a, 0, memory_order_release);
}

enum {
    CONE_FLAG_LAST_REF  = 0x01,
    CONE_FLAG_SCHEDULED = 0x02,
    CONE_FLAG_WOKEN     = 0x04,
    CONE_FLAG_FINISHED  = 0x08,
    CONE_FLAG_FAILED    = 0x10,
    CONE_FLAG_CANCELLED = 0x20,
    CONE_FLAG_TIMED_OUT = 0x40,
    CONE_FLAG_JOINED    = 0x80,
};

static void cone_run(struct cone *);

// Returns the loop if it may need a ping to notice the change in its run queue.
// If it is know that the loop is not blocked in a syscall, this can be ignored.
static struct cone_loop *cone_schedule(struct cone *, int);

struct cone_event_at {
    mun_usec at;
    uintptr_t c;
};

struct cone_event_schedule mun_vec(struct cone_event_at);

static void cone_event_schedule_fini(struct cone_event_schedule *ev) {
    mun_vec_fini(ev);
}

static int cone_event_schedule_add(struct cone_event_schedule *ev, mun_usec at, struct cone *c, int deadline) {
    return mun_vec_insert(ev, mun_vec_bisect(ev, at < _->at), &((struct cone_event_at){at, (uintptr_t)c|deadline}));
}

static void cone_event_schedule_del(struct cone_event_schedule *ev, mun_usec at, struct cone *c, int deadline) {
    for (size_t i = mun_vec_bisect(ev, at < _->at); i-- && ev->data[i].at == at; )
        if (ev->data[i].c == ((uintptr_t)c|deadline))
            return mun_vec_erase(ev, i, 1);
}

static mun_usec cone_event_schedule_emit(struct cone_event_schedule *ev) {
    while (ev->size) {
        mun_usec t = mun_usec_monotonic();
        if (ev->data->at > t)
            return ev->data->at - t;
        for (; ev->size && ev->data->at <= t; mun_vec_erase(ev, 0, 1))
            cone_schedule((struct cone *)(ev->data->c & ~1ul), ev->data->c & 1 ? CONE_FLAG_TIMED_OUT : CONE_FLAG_WOKEN);
    }
    return MUN_USEC_MAX;
}

struct cone_event_fd {
    int fd;
    int write;
    struct cone *c;
    struct cone_event_fd *link;
};

struct cone_event_io {
    int poller;
    int selfpipe[2];
    cone_atom pinged;
    #if CONE_EVNOTIFIER != 2
        struct cone_event_fd *fds[127];
    #endif
};

static void cone_event_io_fini(struct cone_event_io *set) {
    if (set->poller >= 0)
        close(set->poller);
    if (set->selfpipe[0] >= 0)
        close(set->selfpipe[0]), close(set->selfpipe[1]);
}

static int cone_event_io_init(struct cone_event_io *set) {
    set->selfpipe[0] = set->selfpipe[1] = set->poller = -1;
    // XXX racy in forking multithreaded applications (see `man fcntl`); use pipe2 on linux?
    if (pipe(set->selfpipe) || fcntl(set->selfpipe[0], F_SETFD, FD_CLOEXEC)
                            || fcntl(set->selfpipe[1], F_SETFD, FD_CLOEXEC) MUN_RETHROW_OS)
        return cone_event_io_fini(set), -1;
    #if CONE_EVNOTIFIER == 1
        if ((set->poller = epoll_create1(EPOLL_CLOEXEC)) < 0 MUN_RETHROW_OS)
            return cone_event_io_fini(set), -1;
        struct epoll_event ev = {EPOLLIN, {.ptr = NULL}};
        if (epoll_ctl(set->poller, EPOLL_CTL_ADD, set->selfpipe[0], &ev) MUN_RETHROW_OS)
            return cone_event_io_fini(set), -1;
    #elif CONE_EVNOTIFIER == 2
        if ((set->poller = kqueue()) < 0 || fcntl(set->poller, F_SETFD, FD_CLOEXEC) MUN_RETHROW_OS)
            return cone_event_io_fini(set), -1;
        struct kevent ev = {set->selfpipe[0], EVFILT_READ, EV_ADD, 0, 0, NULL};
        if (kevent(set->poller, &ev, 1, NULL, 0, NULL) MUN_RETHROW_OS)
            return cone_event_io_fini(set), -1;
    #endif
    return 0;
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

static void cone_event_io_ping(struct cone_event_io *set) {
    if (!atomic_exchange(&set->pinged, 1))
        write(set->selfpipe[1], "", 1);
}

static void cone_event_io_call(struct cone_event_io *set, struct cone_event_fd *e) {
    cone_schedule(e->c, CONE_FLAG_WOKEN);
    cone_event_io_del(set, e); // `e` is still allocated & `e->link` points to next element
}

static int cone_event_io_emit(struct cone_event_io *set, mun_usec timeout) {
    if (timeout > 60000000ll)
        timeout = 60000000ll;
    #if CONE_EVNOTIFIER == 1
        struct epoll_event evs[64];
        int got = epoll_wait(set->poller, evs, 64, timeout / 1000ul);
        if (got < 0)
            return errno != EINTR MUN_RETHROW_OS;
        for (int i = 0; i < got; i++)
            for (struct cone_event_fd *st = evs[i].data.ptr, *e = st; e && e->fd == st->fd; e = e->link)
                if (evs[i].events & ((e->write ? EPOLLOUT : EPOLLIN|EPOLLRDHUP)|EPOLLERR|EPOLLHUP))
                    cone_event_io_call(set, e);
    #elif CONE_EVNOTIFIER == 2
        struct kevent evs[64];
        struct timespec ns = {timeout / 1000000ull, timeout % 1000000ull * 1000};
        int got = kevent(set->poller, NULL, 0, evs, 64, &ns);
        if (got < 0)
            return errno != EINTR MUN_RETHROW_OS;
        for (int i = 0; i < got; i++)
            if (evs[i].udata)
                cone_event_io_call(set, evs[i].udata);
    #else
        fd_set fds[2] = {};
        FD_SET(set->selfpipe[0], &fds[0]);
        int max_fd = set->selfpipe[0] + 1;
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
                if (FD_ISSET(e->fd, &fds[!!e->write]))
                    cone_event_io_call(set, e);
    #endif
    if (atomic_load_explicit(&set->pinged, memory_order_acquire)) {
        read(set->selfpipe[0], (char[4]){}, 4);
        atomic_store_explicit(&set->pinged, 0, memory_order_release);
    }
    return 0;
}

struct cone_event_it {
    struct cone_event_it *next, *prev;
    struct cone *c;
};

static int cone_event_add(struct cone_event *ev, struct cone_event_it *it) {
    it->next = NULL;
    it->prev = ev->tail;
    it->prev ? (it->prev->next = it) : (ev->head = it);
    ev->tail = it;
    return 0;
}

static void cone_event_del(struct cone_event *ev, struct cone_event_it *it) {
    // This'd be easier if it was a circular list, but then `cone_event` would not be movable.
    it->prev ? (it->prev->next = it->next) : (ev->head = it->next);
    it->next ? (it->next->prev = it->prev) : (ev->tail = it->prev);
    it->next = it;
    it->prev = it;
}

static struct cone *cone_event_pop(struct cone_event *ev) {
    struct cone_event_it *it = ev->head;
    if (!it)
        return NULL;
    return cone_event_del(ev, it), it->c;
}

static void cone_runq_add(struct cone_event *rq, struct cone_event_it *it) {
    cone_lock(&rq->lk);
    cone_event_add(rq, it);
    cone_unlock(&rq->lk);
}

static int cone_runq_nonempty(struct cone_event *rq) {
    cone_lock(&rq->lk);
    int ret = !!rq->head; // XXX does this REALLY need a lock?
    cone_unlock(&rq->lk);
    return ret;
}

static void cone_runq_emit(struct cone_event *rq, size_t limit) {
    while (limit--) {
        cone_lock(&rq->lk);
        struct cone *c = cone_event_pop(rq);
        cone_unlock(&rq->lk);
        if (!c)
            break;
        cone_run(c);
    }
}

struct cone_loop {
    cone_atom active;
    struct cone_event now;
    struct cone_event_io io;
    struct cone_event_schedule at;
};

static int cone_loop_run(struct cone_loop *loop) {
    if (cone_event_io_init(&loop->io) MUN_RETHROW)
        return -1;
    while (1) {
        cone_runq_emit(&loop->now, 256);
        mun_usec next = cone_event_schedule_emit(&loop->at);
        if (next == MUN_USEC_MAX && !atomic_load_explicit(&loop->active, memory_order_acquire))
            break;
        if (next > 0 && cone_runq_nonempty(&loop->now))
            next = 0;
        // If this fails, coroutines will get leaked.
        mun_cant_fail(cone_event_io_emit(&loop->io, next) MUN_RETHROW);
    }
    cone_event_io_fini(&loop->io);
    cone_event_schedule_fini(&loop->at);
    return 0;
}

struct cone {
    cone_atom flags;
    void **rsp;
    struct cone_loop *loop;
    struct cone_event_it runq;
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
    unsigned mxcsr;
    __asm__("stmxcsr %1      \n"
            "jmp  %=0f       \n"
    "%=1:"  "push %%rbp      \n"
            "push %%rdi      \n"
            "mov  %%rsp, %0  \n" // `xchg` is implicitly `lock`ed; 3 `mov`s are faster
            "mov  %2, %%rsp  \n" // (the third is inserted by the compiler to load c->rsp)
            "pop  %%rdi      \n" // FIXME prone to incur cache misses
            "pop  %%rbp      \n"
            "ret             \n" // jumps into `cone_body` if this is a new coroutine
    "%=0:"  "call %=1b       \n"
            "ldmxcsr %1      \n"
      : "=m"(c->rsp), "=m"(mxcsr) : "c"(c->rsp) // FIXME prone to incur cache misses
      : "rax", "rdx", "rbx", "rsi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "cc", "memory");
    #if CONE_ASAN
        __sanitizer_finish_switch_fiber(fake_stack, &c->target_stack, &c->target_stack_size);
    #endif
    #if CONE_CXX
        *__cxa_get_globals() = cxa_globals;
    #endif
}

static void __attribute__((noreturn)) cone_body(struct cone *c) {
    #if CONE_ASAN
        __sanitizer_finish_switch_fiber(NULL, &c->target_stack, &c->target_stack_size);
    #endif
    #if CONE_CXX
        *__cxa_get_globals() = (struct __cxa_eh_globals){ NULL, 0 };
    #endif
    c->flags |= (c->body.code(c->body.data) ? CONE_FLAG_FAILED : 0) | CONE_FLAG_FINISHED;
    atomic_fetch_sub_explicit(&c->loop->active, 1, memory_order_acq_rel);
    cone_wake(&c->done, (size_t)-1);
    #if CONE_ASAN
        __sanitizer_start_switch_fiber(NULL, c->target_stack, c->target_stack_size);
    #endif
    __asm__("mov  %0, %%rsp  \n"
            "pop  %%rdi      \n"
            "pop  %%rbp      \n"
            "ret             \n" :: "r"(c->rsp));
    __builtin_unreachable();
}

static void cone_run(struct cone *c) {
    struct mun_error *ep = mun_set_error_storage(&c->error);
    struct cone *prev = cone;
    cone_switch(cone = c);
    cone = prev;
    mun_set_error_storage(ep);
    if (c->flags & CONE_FLAG_FINISHED)
        // Must be done after switching back to avoid use-after-free on a detached coroutine.
        cone_drop(c);
}

static struct cone *cone_spawn_on(struct cone_loop *loop, size_t size, struct cone_closure body) {
    size = (size + _Alignof(max_align_t) - 1) & ~(size_t)(_Alignof(max_align_t) - 1);
    struct cone *c = (struct cone *)malloc(sizeof(struct cone) + size);
    if (c == NULL)
        return (void)mun_error(memory, "no space for a stack"), NULL;
    c->flags = CONE_FLAG_SCHEDULED;
    c->loop = loop;
    c->body = body;
    c->runq = (struct cone_event_it){NULL, NULL, c};
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
    cone_runq_add(&loop->now, &c->runq);
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
        free(c);
    }
}

static struct cone_loop *cone_schedule(struct cone *c, int flags) {
    if (atomic_fetch_or(&c->flags, CONE_FLAG_SCHEDULED | flags) & (CONE_FLAG_SCHEDULED | CONE_FLAG_FINISHED))
        return NULL; // loop is already aware of this, don't ping
    struct cone_loop *loop = cone && cone->loop == c->loop ? NULL : c->loop;
    // This may cause the coroutine to be destroyed concurrently by its loop.
    // Meaning, accessing `c->loop` after the call returns is unsafe.
    cone_runq_add(&c->loop->now, &c->runq);
    return loop;
}

void cone_cancel(struct cone *c) {
    struct cone_loop *loop = cone_schedule(c, CONE_FLAG_CANCELLED);
    if (loop)
        cone_event_io_ping(&loop->io);
}

static int cone_ensure_running(struct cone *c) {
    int state = atomic_fetch_and(&c->flags, ~CONE_FLAG_CANCELLED & ~CONE_FLAG_TIMED_OUT & ~CONE_FLAG_WOKEN);
    return state & CONE_FLAG_CANCELLED ? mun_error(cancelled, "blocking call aborted")
         : state & CONE_FLAG_TIMED_OUT ? mun_error(timeout, "blocking call timed out") : 0;
}

// CONE_FLAG_SCHEDULED must only be removed once `cone_switch` is inevitable.
// Otherwise, the coroutine might get scheduled despite not being paused; if it then
// terminates without another blocking call, `cone_run` will use-after-free.
static void cone_deschedule(struct cone *c) {
    unsigned flags = c->flags;
    do if (flags & (CONE_FLAG_CANCELLED | CONE_FLAG_TIMED_OUT | CONE_FLAG_WOKEN))
        // There's a window between `cone_ensure_running` and `cone_deschedule` where
        // `cone_cancel`/`cone_wake` will set the flag, but not schedule. Since a resumption
        // callback has already been attached, we can't abort the switch anymore.
        return cone_runq_add(&c->loop->now, &c->runq);
    while (!atomic_compare_exchange_weak(&c->flags, &flags, flags & ~CONE_FLAG_SCHEDULED));
}

#define cone_pause(ev_add, ev_del) do {                           \
    if (cone_ensure_running(cone) || ev_add MUN_RETHROW)          \
        return -1;                                                \
    cone_deschedule(cone);                                        \
    if (cone_switch(cone), cone_ensure_running(cone) MUN_RETHROW) \
        return ev_del, -1;                                        \
    return 0;                                                     \
} while (0)

int cone_wait(struct cone_event *ev, const cone_atom *uptr, unsigned u) {
    struct cone_event_it it = { NULL, NULL, cone };
    cone_pause(({
        cone_lock(&ev->lk);
        *uptr == u ? (cone_event_add(ev, &it), cone_unlock(&ev->lk), 0)
                   : (cone_unlock(&ev->lk), mun_error(retry, "compare-and-sleep precondition failed"));
    }), ({
        cone_lock(&ev->lk);
        cone_event_del(ev, &it);
        cone_unlock(&ev->lk);
    }));
}

void cone_wake(struct cone_event *ev, size_t n) {
    // XXX check if nobody is waiting and don't even lock? (that's what FUTEX_WAKE does, dunno if it helps)
    cone_lock(&ev->lk);
    for (struct cone *c; n-- && (c = cone_event_pop(ev));) {
        // This should be done while holding the event lock so that the coroutine couldn't
        // be cancelled, scheduled, and executed to completion in this tiny space.
        struct cone_loop *loop = cone_schedule(c, CONE_FLAG_WOKEN);
        if (loop) {
            cone_unlock(&ev->lk);
            cone_event_io_ping(&loop->io);
            cone_lock(&ev->lk);
        }
    }
    cone_unlock(&ev->lk);
}

int cone_iowait(int fd, int write) {
    struct cone_event_fd ev = {fd, write, cone, NULL};
    cone_pause(cone_event_io_add(&cone->loop->io, &ev),
               cone_event_io_del(&cone->loop->io, &ev));
}

int cone_sleep_until(mun_usec t) {
    cone_pause(cone_event_schedule_add(&cone->loop->at, t, cone, 0),
               cone_event_schedule_del(&cone->loop->at, t, cone, 0));
}

int cone_yield(void) {
    return cone_sleep_until(mun_usec_monotonic()) MUN_RETHROW;
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

int cone_deadline(struct cone *c, mun_usec t) {
    return cone_event_schedule_add(&c->loop->at, t, c, 1) MUN_RETHROW;
}

void cone_complete(struct cone *c, mun_usec t) {
    cone_event_schedule_del(&c->loop->at, t, c, 1);
}

const cone_atom * cone_count(void) {
    return cone ? &cone->loop->active : NULL;
}

int cone_loop(size_t size, struct cone_closure body) {
    struct cone_loop loop = {};
    struct cone *c = cone_spawn_on(&loop, size, body);
    return c == NULL || cone_loop_run(&loop) || cone_join(c, 0) MUN_RETHROW;
}

static int cone_main_run(struct cone_loop *loop) {
    return mun_cant_fail(cone_loop_run(loop) MUN_RETHROW);
}

static struct cone_loop cone_main_loop = {};

static void __attribute__((constructor)) cone_main_init(void) {
    struct cone *c = cone_spawn_on(&cone_main_loop, CONE_DEFAULT_STACK, cone_bind(&cone_main_run, &cone_main_loop));
    mun_cant_fail(c == NULL MUN_RETHROW);
    cone_switch(c); // the loop will then switch back because the coroutine is scheduled to run
}

static void __attribute__((destructor)) cone_main_fini(void) {
    mun_assert(!cone || cone->loop->active == 1,
        "main() returned, but %u more coroutine(s) are still alive. They may attempt to use "
        "destroyed global data. main() should join all coroutines it spawns.", cone->loop->active - 1);
}
