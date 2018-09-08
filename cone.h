#pragma once
#ifndef CONE_DEFAULT_STACK
#define CONE_DEFAULT_STACK 65536
#endif

#include "mun.h"

#if __cplusplus
#include <atomic>
extern "C" {
#endif

struct cone_closure {
    int (*code)(void*);
    void *data;
};

#define cone_bind(f, data) ((struct cone_closure){(int(*)(void*))f, data})

// The coroutine in which the code is currently executing.
#if __cplusplus && !__clang__
extern thread_local struct cone *cone;
#else
extern _Thread_local struct cone *cone;
#endif

// Create a new coroutine that runs a given function with a single pointer argument.
// The memory will be freed when the coroutine finishes and the returned reference is
// dropped, no matter the order. For what happens when the closure fails, see `cone_cowait`.
struct cone *cone_spawn(size_t stack, struct cone_closure) mun_throws(memory);

#define cone(f, arg) cone_spawn(CONE_DEFAULT_STACK, cone_bind(f, arg))

// Like `cone_spawn`, but also creates a new event loop and passes to the provided function
// a callback that runs it to completion. The loop terminates when all coroutines on it
// finish. (The function can, for example, create a new detached thread.)
struct cone *cone_loop(size_t stack, struct cone_closure, int (*run)(struct cone_closure)) mun_throws(memory);

// Drop the reference to a coroutine returned by `cone_spawn`. No-op if the pointer is NULL.
void cone_drop(struct cone *);

// Sleep until a coroutine finishes. If `norethrow` is 0 and the coroutine fails, this
// function returns the error. If this is never done, the error is printed to stderr (see
// `mun_error_show`) when the coroutine is deallocated, and is otherwise ignored.
int cone_cowait(struct cone *, int norethrow) mun_throws(cancelled, timeout, deadlock);

// Same as `cone_cowait`, but also drop the reference.
static inline int cone_join(struct cone *c, int norethrow) mun_throws(cancelled, timeout, deadlock) {
    int r = cone_cowait(c, norethrow);
    return cone_drop(c), r;
}

// Sleep until a file descriptor is ready for reading/writing. If it already is,
// equivalent to `cone_yield`. This is only a best attempt; the action itself may
// still fail with EAGAIN, e.g. if another coroutine already used the file descriptor
// while this one was in the scheduler's run queue. If a call to this function is not
// inside a `while` loop, you're almost certainly doing it wrong.
int cone_iowait(int fd, int write) mun_throws(cancelled, timeout);

// Sleep until at least the specified time, given by the monotonic clock (see
// `mun_usec_monotonic`). Unlike normal system calls, does not interact with signals.
// Clock jitter and scheduling delays apply.
int cone_sleep_until(mun_usec) mun_throws(cancelled, timeout, memory);

// Sleep for at least the specified time, or 0 if it is negative.
static inline int cone_sleep(mun_usec t) mun_throws(cancelled, timeout, memory) {
    return cone_sleep_until(mun_usec_monotonic() + t);
}

// Wait until the next iteration of the event loop. Unlike `cone_sleep(0)`, if this
// call isn't cancelled, pending I/O events are guaranteed to be consumed before
// it returns. (`cone_sleep` may or may not poll for I/O).
int cone_yield(void) mun_throws(cancelled, timeout);

// A manually triggered event. Zero-initialized; no finalizer; trivially movable.
struct cone_event { void *head, *tail, *lk; };

// Prepare to wait for an event. MUST be followed by `cone_evfinish` without yielding
// or calling `cone_evprepare` on another event!
void cone_evprepare(struct cone_event *ev);

// If `success` is the same as `sleep_if`, wait for `cone_wake`. Otherwise, undo `cone_evprepare`.
// If `success` is 0, instead of returning normally, fail with EAGAIN. Everything between `cone_evprepare`
// and `cone_evfinish` is atomic and totally ordered w.r.t. other pairs of calls and `cone_wake`.
int cone_evfinish(struct cone_event *ev, int success, int sleep_if) mun_throws(cancelled, timeout, retry);

// If the second argument evaluates to true, wait for `cone_wake`. Otherwise, fail with EAGAIN.
#define cone_wait_if(ev, x) (cone_evprepare(ev), cone_evfinish(ev, !!(x), 1))

// If the second argument evaluates to false, wait for `cone_wake` and then fail with EAGAIN.
#define cone_wait_if_not(ev, x) (cone_evprepare(ev), cone_evfinish(ev, x, 0))

// Compatibility alias: sleep on `*x == y`, else fail with EAGAIN.
#define cone_wait(ev, x, y) cone_wait_if(ev, *(x) == y)

// Wake up at most N coroutines paused with `cone_wait`.
void cone_wake(struct cone_event *, size_t);

// Enable or disable cancellation and deadlines for this coroutine. If disabled, their effect
// is postponed until they are re-enabled. Returns the previous state.
int cone_intr(int enable);

// Make the next (or current, if any) call to `cone_wait`, `cone_iowait`, `cone_sleep_until`,
// `cone_sleep`, or `cone_yield` from the specified coroutine fail with ECANCELED.
// No-op if the coroutine has already finished.
void cone_cancel(struct cone *);

// See `cone_cancel`, but replace "ECANCELED" with "ETIMEDOUT" and "next"
// with "next after the specified point in time (according to the monotonic clock)".
// If one call would fail with both ECANCELED and ETIMEDOUT, the former takes priority.
// XXX exactly how useful it is to set deadlines on other coroutines?
int cone_deadline(struct cone *, mun_usec) mun_throws(memory);

// Undo *one* previous call to `cone_deadline` with the same arguments.
void cone_complete(struct cone *, mun_usec);

// Return the live counter of coroutines active in the running coroutine's event loop.
#if __cplusplus
const std::atomic<unsigned> *cone_count(void);
#else
const volatile _Atomic(unsigned) *cone_count(void);
#endif

#if __cplusplus
} // extern "C"
#endif
