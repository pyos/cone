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
struct /* __cxxabiv1:: */ __cxa_eh_globals {
    void* caughtExceptions;
    unsigned int uncaughtExceptions;
};

struct __cxa_eh_globals *__cxa_get_globals();
#endif

#if !CONE_EV_SELECT && !CONE_EV_EPOLL && !CONE_EV_KQUEUE
#define CONE_EV_EPOLL  (__linux__)
#define CONE_EV_KQUEUE (__APPLE__ || __FreeBSD__)
#define CONE_EV_SELECT (!CONE_EV_EPOLL && !CONE_EV_KQUEUE)
#elif (!!CONE_EV_SELECT + !!CONE_EV_EPOLL + !!CONE_EV_KQUEUE) != 1
#error "selected more than one of CONE_EV_*"
#endif

#if !__x86_64__
// There used to be a `sigaltstack`-based implementation, but it was crap.
_Static_assert(0, "stack switching is only supported on x86-64 UNIX");
#endif

#if CONE_EV_EPOLL
#include <sys/epoll.h>
#include <sys/timerfd.h>
#elif CONE_EV_KQUEUE
#include <sys/event.h>
#else
#include <sys/select.h>
#endif

enum {
    CONE_FLAG_LAST_REF  = 0x01,
    CONE_FLAG_SCHEDULED = 0x02,
    CONE_FLAG_WOKEN     = 0x04,
    CONE_FLAG_FINISHED  = 0x08,
    CONE_FLAG_FAILED    = 0x10,
    CONE_FLAG_CANCELLED = 0x20,
    CONE_FLAG_TIMED_OUT = 0x40,
    CONE_FLAG_JOINED    = 0x80,
    CONE_FLAG_NO_INTR   = 0x100,
};

static void cone_run(struct cone *);

// Returns the loop if it may need a ping to notice the change in its run queue.
// If it is known that the loop is not blocked in a syscall, this can be ignored.
static struct cone_loop *cone_schedule(struct cone *, int);

struct cone_event_schedule mun_vec(struct { mun_usec at; uintptr_t c; });

static int cone_event_schedule_add(struct cone_event_schedule *ev, mun_usec at, struct cone *c, int deadline) {
    return mun_vec_insert(ev, mun_vec_bisect(ev, at < _->at), &((mun_vec_type(ev)){at, (uintptr_t)c|deadline}));
}

static void cone_event_schedule_del(struct cone_event_schedule *ev, mun_usec at, struct cone *c, int deadline) {
    for (size_t i = mun_vec_bisect(ev, at < _->at); i-- && ev->data[i].at == at; )
        if (ev->data[i].c == ((uintptr_t)c|deadline))
            return mun_vec_erase(ev, i, 1);
}

static mun_usec cone_event_schedule_emit(struct cone_event_schedule *ev, size_t limit) {
    mun_usec t = mun_usec_monotonic();
    for (; limit-- && ev->size && ev->data->at <= t; mun_vec_erase(ev, 0, 1))
        cone_schedule((struct cone *)(ev->data->c & ~1ul), ev->data->c & 1 ? CONE_FLAG_TIMED_OUT : CONE_FLAG_WOKEN);
    // 0 is functionally equivalent to a timestamp in the past, but does not enable
    // the self-pipe, improving cross-thread synchronization performance a bit.
    return ev->size ? (ev->data->at <= t ? 0 : ev->data->at) : MUN_USEC_MAX;
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
    // This flag is set while waiting for I/O to tell the other threads that a write
    // to the self-pipe is needed to make this loop react to additions to the run queue.
    CONE_ATOMIC(char) interruptible;
    // Necessary structures:
    // * epoll - O(1) iterate over per-fd chain, O(1) find the chain to modify it: hashmap.
    // * select - O(n) iterate over all fds: list, but we're implementing a hashmap anyway...
    // * kqueue - nothing! it allows registering many (fd, ptr) pairs with the same fd
    #if !CONE_EV_KQUEUE
        size_t fdcnt;
        size_t fdcap;
        struct cone_event_fd **fds;
    #endif
    // *Also*, epoll does not support timeouts with granularity of less than 1ms.
    // Instead, we have to wait for reads on a high-resolution timerfd.
    #if CONE_EV_EPOLL
        int hrtimer;
    #endif
};

#if !CONE_EV_KQUEUE
// Must be a power of 2.
#define CONE_MIN_FDS_CAP 64

static unsigned inthash(unsigned key) {
    key = (key ^ 61) ^ (key >> 16);
    key = key + (key << 3);
    key = key ^ (key >> 4);
    key = key * 0x27d4eb2d;
    key = key ^ (key >> 15);
    return key;
}
#endif

static void cone_event_io_fini(struct cone_event_io *set) {
    if (set->poller >= 0)
        close(set->poller);
    if (set->selfpipe[0] >= 0)
        close(set->selfpipe[0]), close(set->selfpipe[1]);
    #if !CONE_EV_KQUEUE
        if (set->fds)
            free(set->fds);
    #endif
    #if CONE_EV_EPOLL
        if (set->hrtimer >= 0)
            close(set->hrtimer);
    #endif
}

static int cone_event_io_init(struct cone_event_io *set) {
    set->selfpipe[0] = set->selfpipe[1] = set->poller = -1;
    // XXX racy in forking multithreaded applications (see `man fcntl`); use pipe2 on linux?
    if (pipe(set->selfpipe) || fcntl(set->selfpipe[0], F_SETFD, FD_CLOEXEC)
                            || fcntl(set->selfpipe[1], F_SETFD, FD_CLOEXEC) MUN_RETHROW_OS)
        return cone_event_io_fini(set), -1;
    #if CONE_EV_EPOLL
        struct epoll_event ev = {EPOLLIN, {.ptr = NULL}};
        if ((set->poller = epoll_create1(EPOLL_CLOEXEC)) < 0
         || (set->hrtimer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC)) < 0
         || epoll_ctl(set->poller, EPOLL_CTL_ADD, set->selfpipe[0], &ev)
         || epoll_ctl(set->poller, EPOLL_CTL_ADD, set->hrtimer, &ev) MUN_RETHROW_OS)
            return cone_event_io_fini(set), -1;
    #endif
    #if CONE_EV_KQUEUE
        struct kevent ev = {set->selfpipe[0], EVFILT_READ, EV_ADD, 0, 0, NULL};
        if ((set->poller = kqueue()) < 0
         || fcntl(set->poller, F_SETFD, FD_CLOEXEC)
         || kevent(set->poller, &ev, 1, NULL, 0, NULL) MUN_RETHROW_OS)
            return cone_event_io_fini(set), -1;
    #else
        if ((set->fds = calloc(CONE_MIN_FDS_CAP, sizeof(struct cone_event_fd *))) == NULL)
            return cone_event_io_fini(set), mun_error(ENOMEM, "could not allocate an fd hash map");
        set->fdcap = CONE_MIN_FDS_CAP;
    #endif
    return 0;
}

static int cone_event_io_mod(struct cone_event_io *set, struct cone_event_fd *st, int add) {
    #if CONE_EV_KQUEUE
        uint16_t flags = (add ? EV_ADD|EV_ONESHOT : EV_DELETE)|EV_UDATA_SPECIFIC;
        struct kevent ev = {st->fd, st->write ? EVFILT_WRITE : EVFILT_READ, flags, 0, 0, st};
        return kevent(set->poller, &ev, 1, NULL, 0, NULL) && (add || errno != ENOENT) MUN_RETHROW_OS;
    #else
        struct cone_event_fd **b = &set->fds[inthash(st->fd) & (set->fdcap - 1)];
        while (*b && (*b)->fd != st->fd)
            b = &(*b)->link;
        struct cone_event_fd **c = b;
        if (add) {
            st->link = *b;
            *b = st;
        } else {
            for (; *c != st; c = &(*c)->link)
                if (!*c || (*c)->fd != st->fd)
                    return 0;
            *c = st->link;
        }
        int is_add = add && !st->link;
        int is_del = !add && c == b && (!st->link || st->link->fd != st->fd);
        #if CONE_EV_EPOLL
            struct epoll_event ev = {0, {.ptr = *b}};
            for (struct cone_event_fd *e = *b; e && e->fd == st->fd; e = e->link)
                ev.events |= e->write ? EPOLLOUT : EPOLLIN|EPOLLRDHUP;
            int op = is_add ? EPOLL_CTL_ADD : is_del ? EPOLL_CTL_DEL : EPOLL_CTL_MOD;
            if (epoll_ctl(set->poller, op, st->fd, &ev) MUN_RETHROW_OS)
                return *c = (add ? st->link : st), -1;
        #endif

        set->fdcnt += is_add - is_del;
        size_t rehash = set->fdcnt * 5 > set->fdcap * 6 ? set->fdcap * 2
                      : set->fdcnt * 2 < set->fdcap * 1 ? set->fdcap / 2 : 0;
        if (rehash >= CONE_MIN_FDS_CAP) {
            struct cone_event_fd **m = calloc(rehash, sizeof(struct cone_event_fd *));
            if (!m)
                return 0; // ignore memory error and accept decreased performance
            for (size_t i = 0; i < set->fdcap; i++) {
                for (struct cone_event_fd *p = set->fds[i]; p;) {
                    struct cone_event_fd **h = &m[inthash(p->fd) & (rehash - 1)];
                    struct cone_event_fd *q = p;
                    struct cone_event_fd *r = p->link;
                    while (r && r->fd == p->fd)
                        q = r, r = r->link;
                    q->link = *h, *h = p, p = r;
                }
            }
            free(set->fds);
            set->fds = m;
            set->fdcap = rehash;
        }
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
    if (atomic_exchange(&set->interruptible, 0))
        write(set->selfpipe[1], "", 1);
}

static void cone_event_io_allow_ping(struct cone_event_io *set) {
    set->interruptible = 1; // seq-cst so that it is not reordered after `cone_runq_is_empty`
}

static void cone_event_io_consume_ping(struct cone_event_io *set) {
    if (!atomic_exchange(&set->interruptible, 0))
        read(set->selfpipe[0], (char[4]){}, 4);
}

static int cone_event_io_emit(struct cone_event_io *set, mun_usec deadline) {
    #if !CONE_EV_EPOLL
        mun_usec now = mun_usec_monotonic();
        mun_usec timeout = now > deadline ? 0 : deadline - now;
        if (timeout > 60000000ll)
            timeout = 60000000ll;
    #endif
    #if CONE_EV_SELECT
        fd_set fds[2] = {};
        FD_SET(set->selfpipe[0], &fds[0]);
        int max_fd = set->selfpipe[0] + 1;
        for (size_t i = 0; i < set->fdcap; i++) {
            for (struct cone_event_fd *e = set->fds[i]; e; e = e->link) {
                if (max_fd <= e->fd)
                    max_fd = e->fd + 1;
                FD_SET(e->fd, &fds[!!e->write]);
            }
        }
        struct timeval us = {timeout / 1000000ull, timeout % 1000000ull};
        int got = select(max_fd, &fds[0], &fds[1], NULL, &us);
    #elif CONE_EV_EPOLL
        struct epoll_event evs[64];
        struct itimerspec its = {.it_value = {deadline / 1000000ull, deadline % 1000000ull * 1000}};
        if (deadline && timerfd_settime(set->hrtimer, TFD_TIMER_ABSTIME, &its, NULL) < 0 MUN_RETHROW_OS)
            return -1;
        int got = epoll_wait(set->poller, evs, 64, deadline ? -1 : 0);
    #elif CONE_EV_KQUEUE
        struct kevent evs[64];
        struct timespec ns = {timeout / 1000000ull, timeout % 1000000ull * 1000};
        int got = kevent(set->poller, NULL, 0, evs, 64, &ns);
    #endif
    if (got < 0 && errno != EINTR MUN_RETHROW_OS)
        return -1;
    if (deadline != 0) // else pings were not allowed in the first place (see `cone_loop_run`).
        cone_event_io_consume_ping(set);
    #if CONE_EV_SELECT
        for (size_t i = 0; i < set->fdcap; i++)
            for (struct cone_event_fd *e = set->fds[i]; e; e = e->link)
                if (FD_ISSET(e->fd, &fds[!!e->write]))
                    cone_event_io_del(set, e), cone_schedule(e->c, CONE_FLAG_WOKEN);
    #elif CONE_EV_EPOLL
        for (int i = 0; i < got; i++)
            for (struct cone_event_fd *st = evs[i].data.ptr, *e = st; e && e->fd == st->fd; e = e->link)
                if (evs[i].events & ((e->write ? EPOLLOUT : EPOLLIN|EPOLLRDHUP)|EPOLLERR|EPOLLHUP))
                    cone_event_io_del(set, e), cone_schedule(e->c, CONE_FLAG_WOKEN);
    #elif CONE_EV_KQUEUE
        for (int i = 0; i < got; i++)
            if (evs[i].udata) // oneshot event, removed automatically
                cone_schedule(((struct cone_event_fd *)evs[i].udata)->c, CONE_FLAG_WOKEN);
    #endif
    return 0;
}

struct cone_runq_it {
    CONE_ATOMIC(struct cone_runq_it *) next;
};

struct cone_runq {
    CONE_ATOMIC(struct cone_runq_it *) head;
    CONE_ATOMIC(mun_usec) delay;
    struct cone_runq_it *tail;
    struct cone_runq_it stub;
    mun_usec prev;
};

static void cone_runq_add(struct cone_runq *rq, struct cone_runq_it *it) {
    atomic_store_explicit(&it->next, NULL, memory_order_relaxed);
    // *Almost* wait-free - blocking between xchg and store blocks the consumer too.
    atomic_store_explicit(&atomic_exchange(&rq->head, it)->next, it, memory_order_release);
}

static int cone_runq_is_empty(struct cone_runq *rq) {
    return rq->tail == &rq->stub && !atomic_load_explicit(&rq->stub.next, memory_order_acquire);
}

static struct cone *cone_runq_next(struct cone_runq *rq) {
    struct cone_runq_it *tail = rq->tail;
    struct cone_runq_it *next = atomic_load_explicit(&tail->next, memory_order_acquire);
    if (tail == &rq->stub) {
        mun_usec now = mun_usec_monotonic();
        mun_usec old = atomic_load_explicit(&rq->delay, memory_order_relaxed);
        if (next == NULL) {
            rq->prev = 0;
            atomic_store_explicit(&rq->delay, old * 3 / 4, memory_order_relaxed);
            return NULL; // empty or blocked while pushing first element
        }
        if (rq->prev)
            atomic_store_explicit(&rq->delay, old * 3 / 4 + (now - rq->prev) / 4, memory_order_relaxed);
        rq->prev = now;
        cone_runq_add(rq, &rq->stub);
        tail = rq->tail = next;
        next = atomic_load_explicit(&tail->next, memory_order_acquire);
    }
    if (!next)
        return NULL; // blocked while pushing next element
    rq->tail = next;
    return (struct cone *)tail;
}

struct cone_loop {
    CONE_ATOMIC(unsigned) active;
    struct cone_runq now;
    struct cone_event_io io;
    struct cone_event_schedule at;
};

static int cone_loop_init(struct cone_loop *loop) {
    atomic_store_explicit(&loop->now.head, loop->now.tail = &loop->now.stub, memory_order_relaxed);
    return cone_event_io_init(&loop->io) MUN_RETHROW;
}

static void cone_loop_run(struct cone_loop *loop) {
    for (struct cone *c;;) {
        for (size_t limit = 256; limit-- && (c = cone_runq_next(&loop->now));)
            cone_run(c);
        mun_usec next = cone_event_schedule_emit(&loop->at, 256);
        if (next == MUN_USEC_MAX && !atomic_load_explicit(&loop->active, memory_order_acquire))
            break;
        if (next > 0) {
            cone_event_io_allow_ping(&loop->io);
            if (!cone_runq_is_empty(&loop->now))
                cone_event_io_consume_ping(&loop->io), next = 0;
            // else the paired `cone_event_io_consume_ping` is in `cone_event_io_emit`.
        }
        // If this fails, coroutines will get leaked.
        mun_cant_fail(cone_event_io_emit(&loop->io, next) MUN_RETHROW);
    }
    cone_event_io_fini(&loop->io);
    mun_vec_fini(&loop->at);
}

struct cone {
    struct cone_runq_it runq;
    CONE_ATOMIC(unsigned) flags;
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
    cone_wake(&c->done, (size_t)-1, 0);
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
        return (void)mun_error(ENOMEM, "no space for a stack"), NULL;
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
    atomic_fetch_add_explicit(&loop->active, 1, memory_order_relaxed);
    cone_runq_add(&loop->now, &c->runq);
    return c;
}

struct cone *cone_spawn(size_t size, struct cone_closure body) {
    return cone_spawn_on(cone->loop, size, body);
}

struct cone *cone_spawn_at(struct cone *c, size_t size, struct cone_closure body) {
    struct cone *n = cone_spawn_on(c->loop, size, body);
    if (!n MUN_RETHROW)
        return NULL;
    cone_event_io_ping(&n->loop->io);
    return n;
}

void cone_drop(struct cone *c) {
    if (c && (atomic_fetch_xor(&c->flags, CONE_FLAG_LAST_REF) & CONE_FLAG_LAST_REF)) {
        if ((c->flags & (CONE_FLAG_FAILED | CONE_FLAG_JOINED)) == CONE_FLAG_FAILED)
            if (c->error.code != ECANCELED)
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

static int cone_deschedule(struct cone *c) {
    unsigned flags = c->flags;
    // Don't even yield if cancelled by another thread while registering the wakeup callback.
    while (!(flags & CONE_FLAG_WOKEN) && (flags & CONE_FLAG_NO_INTR || !(flags & (CONE_FLAG_CANCELLED | CONE_FLAG_TIMED_OUT))))
        if (atomic_compare_exchange_weak(&c->flags, &flags, flags & ~CONE_FLAG_SCHEDULED))
            cone_switch(c), flags = c->flags;
    if (flags & CONE_FLAG_NO_INTR) {
        c->flags &= ~CONE_FLAG_WOKEN;
        return 0;
    }
    int state = atomic_fetch_and(&c->flags, ~CONE_FLAG_WOKEN & ~CONE_FLAG_CANCELLED & ~CONE_FLAG_TIMED_OUT);
    return state & CONE_FLAG_CANCELLED ? mun_error(ECANCELED, "blocking call aborted")
         : state & CONE_FLAG_TIMED_OUT ? mun_error(ETIMEDOUT, "blocking call timed out") : 0;
}

struct cone_event_it {
    struct cone_event_it *next, *prev;
    struct cone *c;
    int v;
};

static _Thread_local struct cone_mcs_lock {
    CONE_ATOMIC(struct cone_mcs_lock *) next;
    CONE_ATOMIC(char) locked;
} lki;

#ifndef CONE_SPIN_INTERVAL
#define CONE_SPIN_INTERVAL 512
#endif

static void cone_tx_lock(struct cone_event *ev) {
    struct cone_mcs_lock *p = atomic_exchange(&ev->lk, &lki);
    if (!p)
        return;
    atomic_store_explicit(&lki.locked, 1, memory_order_relaxed);
    atomic_store_explicit(&p->next, &lki, memory_order_release);
    for (size_t __n = 0; atomic_load_explicit(&lki.locked, memory_order_acquire);)
        if (++__n % CONE_SPIN_INTERVAL) __asm__ __volatile__("pause"); else sched_yield();
}

static void cone_tx_unlock(struct cone_event *ev) {
    if (atomic_compare_exchange_strong(&ev->lk, &(void *){&lki}, NULL))
        return;
    struct cone_mcs_lock *n;
    for (size_t __n = 0; !(n = atomic_load_explicit(&lki.next, memory_order_acquire));)
        if (++__n % CONE_SPIN_INTERVAL) __asm__ __volatile__("pause"); else sched_yield();
    atomic_store_explicit(&n->locked, 0, memory_order_release);
    atomic_store_explicit(&lki.next, NULL, memory_order_relaxed);
}

void cone_tx_begin(struct cone_event *ev) {
    // The increment must be a seq-cst operation so that it is not reordered with
    // the contents of the transaction.
    cone_tx_lock(ev), ev->w++;
}

void cone_tx_end(struct cone_event *ev) {
    cone_tx_unlock(ev), ev->w--;
}

int cone_tx_wait(struct cone_event *ev) {
    struct cone_event_it it = { NULL, ev->tail, cone, -1 };
    ev->tail ? (it.prev->next = &it) : (ev->head = &it);
    ev->tail = &it;
    cone_tx_unlock(ev);
    if (cone_deschedule(cone) MUN_RETHROW) {
        cone_tx_lock(ev);
        if (it.v < 0) {
            ev->w--;
            it.prev ? (it.prev->next = it.next) : (ev->head = it.next);
            it.next ? (it.next->prev = it.prev) : (ev->tail = it.prev);
        }
        cone_tx_unlock(ev);
        return it.v < 0 ? -1 : ~it.v;
    }
    return it.v;
}

size_t cone_wake(struct cone_event *ev, size_t n, int ret) {
    size_t r = 0;
    if (!n || !atomic_load_explicit(&ev->w, memory_order_acquire))
        return 0; // serialized before any `cone_tx_begin`
    cone_tx_lock(ev);
    for (struct cone_event_it *it; n-- && (it = ev->head); r++) {
        ev->w--;
        ev->head = it->next;
        it->next ? (it->next->prev = it->prev) : (ev->tail = it->prev);
        it->v = ret < 0 ? 0 : ret;
        struct cone_loop *loop = cone_schedule(it->c, CONE_FLAG_WOKEN);
        // The coroutine may be concurrently cancelled, so everything above must happen
        // before the `if (cone_deschedule)` branch's contents in `cone_tx_wait`.
        if (loop) {
            int should_continue = n && ev->head;
            // This unlock introduces an anomaly in `cone_cancel`: if A and B are waiting
            // on this event and they are cancelled in the same order while we're busy
            // pinging a loop of some coroutine between them, it'll look like B was
            // cancelled before `cone_wake`, while A was cancelled after.
            cone_tx_unlock(ev);
            cone_event_io_ping(&loop->io);
            if (!should_continue)
                // Another item might have been added already, but we don't care, we
                // pretend to see the old state.
                return r + 1;
            cone_tx_lock(ev);
        }
    }
    cone_tx_unlock(ev);
    return r;
}

int cone_try_lock(struct cone_mutex *m) {
    return atomic_exchange(&m->lk, 1) ? mun_error(EAGAIN, "mutex already locked") : 0;
}

int cone_lock(struct cone_mutex *m) {
    int r = 0;
    // 0 = xchg succeeded, 1 = fair handoff, 2 = retry xchg
    if (atomic_exchange(&m->lk, 1)) while ((r = cone_wait(&m->e, atomic_exchange(&m->lk, 1))) == 2) {}
    if (r < 0 MUN_RETHROW) {
        if (r == ~1) // acquired the lock by direct handoff, but also cancelled
            cone_unlock(m, 1);
        if (r == ~2) // signal more to retry in case this was the last one woken
            cone_wake(&m->e, 1, 2);
        return -1;
    }
    return 0;
}

int cone_unlock(struct cone_mutex *m, int fair) {
    if (fair && cone_wake(&m->e, 1, 1))
        return 1;
    // (Some waiters may queue here, so wake(n, 2) after a store is needed even on a fair unlock.)
    atomic_store_explicit(&m->lk, 0, memory_order_release);
    // XXX if the critical section rarely yields, waking one by one leaves huge gaps
    //     in the run queue where another coroutine may barge in. This isn't just unfair,
    //     this is super unfair.
    return cone_wake(&m->e, 1, 2);
}

int cone_iowait(int fd, int write) {
    struct cone_event_fd ev = {fd, write, cone, NULL};
    if (cone_event_io_add(&cone->loop->io, &ev) MUN_RETHROW)
        return -1;
    if (cone_deschedule(cone) MUN_RETHROW)
        return cone_event_io_del(&cone->loop->io, &ev), -1;
    return 0;
}

int cone_sleep_until(mun_usec t) {
    if (cone_event_schedule_add(&cone->loop->at, t, cone, 0) MUN_RETHROW)
        return -1;
    if (cone_deschedule(cone) MUN_RETHROW)
        return cone_event_schedule_del(&cone->loop->at, t, cone, 0), -1;
    return 0;
}

int cone_cowait(struct cone *c, int norethrow) {
    if (c == cone) // maybe detect more complicated deadlocks too?..
        return mun_error(EDEADLK, "coroutine waiting on itself");
    if (!(c->flags & CONE_FLAG_FINISHED) && cone_wait(&c->done, !(c->flags & CONE_FLAG_FINISHED)) MUN_RETHROW)
        return -1;
    // XXX the ordering here doesn't actually matter.
    if (!norethrow && atomic_fetch_or(&c->flags, CONE_FLAG_JOINED) & CONE_FLAG_FAILED)
        return *mun_last_error() = c->error, mun_error_up(MUN_CURRENT_FRAME);
    return 0;
}

int cone_intr(int enable) {
    int prev = enable ? atomic_fetch_and(&cone->flags, ~CONE_FLAG_NO_INTR)
                      : atomic_fetch_or(&cone->flags, CONE_FLAG_NO_INTR);
    return !(prev & CONE_FLAG_NO_INTR);
}

void cone_cancel(struct cone *c) {
    struct cone_loop *loop = cone_schedule(c, CONE_FLAG_CANCELLED);
    if (loop)
        cone_event_io_ping(&loop->io);
}

int cone_deadline(struct cone *c, mun_usec t) {
    // XXX were the user required to supply a storage, this could be a bst...
    return cone_event_schedule_add(&c->loop->at, t, c, 1) MUN_RETHROW;
}

void cone_complete(struct cone *c, mun_usec t) {
    cone_event_schedule_del(&c->loop->at, t, c, 1);
}

const CONE_ATOMIC(unsigned) *cone_count(void) {
    return cone ? &cone->loop->active : NULL;
}

const CONE_ATOMIC(mun_usec) *cone_delay(void) {
    return cone ? &cone->loop->now.delay : NULL;
}

static int cone_fork(struct cone_loop *loop) {
    return cone_loop_run(loop), free(loop), 0;
}

struct cone *cone_loop(size_t size, struct cone_closure body, int (*run)(struct cone_closure)) {
    struct cone_loop *loop = calloc(sizeof(struct cone_loop), 1);
    if (loop == NULL || cone_loop_init(loop) MUN_RETHROW_OS)
        return free(loop), NULL;
    struct cone *c = cone_spawn_on(loop, size, body);
    if (c == NULL MUN_RETHROW)
        return free(loop), NULL;
    if (run(cone_bind(&cone_fork, loop)) MUN_RETHROW)
        return free(loop), free(c), NULL;
    return c;
}

static int cone_main_run(struct cone_loop *loop) {
    return cone_loop_run(loop), 0;
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
    // Sure, there's the priority parameter of __attribute__((destructor)), but we can't
    // guarantee nobody else will use it. Also, we can't wait for other threads here.
}
