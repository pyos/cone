#pragma once
#ifndef CONE_DEFAULT_STACK
#define CONE_DEFAULT_STACK 65536
#endif

#include "mun.h"

typedef volatile _Atomic(unsigned) cone_atom;

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
    struct cone * volatile cone;

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
    CONE_NORETHROW,
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

// Sleep just because. Unlike normal system calls, does not interact with signals.
int cone_sleep(mun_usec delay) mun_throws(cancelled, memory);

// Wait until the next iteration of the event loop.
int cone_yield(void) mun_throws(cancelled, memory);

// If the value at the address is the same as the one passed as an argument, sleep until
// `cone_wake` is called with the same event. Otherwise, return 1. This compare-and-sleep
// operation is atomic, but only within a single event loop; if multiple coroutines
// from different loops touch the same `cone_atom`, behavior is undefined.
int cone_wait(struct cone_event *, cone_atom *, unsigned) mun_throws(cancelled, memory);

// Wake up at most N coroutines paused with `cone_wait`.
int cone_wake(struct cone_event *, size_t) mun_throws(memory);

// Arrange for the coroutine to be woken up with an error, even if the event it was waiting
// for did not yet occur. No-op if the coroutine has already finished. If it is currently
// running, it will only receive a cancellation signal upon reaching `cone_wait`,
// `cone_iowait`, `cone_sleep`, or `cone_yield`; and even then, the error may be ignored.
int cone_cancel(struct cone *) mun_throws(memory);
