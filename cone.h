#pragma once
#ifndef CONE_DEFAULT_STACK
#define CONE_DEFAULT_STACK 65536
#endif

#include "mun.h"

#if __cplusplus
#include <atomic>
extern "C" {
#endif

#if __cplusplus
#define CONE_ATOMIC(T) std::atomic<T>
#else
#define CONE_ATOMIC(T) __typeof__(volatile _Atomic(T))
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
// May fail with ENOMEM.
struct cone *cone_spawn(size_t stack, struct cone_closure);

#define cone(f, arg) cone_spawn(CONE_DEFAULT_STACK, cone_bind(f, arg))

// Like `cone_spawn`, but also creates a new event loop and passes to the provided function
// a callback that runs it to completion. The loop terminates when all coroutines on it
// finish. (The function can, for example, create a new detached thread.)
struct cone *cone_loop(size_t stack, struct cone_closure, int (*run)(struct cone_closure));

// Like `cone_spawn`, but starts the coroutine on the same event loop as the other one
// (may be in a different thread).
//
// WARNING: the provided coroutine must not terminate until this call returns.
struct cone *cone_spawn_at(struct cone *, size_t stack, struct cone_closure);

// Drop the reference to a coroutine returned by `cone_spawn`. No-op if the pointer is NULL.
//
// WARNING: calling this twice on the one pointer is effectively a double-free. Avoid that.
void cone_drop(struct cone *);

// More readable names for the second argument of `cone_cowait`.
enum { CONE_RETHROW = 0, CONE_NORETHROW = 1 };

// Sleep until a coroutine finishes. If `norethrow` is 0 (`CONE_RETHROW`) and the coroutine
// fails, this function returns the error. If this is never done, the error is printed to
// stderr (see `mun_error_show`) when the coroutine is deallocated, and is otherwise ignored.
// May fail with `EDEADLK` if the provided coroutine is already waiting on the current one.
int cone_cowait(struct cone *, int norethrow);

// Same as `cone_cowait`, but also drop the reference.
static inline int cone_join(struct cone *c, int norethrow) {
    int r = cone_cowait(c, norethrow);
    return cone_drop(c), r;
}

// Sleep until a file descriptor is ready for reading/writing. If it already is, equivalent
// to `cone_yield`. This is only a best attempt; the action itself may still fail with
// EAGAIN, e.g. if another coroutine already used the file descriptor while this one was in
// the scheduler's run queue. If a call to this function is not inside a `while` loop,
// you're almost certainly doing it wrong.
int cone_iowait(int fd, int write);

// Sleep until at least the specified time, given by the monotonic clock (see
// `mun_usec_monotonic`). Unlike normal system calls, does not interact with signals.
// Clock jitter and scheduling delays apply.
int cone_sleep_until(mun_usec);

// Sleep for at least the specified time, or 0 if it is negative.
static inline int cone_sleep(mun_usec t) {
    return cone_sleep_until(mun_usec_monotonic() + t);
}

// Wait until the next iteration of the event loop. If any I/O events are pending, at least
// some of them will be processed. If any coroutines are pending, they will all resume
// before this coroutine.
static inline int cone_yield(void) {
    return cone_sleep_until(mun_usec_monotonic());
}

// A manually triggered event. Must be zero-initialized.
struct cone_event { void *head; void *tail; CONE_ATOMIC(void *) lk; CONE_ATOMIC(unsigned) w; };

// Begin an atomic transaction bound to an event. MUST be followed by one of the functions
// below without yielding or beginning a transaction on another event.
//
// WARNING: prefer using `cone_wait` instead. This is exposed only to permit `cone_wait` to
// accept arbitrary expressions. (Linux's futex implementation hides this, but the cost is
// that it can only do the equivalent of `cone_wait(e, *p == c)`.)
void cone_tx_begin(struct cone_event *);

// Finish an atomic transaction.
void cone_tx_end(struct cone_event *);

// Finish an atomic transaction, wait for `cone_wake` and return the value passed to it as
// an argument. If cancelled, `~value` is returned if cancellation arrived while this
// coroutine was resuming, or `~0` (-1) if it happened before any `cone_wake`s have
// occurred.
int cone_tx_wait(struct cone_event *);

// Atomically(*) execute an expression; if the result is 0, return 0, otherwise wait for
// `cone_wake` and return the value passed to it as an argument. If cancelled, `~value` is
// returned if cancellation arrived while this coroutine was resuming, or `~0` (-1) if it
// happened before any `cone_wake`s have occurred.
//
// (*) Atomicity here is ONLY with respect to `cone_wake`. Any reads/writes of shared
// variables in the expression should still use atomic instructions.
#define cone_wait(ev, x) (cone_tx_begin(ev), !(x) ? cone_tx_end(ev), 0 : cone_tx_wait(ev))

// Wake up at most N coroutines paused with `cone_tx_wait`, return the actual number.
//
// This function is atomic w.r.t. code between `cone_tx_begin` and `cone_tx_wait`:
// ```
// thread-1:
//     cone_tx_begin(&e); // begin
//     ... = memory;      // read
//     cone_tx_wait(&e);  // wait
// thread-2:
//     memory = ...;      // write
//     cone_wake(&e);     // wake
// ```
// If *read* observes the value from *write* or a later one, then *wake* happens-
// before *begin*; otherwise, *wait* happens-before *wake*.
size_t cone_wake(struct cone_event *, size_t, int /* non-negative */ ret);

// A coroutine-owned mutex. Must be zero-initialized.
struct cone_mutex { struct cone_event e; CONE_ATOMIC(char) lk; };

// Either lock and succeed, or fail with EAGAIN. This never fails with any other error,
// and cannot be cancelled.
int cone_try_lock(struct cone_mutex *);

// Lock, waiting until it's possible. Fail on cancellation or timeout.
int cone_lock(struct cone_mutex *);

// Allow a `cone_lock` to continue. If fair and there are waiters, it is guaranteed
// that the lock will be acquired by the earliest one. Returns whether any coroutine
// was waiting to acquire this lock.
int cone_unlock(struct cone_mutex *, int fair);

// Enable or disable cancellation and deadlines for this coroutine. If disabled, their effect
// is postponed until they are re-enabled. Returns the previous state.
//
// Disabling interrupts is required to safely use potentially-yielding calls in
// destructors. On the surface, doing that seems like a bad idea (do you *really* want
// to write to a database in a destructor?..), but there are cases where it's correct.
// For example, when coroutine A owns some object that is borrowed by coroutine B,
// A must never terminate before B no matter how many times someone requests its
// cancellation, so A absolutely has to execute a sequence like
//
//     int restore = cone_intr(0);
//     cone_join(B, CONE_NORETHROW); // never fails
//     cone_intr(restore);
//
// before that object can be deallocated.
int cone_intr(int enable);

// Make the next (or current, if any) call to a blocking function from the specified
// coroutine fail with ECANCELED.
//
// Blocking functions: `cone_tx_wait`, `cone_iowait`, `cone_sleep_until`, `cone_sleep`,
// `cone_yield`.
//
// No-op if the coroutine has already finished, or will never call a blocking function
// before finishing. To signal coroutines to stop at some point other than a blocking
// call, use other synchronization primitives, e.g. atomics.
void cone_cancel(struct cone *);

// After the monotonic clock reaches the specified point, make the next (or ongoing, if
// any) call to a blocking function from the specified coroutine fail with ETIMEDOUT.
// If one call would fail with both ECANCELED and ETIMEDOUT, the former takes priority.
//
// WARNING: the provided coroutine has to be running on the same thread.
int cone_deadline(struct cone *, mun_usec);

// Undo *one* previous call to `cone_deadline` with the *same* arguments.
void cone_complete(struct cone *, mun_usec);

// The live counter of coroutines active in the running coroutine's event loop.
const CONE_ATOMIC(unsigned) *cone_count(void);

// The current scheduling delay, i.e. the time it takes to clear the run queue.
const CONE_ATOMIC(mun_usec) *cone_delay(void);

#if __cplusplus
} // extern "C"
#endif
