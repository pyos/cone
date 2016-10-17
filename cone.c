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
#define SA_ONSTACK 0x8000000  // avoid requiring _GNU_SOURCE
#endif
#endif

// Call a function when the next time the event is triggered, or, if already in
// `cone_event_emit`, as soon as the existing callbacks are fired.
//
// Errors: `memory`.
//
static int cone_event_add(struct cone_event *ev, struct cone_closure f) {
    return mun_vec_append(ev, &f);
}

// Cancel a previous `cone_event_add(ev, f)`. No-op if there was none/it has already been fired.
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

// A priority queue of functions to be called at precise points in time.
// Zero-initialized; finalized with `mun_vec_fini`; must not be destroyed
// if there are callbacks attached, else program state is indeterminate.
struct cone_event_schedule mun_vec(struct cone_event_at);
struct cone_event_at { mun_usec at; struct cone_closure f; };

// Schedule a callback to be fired at some time defined by a monotonic system clock.
//
// Errors: `memory`.
//
static int cone_event_schedule_add(struct cone_event_schedule *ev, mun_usec at, struct cone_closure f) {
    return mun_vec_insert(ev, mun_vec_bisect(ev, at < _->at), &((struct cone_event_at){at, f}));
}

// Remove a callback previously scheduled with `cone_event_schedule_add(ev, at, f)`.
// No-op if there was none/it has already been fired.
static void cone_event_schedule_del(struct cone_event_schedule *ev, mun_usec at, struct cone_closure f) {
    for (size_t i = mun_vec_bisect(ev, at < _->at); i-- && ev->data[i].at == at; )
        if (ev->data[i].f.code == f.code && ev->data[i].f.data == f.data)
            return mun_vec_erase(ev, i, 1);
}

// Call all due callbacks and pop them off the queue. If a callback fails, it is
// not removed, and the rest are not fired either.
//
// Errors: does not fail by itself, but rethrows anything from functions in the queue.
//
static mun_usec cone_event_schedule_emit(struct cone_event_schedule *ev) {
    for (; ev->size; mun_vec_erase(ev, 0, 1)) {
        mun_usec now = mun_usec_monotonic();
        if (ev->data[0].at > now)
            return ev->data[0].at - now;
        if (ev->data[0].f.code(ev->data[0].f.data) MUN_RETHROW)
            return -1;
    }
    return INT32_MAX;
}

// A set of callbacks attached to a single file descriptor: one for reading, one
// for writing. Both are called on errors. Zero-initialized, except for `fd`; must not
// be destroyed if there are callbacks attached, else program state is indeterminate.
struct cone_event_fd
{
    int fd;
    struct cone_closure cbs[2];
    struct cone_event_fd *link;
};

// A map of file descriptors to corresponding callbacks. Zero-initialized, followed by
// `cone_event_io_init`; finalized by `cone_event_io_fini`; must not be destroyed if
// there are callbacks attached, else program state is indeterminate.
struct cone_event_io
{
    int epoll;
    struct cone_event_fd *fds[127];
};

// Initializer of `struct cone_event_io`.
//
// Errors: see epoll_create1(2) if CONE_EPOLL is 1.
//
static int cone_event_io_init(struct cone_event_io *set) {
    set->epoll = -1;
#if CONE_EPOLL
    if ((set->epoll = epoll_create1(0)) < 0 MUN_RETHROW) return -1;
#endif
    return 0;
}

// Finalizer of `struct cone_event_io`. Object state is undefined afterwards.
static void cone_event_io_fini(struct cone_event_io *set) {
    for (unsigned i = 0; i < sizeof(set->fds) / sizeof(*set->fds); i++)
        for (struct cone_event_fd *c; (c = set->fds[i]) != NULL; free(c))
            set->fds[i] = c->link;
    if (set->epoll >= 0)
        close(set->epoll);
}

// Find the pointer that links to the structure describing this fd. Or should,
// if it does not exist yet. Never fails.
static struct cone_event_fd **cone_event_io_bucket(struct cone_event_io *set, int fd) {
    struct cone_event_fd **b = &set->fds[fd % (sizeof(set->fds) / sizeof(set->fds[0]))];
    while (*b && (*b)->fd != fd) b = &(*b)->link;
    return b;
}

// Request a function to be called when a file descriptor becomes available for reading
// or writing. If it already is, behavior is undefined. No more than one callback can
// correspond to an (fd, write) pair.
//
// Errors:
//   * `memory`;
//   * `assert`: there is already a callback attached to this event.
//   * see epoll_ctl(2) if CONE_EPOLL is 1.
//
static int cone_event_io_add(struct cone_event_io *set, int fd, int write, struct cone_closure f) {
    struct cone_event_fd **b = cone_event_io_bucket(set, fd);
    if (*b == NULL) {
        if ((*b = malloc(sizeof(struct cone_event_fd))) == NULL)
            return mun_error(memory, "-");
        **b = (struct cone_event_fd){.fd = fd};
    #if CONE_EPOLL
        struct epoll_event params = {EPOLLRDHUP|EPOLLHUP|EPOLLET|EPOLLIN|EPOLLOUT, {.ptr = *b}};
        if (epoll_ctl(set->epoll, EPOLL_CTL_ADD, fd, &params) MUN_RETHROW_OS)
            return free(*b), *b = NULL, -1;
    #endif
    } else if ((*b)->cbs[write].code)
        return mun_error(assert, "two readers/writers on one file descriptor");
    (*b)->cbs[write] = f;
    return 0;
}

// Remove a previously attached callback. No-op if there is none or this is not the correct one.
static void cone_event_io_del(struct cone_event_io *set, int fd, int write, struct cone_closure f) {
    struct cone_event_fd **b = cone_event_io_bucket(set, fd), *e = *b;
    if (e == NULL || e->cbs[write].code != f.code || e->cbs[write].data != f.data)
        return;
    e->cbs[write] = (struct cone_closure){};
    if (e->cbs[!write].code == NULL) {
    #if CONE_EPOLL
        epoll_ctl(set->epoll, EPOLL_CTL_DEL, fd, NULL);
    #endif
        *b = e->link;
        free(e);
    }
}

// Fire all pending I/O events; if there are none, wait for an event for at most `timeout`
// microseconds, or until interrupted by a signal or a ping. If `timeout` is -1, rethrow
// the previous error under the assumption that a `cone_event_schedule_emit` call has failed.
//
// Errors:
//   * rethrows the last error if timeout is -1;
//   * rethrows anything the callbacks might throw;
//   * see either epoll_wait(2) or select(2) depending on CONE_EPOLL.
//
static int cone_event_io_emit(struct cone_event_io *set, mun_usec timeout) {
    if (timeout < 0 MUN_RETHROW)
        return -1;
    if (timeout > 60000000ll)
        timeout = 60000000ll;
#if CONE_EPOLL
    struct epoll_event evs[32];
    int got = epoll_wait(set->epoll, evs, 32, timeout / 1000ul);
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
#else
    fd_set fds[2] = {};
    int max_fd = 0;
    for (unsigned i = 0; i < sizeof(set->fds) / sizeof(*set->fds); i++) {
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
    for (unsigned i = 0; i < sizeof(set->fds) / sizeof(*set->fds); i++)
        for (struct cone_event_fd *e = set->fds[i]; e; e = e->link)
            for (int i = 0; i < 2; i++)
                if (FD_ISSET(e->fd, &fds[i]) && e->cbs[i].code && e->cbs[i].code(e->cbs[i].data) MUN_RETHROW)
                    return -1;
#endif
    return 0;
}

// An amalgamation of `cone_event_io`, `cone_event_schedule`, and a pipe that forces
// `cone_event_io_emit` to stop instantly. Zero-initialized, followed by `cone_loop_init`;
// finalized by `cone_loop_fini`; must not be destroyed until there are no callbacks
// or coroutines, else program state is indeterminate.
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

// Read a byte from the ping pipe, then schedule a ping event to be fired on next iteration.
//
// Errors: `memory`.
//
static int cone_loop_consume_ping(struct cone_loop *loop) {
    ssize_t rd = read(loop->selfpipe[0], &rd, sizeof(rd));  // never yields
    atomic_store_explicit(&loop->pinged, 0, memory_order_release);
    return cone_event_schedule_add(&loop->at, mun_usec_monotonic(), cone_bind(&cone_event_emit, &loop->ping)) MUN_RETHROW;
}

// Finalizer of `struct cone_loop`. Object state is undefined afterwards.
static void cone_loop_fini(struct cone_loop *loop) {
    close(loop->selfpipe[0]);
    close(loop->selfpipe[1]);
    cone_event_io_fini(&loop->io);
    mun_vec_fini(&loop->ping);
    mun_vec_fini(&loop->at);
}

// Initializer of `struct cone_loop`.
//
// Errors:
//   * `memory`;
//   * see pipe(2), cone_unblock, and cone_event_io_init.
//
static int cone_loop_init(struct cone_loop *loop) {
    atomic_init(&loop->active, 0);
    atomic_init(&loop->pinged, 0);
    if (pipe(loop->selfpipe) MUN_RETHROW_OS)
        return -1;
    if (cone_event_io_init(&loop->io)
     || cone_event_io_add(&loop->io, loop->selfpipe[0], 0, cone_bind(&cone_loop_consume_ping, loop))
     || cone_unblock(loop->selfpipe[0]) || cone_unblock(loop->selfpipe[1]) MUN_RETHROW)
        return cone_loop_fini(loop), -1;
    return 0;
}

// Until the reference count reaches zero, fire off callbacks while waiting for I/O
// events in between. If an error is returned (due to a failed callback or otherwise),
// program state is indeterminate due to eternally frozen coroutines.
//
// Errors: see cone_event_schedule_emit and cone_event_io_emit.
//
static int cone_loop_run(struct cone_loop *loop) {
    while (atomic_load_explicit(&loop->active, memory_order_acquire))
        if (cone_event_io_emit(&loop->io, cone_event_schedule_emit(&loop->at)) MUN_RETHROW)
            return -1;
    return 0;
}

// Send a ping through the pipe, waking up the loop if it's currently waiting for I/O
// events and triggering a ping event. Surprisingly, this function is thread-safe.
static void cone_loop_ping(struct cone_loop *loop) {
    _Bool expect = 0;
    if (atomic_compare_exchange_strong(&loop->pinged, &expect, 1))
        write(loop->selfpipe[1], "", 1);
}

// Increment the reference count of the loop. `cone_loop_run` will only stop when
// a matching number of calls to `cone_loop_dec` is done.
static void cone_loop_inc(struct cone_loop *loop) {
    atomic_fetch_add_explicit(&loop->active, 1, memory_order_release);
}

// Decrement the reference count. If it reaches 0, send a ping to notify and stop the loop.
static void cone_loop_dec(struct cone_loop *loop) {
    if (atomic_fetch_sub_explicit(&loop->active, 1, memory_order_release) == 1)
        cone_loop_ping(loop);
}

enum
{
    CONE_FLAG_SCHEDULED = 0x01,  // `cone_schedule` has been called, but `cone_run` has not (yet).
    CONE_FLAG_RUNNING   = 0x02,  // Currently on this coroutine's stack.
    CONE_FLAG_FINISHED  = 0x04,  // This coroutine has reached its end. The next `cone_switch` will abort.
    CONE_FLAG_FAILED    = 0x08,  // This coroutine has reached its end, and the `error` field contains a present.
    CONE_FLAG_CANCELLED = 0x10,  // This coroutine has reached an untimely end due to `cone_cancel`.
    CONE_FLAG_RETHROWN  = 0x20,  // This coroutine's `error` field has been examined by `cone_join`.
};

// A nice, symmetric shape. Never initialized/finalized directly; allocated by `cone_spawn`;
// deallocated by `cone_decref`; must not be destroyed until `CONE_FLAG_FINISHED` is set,
// else program state is indeterminate, and 100% incorrect if `CONE_FLAG_RUNNING` was on.
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

// The innermost coroutine that has `CONE_RUNNING` set.
_Thread_local struct cone * volatile cone = NULL;

// Swap the stack, in either direction.
static void cone_switch(struct cone *c) {
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
}

// Switch to this coroutine's stack. Behavior is undefined if already on it. Never fails;
// the `int` return is for type compatibility with `cone_closure`.
static int cone_run(struct cone *c) {
    c->flags &= ~CONE_FLAG_SCHEDULED;
    struct cone* prev = cone;
    cone_switch(cone = c);
    cone = prev;
    return 0;
}

// Call `cone_run` on the next iteration of the event loop.
//
// Errors: `memory`.
//
static int cone_schedule(struct cone *c) {
    if (!(c->flags & CONE_FLAG_SCHEDULED))
        if (cone_event_schedule_add(&c->loop->at, mun_usec_monotonic(), cone_bind(&cone_run, c)) MUN_RETHROW)
            return -1;
    c->flags |= CONE_FLAG_SCHEDULED;
    return 0;
}

// Sleep until an event is fired. If the coroutine is cancelled during that time,
// unsubscribe and throw a `cancelled` error.
#define cone_pause(always_unsub, ev_add, ev_del, ...) do {                \
    if (ev_add(__VA_ARGS__, cone_bind(&cone_schedule, cone)) MUN_RETHROW) \
        return -1;                                                        \
    cone_switch(cone);                                                    \
    if (always_unsub || cone->flags & CONE_FLAG_CANCELLED)                \
        ev_del(__VA_ARGS__, cone_bind(&cone_schedule, cone));             \
    if (!(cone->flags & CONE_FLAG_CANCELLED))                             \
        return 0;                                                         \
    cone->flags &= ~CONE_FLAG_CANCELLED;                                  \
    return cone_cancel(cone);                                             \
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
    cone_loop_ping(cone->loop);
    return cone_wait(&cone->loop->ping) MUN_RETHROW;
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

// Main function run on the coroutine's stack. Must not return because the return
// address is undefined. Errors encountered while executing the body are saved in the
// object; `memory` errors during scheduling of the "done" callbacks abort the program.
// Trying to go past the end of a coroutine is also an instant SIGABRT.
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
// Argument to `cone_sigstackswitch` because I can't be bothered to use `sigqueue`.
static _Thread_local struct cone *volatile cone_sigctx;

// Construct a jump context that runs `cone_body` on an alternate stack. Intended
// to be used with `sigaltstack`, `sigaction`, and `SA_ONSTACK`.
static void cone_sigstackswitch() {
    struct cone *c = cone_sigctx;
    if (setjmp(c->ctx[0]))
        cone_body(c);
}
#endif

// Create a coroutine on a different event loop (not the one we're currently on).
// See documentation for `cone_spawn` in header.
static struct cone *cone_spawn_on(struct cone_loop *loop, size_t size, struct cone_closure body) {
    size &= ~(size_t)15;
    if (size < sizeof(struct cone) + MINSIGSTKSZ)
        size = CONE_DEFAULT_STACK;
    struct cone *c = (struct cone *)malloc(size);
    if (c == NULL)
        return mun_error(memory, "no space for a stack"), NULL;
    c->refcount = 1;
    c->flags = 0;
    c->loop = loop;
    c->body = body;
    c->done = (struct cone_event){};
#if CONE_XCHG_RSP
    c->rsp = (void **)(c->stack + size - sizeof(struct cone)) - 4;
    c->rsp[0] = c;                  // %rdi: first argument
    c->rsp[1] = NULL;               // %rbp: nothing; there's no previous frame yet
    c->rsp[2] = (void*)&cone_body;  // %rip: code to execute;
    c->rsp[3] = NULL;               // return address: nothing; same as for %rbp
#else
    stack_t old_stk, new_stk = {.ss_sp = c->stack, .ss_size = size - sizeof(struct cone)};
    struct sigaction old_act, new_act;
    new_act.sa_handler = &cone_sigstackswitch;
    new_act.sa_flags = SA_ONSTACK;
    sigemptyset(&new_act.sa_mask);
    sigaltstack(&new_stk, &old_stk);
    sigaction(SIGUSR1, &new_act, &old_act);
    cone_sigctx = c;
    raise(SIGUSR1);  // FIXME should block SIGUSR1 until this line
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
        return cone_decref(c), cone_loop_fini(&loop), -1;
    return cone_loop_fini(&loop), cone_join(c) MUN_RETHROW;
}

static struct cone_loop cone_main_loop = {};

static int cone_main_run(struct cone_loop *loop) {
    if (cone_loop_run(loop))
        mun_error_show("main loop", NULL), exit(124);
    return 0;
}

static void __attribute__((constructor)) cone_main_init(void) {
    if (cone_loop_init(&cone_main_loop) MUN_RETHROW)
        mun_error_show("cone init", NULL), exit(124);
    struct cone *c = cone_spawn_on(&cone_main_loop, 0, cone_bind(&cone_main_run, &cone_main_loop));
    if (c == NULL MUN_RETHROW)
        mun_error_show("cone init", NULL), exit(124);
    cone_switch(c);
}

static void __attribute__((destructor)) cone_main_fini(void) {
    struct cone *c = cone;
    if (c) {
        struct cone_loop *loop = c->loop;
        cone_loop_dec(loop);
        cone_switch(c);
        cone_decref(c);
        if (cone_event_schedule_emit(&loop->at) < 0)
            mun_error_show("cone fini", NULL), exit(124);
        cone_loop_fini(loop);
    }
}
