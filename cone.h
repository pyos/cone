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
typedef std::atomic<unsigned> cone_atom;
#else
typedef volatile _Atomic(unsigned) cone_atom;
#endif

struct cone_closure
{
    int (*code)(void*);
    void *data;
};

#define cone_bind(f, data) ((struct cone_closure){(int(*)(void*))f, data})

// The coroutine in which the code is currently executing.
extern
#if __cplusplus && !__clang__
    thread_local
#else
    _Thread_local
#endif
    struct cone * cone;

// A manually triggered event. Zero-initialized; finalized with `mun_vec_fini`;
// must not be destroyed if there are callbacks attached.
struct cone_event mun_vec(struct cone *);

// Switch a file descriptor into non-blocking mode. See fcntl(2) for error codes.
int cone_unblock(int fd);

// Create a new coroutine that runs a given function with a single pointer argument.
// The memory will be freed when the coroutine finishes and the returned reference is
// dropped, no matter the order.
struct cone *cone_spawn(size_t stack, struct cone_closure) mun_throws(memory);

#define cone(f, arg) cone_spawn(CONE_DEFAULT_STACK, cone_bind(f, arg))

// Drop the reference to a coroutine returned by `cone_spawn`.
int cone_drop(struct cone *);

enum CONE_COWAIT_FLAGS
{
    CONE_NORETHROW = 1,
};

// Sleep until a coroutine finishes. If it happens to throw an error in the process,
// rethrow it into the current coroutine instead of printing, unless CONE_NORETHROW is in flags.
int cone_cowait(struct cone *, int flags);

// Same as `cone_cowait`, but also drop the reference.
static inline int cone_join(struct cone *c, int flags) {
    int r = cone_cowait(c, flags);
    cone_drop(c);
    return r;
}

// Sleep until a file descriptor is ready for reading/writing. If it already is,
// equivalent to `cone_yield`.
int cone_iowait(int fd, int write) mun_throws(cancelled, memory);

// Sleep until at least the specified time, given by the monotonic clock (see
// `mun_usec_monotonic`). Unlike normal system calls, does not interact with signals.
int cone_sleep_until(mun_usec) mun_throws(cancelled, memory);

// Sleep for at least the specified time, or 0 if it is negative.
static inline int cone_sleep(mun_usec t) {
    return cone_sleep_until(mun_usec_monotonic() + t);
}

// Wait until the next iteration of the event loop. Unlike `cone_sleep`, if this
// call isn't cancelled, pending I/O events are guaranteed to be consumed before
// it returns. (`cone_sleep` may or may not poll for I/O).
int cone_yield(void) mun_throws(cancelled, memory);

// If the value at the address is the same as the one passed as an argument, sleep until
// `cone_wake` is called with the same event.
int cone_wait(struct cone_event *, const cone_atom *, unsigned) mun_throws(cancelled, memory, retry);

// Wake up at most N coroutines paused with `cone_wait`.
int cone_wake(struct cone_event *, size_t) mun_throws(memory);

// Make the next (or current, if any) call to `cone_wait`, `cone_iowait`, `cone_sleep_until`,
// `cone_sleep`, or `cone_yield` from the specified coroutine fail with ECANCELED.
// No-op if the coroutine has already finished.
int cone_cancel(struct cone *) mun_throws(memory);

// See `cone_cancel`, but replace "ECANCELED" with "ETIMEDOUT" and "next"
// with "next after the specified point in time (according to the monotonic clock)".
// If one call would fail with both ECANCELED and ETIMEDOUT, the former takes priority.
int cone_deadline(struct cone *, mun_usec) mun_throws(memory);

// Undo *one* previous call to `cone_deadline` with the same arguments.
void cone_complete(struct cone *, mun_usec);

// Create a coroutine on a new event loop, then block until all coroutines on it complete.
// Note that `main()` is already running within an event loop; this is only useful
// in newly created threads.
int cone_loop(size_t stack, struct cone_closure) mun_throws(memory);

// Return the number of coroutines active in the running coroutine's loop.
const cone_atom * cone_count(void);

#if __cplusplus
} // extern "C"
#endif
