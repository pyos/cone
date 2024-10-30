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
} *__cxa_get_globals();
#endif

#if !CONE_EV_SELECT && !CONE_EV_EPOLL && !CONE_EV_KQUEUE
#define CONE_EV_EPOLL  (__linux__)
#define CONE_EV_KQUEUE (__APPLE__ || __FreeBSD__)
#elif (!!CONE_EV_SELECT + !!CONE_EV_EPOLL + !!CONE_EV_KQUEUE) != 1
#error "selected more than one of CONE_EV_*"
#endif

#if !CONE_ASM_X64 && !CONE_ASM_ARM64
#define CONE_ASM_X64 (__x86_64__)
#define CONE_ASM_ARM64 (__aarch64__)
#elif (!!CONE_ASM_X64 + !!CONE_ASM_ARM64) != 1
#error "selected more than one of CONE_ASM_*"
#endif

#if !CONE_ASM_X64 && !CONE_ASM_ARM64
// There used to be a `sigaltstack`-based implementation, but it was crap.
#error "unsupported platform; has to be x86-64 or arm64"
#endif

#if CONE_EV_EPOLL
#include <sys/epoll.h>
#elif CONE_EV_KQUEUE
#include <sys/event.h>
#else
#include <sys/select.h>
#endif

#if CONE_ASM_X64
#define CONE_STACK_ALIGN _Alignof(max_align_t)
#elif CONE_ASM_ARM64
#define CONE_STACK_ALIGN 16
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

struct cone_event_schedule mun_vec(struct { mun_usec at; struct cone *c; });

static int cone_event_schedule_add(struct cone_event_schedule *ev, mun_usec at, struct cone *c, int deadline) {
    return mun_vec_insert(ev, mun_vec_bisect(ev, at < _->at), &((mun_vec_type(ev)){at, mun_tag_add(c, !!deadline)}));
}

static void cone_event_schedule_del(struct cone_event_schedule *ev, mun_usec at, struct cone *c, int deadline) {
    for (size_t i = mun_vec_bisect(ev, at < _->at); i-- && ev->data[i].at == at; )
        if (ev->data[i].c == mun_tag_add(c, !!deadline))
            return mun_vec_erase(ev, i, 1);
}

static mun_usec cone_event_schedule_emit(struct cone_event_schedule *ev, size_t limit) {
    if (limit > ev->size)
        limit = ev->size;
    size_t i = 0;
    // Could've spent a while pushing into the queue, so if the check fails once, re-read the timer.
    for (mun_usec t = 0; i < limit && (ev->data[i].at <= t || (ev->data[i].at <= (t = mun_usec_monotonic()))); i++)
        cone_schedule(mun_tag_ptr_aligned(ev->data[i].c, CONE_STACK_ALIGN),
                      mun_tag_get_aligned(ev->data[i].c, CONE_STACK_ALIGN) ? CONE_FLAG_TIMED_OUT : CONE_FLAG_WOKEN);
    mun_vec_erase(ev, 0, i);
    return i ? 0 : ev->size ? ev->data->at : MUN_USEC_MAX;
}

struct cone_event_fd {
    int fd;
    int flags;
    struct cone *c;
    struct cone_event_fd *link;
};

enum { IO_R = 1, IO_W = 2, IO_RW = 3 };

struct cone_event_io {
    int poller;
    int selfpipe[2];
    // This flag is set while waiting for I/O to tell the other threads that a write
    // to the self-pipe is needed to make this loop react to additions to the run queue.
    CONE_ATOMIC(char) interruptible;
    // epoll and select deduplicate events by file descriptor, so we need a hash map from file
    // descriptors to linked lists of listeners.
    size_t keys;
    size_t capacity;
    struct cone_event_fd **buckets;
};

// Must be a power of 2.
#define CONE_MIN_FDS_CAP 64

static void cone_event_io_fini(struct cone_event_io *set) {
    if (set->poller >= 0)
        close(set->poller);
    if (set->selfpipe[0] >= 0)
        close(set->selfpipe[0]), close(set->selfpipe[1]);
    if (set->buckets)
        free(set->buckets);
}

static int cone_event_io_init(struct cone_event_io *set) {
    set->selfpipe[0] = set->selfpipe[1] = set->poller = -1;
    // XXX racy in forking multithreaded applications (see `man fcntl`); use pipe2 on linux?
    if (pipe(set->selfpipe) || fcntl(set->selfpipe[0], F_SETFD, FD_CLOEXEC)
                            || fcntl(set->selfpipe[1], F_SETFD, FD_CLOEXEC) MUN_RETHROW_OS)
        return cone_event_io_fini(set), -1;
    #if CONE_EV_KQUEUE
        struct kevent ev = {set->selfpipe[0], EVFILT_READ, EV_ADD, 0, 0, NULL};
        if ((set->poller = kqueue()) < 0
         || fcntl(set->poller, F_SETFD, FD_CLOEXEC)
         || kevent(set->poller, &ev, 1, NULL, 0, NULL) MUN_RETHROW_OS)
            return cone_event_io_fini(set), -1;
    #elif CONE_EV_EPOLL
        struct epoll_event ev = {EPOLLIN, {.fd = set->selfpipe[0]}};
        if ((set->poller = epoll_create1(EPOLL_CLOEXEC)) < 0
         || epoll_ctl(set->poller, EPOLL_CTL_ADD, set->selfpipe[0], &ev) MUN_RETHROW_OS)
            return cone_event_io_fini(set), -1;
    #endif
    if ((set->buckets = calloc(CONE_MIN_FDS_CAP, sizeof(struct cone_event_fd *))) == NULL)
        return cone_event_io_fini(set), mun_error(ENOMEM, "could not allocate an fd hash map");
    set->capacity = CONE_MIN_FDS_CAP;
    return 0;
}

static unsigned inthash(unsigned key) {
    key = (key ^ 61) ^ (key >> 16);
    key = key + (key << 3);
    key = key ^ (key >> 4);
    key = key * 0x27d4eb2d;
    key = key ^ (key >> 15);
    return key;
}

static struct cone_event_fd **cone_hash_find(struct cone_event_io *set, int fd) {
    struct cone_event_fd **r = &set->buckets[inthash(fd) & (set->capacity - 1)];
    while (*r && (*r)->fd != fd)
        r = &(*r)->link;
    return r;
}

static void cone_hash_update_size(struct cone_event_io *set, int delta) {
    size_t size = set->keys += delta;
    size_t capacity = set->capacity;
    while (size * 5 > capacity * 6) capacity *= 2;
    while (size * 2 < capacity * 1 && capacity > CONE_MIN_FDS_CAP) capacity /= 2;
    if (capacity == set->capacity) return;

    struct cone_event_fd **m = calloc(capacity, sizeof(struct cone_event_fd *));
    if (!m) return; // ignore memory error and accept decreased performance
    for (size_t i = 0; i < set->capacity; i++) {
        for (struct cone_event_fd *p = set->buckets[i]; p; ) {
            struct cone_event_fd **bucket = &m[inthash(p->fd) & (capacity - 1)];
            struct cone_event_fd *a = p, *b = p;
            while (b->link && b->link->fd == b->fd) b = b->link;
            p = b->link, b->link = *bucket, *bucket = a;
        }
    }
    free(set->buckets);
    set->buckets = m;
    set->capacity = capacity;
}

static int cone_event_io_set_mode(struct cone_event_io *set, int fd, int from, int to) {
    if (from == to) return 0;
    #if CONE_EV_KQUEUE
        int rflag = (to & IO_R) > (from & IO_R) ? EV_ADD : (to & IO_R) < (from & IO_R) ? EV_DELETE : 0;
        int wflag = (to & IO_W) > (from & IO_W) ? EV_ADD : (to & IO_W) < (from & IO_W) ? EV_DELETE : 0;
        struct kevent evs[] = {{fd, EVFILT_READ, rflag, 0, 0, NULL}, {fd, EVFILT_WRITE, wflag, 0, 0, NULL}};
        return kevent(set->poller, &evs[!rflag], !!rflag + !!wflag, NULL, 0, NULL);
    #elif CONE_EV_EPOLL
        int op = !from ? EPOLL_CTL_ADD : !to ? EPOLL_CTL_DEL : EPOLL_CTL_MOD;
        int flags = (to & IO_R ? EPOLLIN|EPOLLRDHUP : 0) | (to & IO_W ? EPOLLOUT : 0);
        return epoll_ctl(set->poller, op, fd, &(struct epoll_event){flags, {.fd = fd}});
    #else
        return (void)set, (void)fd, 0;
    #endif
}

static int cone_event_io_schedule_all(struct cone_event_io *set, int fd, int flags) {
    struct cone_event_fd **bucket = cone_hash_find(set, fd);
    struct cone_event_fd **it = bucket;
    if (!*bucket) return 0; // nothing was listening for this event
    int from = 0, to = 0;
    for (struct cone_event_fd *e = *bucket; e && e->fd == fd; e = e->link) {
        from |= e->flags;
        if (e->flags & flags) {
            *it = e->link;
            cone_schedule(e->c, CONE_FLAG_WOKEN);
        } else {
            it = &e->link;
            to |= e->flags;
        }
    }
    mun_cant_fail(cone_event_io_set_mode(set, fd, from, to) MUN_RETHROW_OS);
    return it == bucket;
}

static int cone_event_io_add(struct cone_event_io *set, struct cone_event_fd *st) {
    struct cone_event_fd **bucket = cone_hash_find(set, st->fd);
    int from = 0;
    for (struct cone_event_fd *e = *bucket; e && e->fd == st->fd; e = e->link)
        from |= e->flags;
    if (cone_event_io_set_mode(set, st->fd, from, from|st->flags) MUN_RETHROW_OS)
        return -1;
    st->link = *bucket, *bucket = st;
    if (!st->link) cone_hash_update_size(set, 1);
    return 0;
}

static int cone_event_io_del(struct cone_event_io *set, struct cone_event_fd *st) {
    struct cone_event_fd **bucket = cone_hash_find(set, st->fd);
    struct cone_event_fd **it = bucket;
    for (; *it != st; it = &(*it)->link)
        if (!*it || (*it)->fd != st->fd)
            return 0;
    int to = 0;
    for (struct cone_event_fd *e = *bucket; e && e->fd == st->fd; e = e->link)
        if (e != st) to |= e->flags;
    if (cone_event_io_set_mode(set, st->fd, to|st->flags, to) MUN_RETHROW_OS)
        return -1;
    *it = st->link;
    if (it == bucket && (!st->link || st->link->fd != st->fd))
        cone_hash_update_size(set, -1);
    return 0;
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
    if (deadline == 0 && !set->keys) return 0;
    mun_usec now = mun_usec_monotonic();
    mun_usec timeout = now > deadline ? 0 : deadline - now;
    if (timeout > 60000000ll)
        timeout = 60000000ll;
    struct timespec ns = {timeout / 1000000ull, timeout % 1000000ull * 1000};
    #if CONE_EV_KQUEUE
        struct kevent evs[64];
        int n = kevent(set->poller, NULL, 0, evs, 64, &ns);
    #elif CONE_EV_EPOLL
        struct epoll_event evs[64];
        int n = epoll_pwait2(set->poller, evs, 64, &ns, NULL);
    #else
        fd_set rset = {}, wset = {};
        FD_SET(set->selfpipe[0], &rset);
        int max_fd = set->selfpipe[0];
        for (size_t i = 0; i < set->capacity; i++) {
            for (struct cone_event_fd *e = set->buckets[i]; e; e = e->link) {
                if (max_fd < e->fd) max_fd = e->fd;
                if (e->flags & IO_R) FD_SET(e->fd, &rset);
                if (e->flags & IO_W) FD_SET(e->fd, &wset);
            }
        }
        int n = pselect(max_fd + 1, &rset, &wset, NULL, &ns, NULL);
    #endif
    if (n < 0 && errno != EINTR MUN_RETHROW_OS)
        return -1;
    if (deadline != 0) // else pings were not allowed in the first place (see `cone_loop_run`).
        // This *could* also be done after this function returns, but doing it immediately
        // after the syscall reduces redundant pings.
        cone_event_io_consume_ping(set);
    int removed_from_map = 0;
    for (int i = 0; i < n; i++) {
        #if CONE_EV_KQUEUE
            int fd = evs[i].ident;
            int flags = evs[i].filter == EVFILT_WRITE ? IO_W : IO_R;
        #elif CONE_EV_EPOLL
            int fd = evs[i].data.fd;
            int flags = (evs[i].events & (EPOLLIN|EPOLLRDHUP|EPOLLERR|EPOLLHUP) ? IO_R : 0)
                      | (evs[i].events & (EPOLLOUT|EPOLLERR|EPOLLHUP) ? IO_W : 0);
        #else
            int fd = i;
            int flags = (FD_ISSET(fd, &rset) ? IO_R : 0) | (FD_ISSET(fd, &wset) ? IO_W : 0);
            n += 1 - !!flags - (flags == IO_RW); // `n` counts events; this maps it to file descriptors
        #endif
        if (flags) removed_from_map += cone_event_io_schedule_all(set, fd, flags);
    }
    cone_hash_update_size(set, -removed_from_map);
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
    // TODO: ARM64 says release/acquire for head and next do not work?...
    atomic_store(&atomic_exchange(&rq->head, it)->next, it);
}

static int cone_runq_is_empty(struct cone_runq *rq) {
    return rq->tail == &rq->stub && atomic_load(&rq->head) == &rq->stub;
}

static struct cone *cone_runq_next(struct cone_runq *rq) {
    struct cone_runq_it *tail = rq->tail;
    struct cone_runq_it *next = atomic_load(&tail->next);
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
        next = atomic_load(&tail->next);
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
    atomic_store_explicit(&loop->now.head, loop->now.tail = &loop->now.stub, memory_order_release);
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
            if (!cone_runq_is_empty(&loop->now)) // must be checked *after* enabling pings
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
    _Alignas(CONE_STACK_ALIGN) char stack[];
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
    #if CONE_ASM_X64
        __asm__("mov  %0, %%rax    \n"
                "add  $-128, %%rsp \n" // avoid the red zone
                "stmxcsr -32(%%rsp)\n"
                "jmp  %=0f         \n" // jmp-call + ret is smaller than lea-push + pop-jmp
        "%=1:"  "push %%rbp        \n"
                "push %%rdi        \n"
                "mov  %%rsp, %0    \n" // `xchg` is implicitly `lock`ed; 3 `mov`s are faster
                "mov  %%rax, %%rsp \n"
                "pop  %%rdi        \n" // FIXME prone to incur cache misses
                "pop  %%rbp        \n"
                "ret               \n"
        "%=0:"  "call %=1b         \n"
                "ldmxcsr -32(%%rsp)\n"
                "sub  $-128, %%rsp \n"
          : "=m"(c->rsp) :
          // Preserved: rdi (first argument), rbp (frame pointer), rsp (stack pointer)
          // The remaining registers could be clobbered by the coroutine's body:
          : "rax", "rcx", "rdx", "rbx", "rsi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "cc", "memory");
    #elif CONE_ASM_ARM64
        register void *sp __asm__("x0") = &c->rsp;
        __asm__("ldr  x10,      [%0]     \n"
                "mrs  x13, fpcr          \n"
                "sub  x11,  sp, 48       \n"
                "adr  x12, %=0f          \n"
                "stp   x0, x29, [x11]    \n"
                "stp  x12, x30, [x11,16] \n"
                "stp  x13, x18, [x11,32] \n"
                "str  x11,      [%0]     \n"
                "ldp   x0, x29, [x10]    \n"
                "ldp  x12, x30, [x10,16] \n"
                "add   sp, x10, 32       \n"
                "br   x12                \n"
        "%=0:"  "ldp  x13, x18, [sp], 16 \n"
                "msr  fpcr, x13          \n"
          :: "r"(sp)
          // Preserved: x0 (first argument), x18 (platform-defined), x29 (frame pointer), x30 (return address), x31 (stack pointer)
          // The remaining registers could be clobbered by the coroutine's body:
          :         "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",  "x8",  "x9", "x10", "x11", "x12", "x13", "x14", "x15"
          , "x16", "x17",        "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28"
          ,  "v0",  "v1",  "v2",  "v3",  "v4",  "v5",  "v6",  "v7",  "v8",  "v9", "v10", "v11", "v12", "v13", "v14", "v15"
          , "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31"
          , "cc", "memory");
    #endif
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
    atomic_fetch_sub_explicit(&c->loop->active, 1, memory_order_release);
    cone_wake(&c->done, (size_t)-1, 0);
    #if CONE_ASAN
        __sanitizer_start_switch_fiber(NULL, c->target_stack, c->target_stack_size);
    #endif
    #if CONE_ASM_X64
        __asm__("mov  %0, %%rsp  \n"
                "pop  %%rdi      \n"
                "pop  %%rbp      \n"
                "ret             \n" :: "r"(c->rsp));
    #elif CONE_ASM_ARM64
        __asm__("ldp   x0, x29, [%0]    \n"
                "ldp  x12, x30, [%0,16] \n"
                "add  sp, %0, 32        \n"
                "br   x12               \n" :: "r"(c->rsp));
    #endif
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
    size = (size + CONE_STACK_ALIGN - 1) & ~(size_t)(CONE_STACK_ALIGN - 1);
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
    c->rsp[0] = c;                  // first argument
    c->rsp[1] = NULL;               // frame pointer
    c->rsp[2] = (void*)&cone_body;  // program counter
    c->rsp[3] = NULL;               // return address (not actually used, but it terminates debugger stacks)
    atomic_fetch_add_explicit(&loop->active, 1, memory_order_release);
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

#ifndef CONE_SPIN_INTERVAL
#define CONE_SPIN_INTERVAL 512
#endif

static inline void arch_pause() {
    #if CONE_ASM_X64
        __asm__ __volatile__("pause");
    #elif CONE_ASM_ARM64
        __asm__ __volatile__("yield");
    #endif
}

static _Thread_local CONE_ATOMIC(uintptr_t) lki;

static void cone_tx_lock(struct cone_event *ev) {
    CONE_ATOMIC(uintptr_t) *p = atomic_exchange(&ev->lk, (void*)&lki);
    if (!p) return;
    atomic_fetch_or_explicit(&lki, 1, memory_order_relaxed);
    atomic_fetch_or_explicit(p, (uintptr_t)&lki, memory_order_release);
    for (size_t i = 0; atomic_load_explicit(&lki, memory_order_acquire) & 1;)
        if (++i % CONE_SPIN_INTERVAL) arch_pause(); else sched_yield();
}

static void cone_tx_unlock(struct cone_event *ev) {
    CONE_ATOMIC(uintptr_t) *n = (CONE_ATOMIC(uintptr_t) *)atomic_load_explicit(&lki, memory_order_acquire);
    if (!n) {
        if (atomic_compare_exchange_strong(&ev->lk, &(void *){(void*)&lki}, NULL))
            return;
        for (size_t i = 0; !(n = (CONE_ATOMIC(uintptr_t) *)atomic_load_explicit(&lki, memory_order_acquire));)
            if (++i % CONE_SPIN_INTERVAL) arch_pause(); else sched_yield();
    }
    atomic_fetch_and_explicit(n, ~(uintptr_t)1, memory_order_release);
    atomic_store_explicit(&lki, 0, memory_order_relaxed);
}

void cone_tx_begin(struct cone_event *ev) {
    cone_tx_lock(ev);
    // The increment must be an acquire operation so that it is not reordered with
    // the contents of the transaction.
    atomic_fetch_add_explicit(&ev->w, 1, memory_order_acq_rel);
}

void cone_tx_end(struct cone_event *ev) {
    cone_tx_unlock(ev);
    atomic_fetch_sub_explicit(&ev->w, 1, memory_order_release);
}

struct cone_event_it {
    struct cone_event_it *next, *prev;
    struct cone *c;
    intptr_t v;
};

intptr_t cone_tx_wait(struct cone_event *ev) {
    struct cone_event_it it = { NULL, ev->tail, cone, -1 };
    ev->tail ? (it.prev->next = &it) : (ev->head = &it);
    ev->tail = &it;
    cone_tx_unlock(ev);
    if (cone_deschedule(cone) MUN_RETHROW) {
        // This has to lock; even if we do an atomic read of it.v and observe a value,
        // `cone_wake` could still be between the write to it.v and the `cone_schedule`
        // call, so we can't release the memory yet. (This could be avoided if the write
        // was moved to after the wakeup call, but then if `cone_deschedule` succeeded
        // we'd need to spin until the value appears, so that'd improve error paths
        // at the cost of success paths, and that's probably a bad tradeoff.)
        cone_tx_lock(ev);
        if (it.v < 0) {
            atomic_fetch_sub_explicit(&ev->w, 1, memory_order_relaxed);
            it.prev ? (it.prev->next = it.next) : (ev->head = it.next);
            it.next ? (it.next->prev = it.prev) : (ev->tail = it.prev);
        }
        cone_tx_unlock(ev);
        return it.v < 0 ? -1 : ~it.v;
    }
    return it.v;
}

size_t cone_wake(struct cone_event *ev, size_t n, intptr_t ret) {
    size_t r = 0;
    if (!n || !atomic_load_explicit(&ev->w, memory_order_acquire))
        return 0; // serialized before any `cone_tx_begin`
    cone_tx_lock(ev);
    for (struct cone_event_it *it; n-- && (it = ev->head); r++) {
        atomic_fetch_sub_explicit(&ev->w, 1, memory_order_relaxed);
        ev->head = it->next;
        it->next ? (it->next->prev = it->prev) : (ev->tail = it->prev);
        it->v = ret & INTPTR_MAX;
        struct cone_loop *loop = cone_schedule(it->c, CONE_FLAG_WOKEN);
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
    return atomic_exchange_explicit(&m->lk, 1, memory_order_acquire) ? mun_error(EAGAIN, "mutex already locked") : 0;
}

int cone_lock(struct cone_mutex *m) {
    int r = 0;
    // 0 = xchg succeeded, 1 = fair handoff, 2 = retry xchg
    if (atomic_exchange_explicit(&m->lk, 1, memory_order_acquire))
        while ((r = cone_wait(&m->e, atomic_exchange_explicit(&m->lk, 1, memory_order_acquire))) == 2) {}
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
    struct cone_event_fd ev = {.fd = fd, .flags = write ? IO_W : IO_R, .c = cone};
    if (cone_event_io_add(&cone->loop->io, &ev) MUN_RETHROW)
        return -1;
    if (cone_deschedule(cone) MUN_RETHROW)
        return mun_cant_fail(cone_event_io_del(&cone->loop->io, &ev)), -1;
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
    // This can be relaxed because this flag is only used by the same thread.
    int prev = enable ? atomic_fetch_and_explicit(&cone->flags, ~CONE_FLAG_NO_INTR, memory_order_relaxed)
                      : atomic_fetch_or_explicit(&cone->flags, CONE_FLAG_NO_INTR, memory_order_relaxed);
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
