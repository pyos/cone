#pragma once
#include "mun.h"

typedef volatile _Atomic unsigned cone_atom;

struct cone_closure
{
    int (*code)(void*);
    void *data;
};

// A manually triggered event. Zero-initialized; finalized with `mun_vec_fini`;
// must not be destroyed if there are callbacks attached.
struct cone_event mun_vec(struct cone *);

// The coroutine in which the code is currently executing.
extern _Thread_local struct cone * volatile cone;

// Switch a file descriptor into non-blocking mode. See fcntl(2) for error codes.
int cone_unblock(int fd);

// Create a new coroutine that runs a given function with a single pointer argument.
// When stack size is 0, an unspecified default value is used. The new coroutine has
// a reference count of 2; one of the references is owned by the loop and released
// when the coroutine terminates.
struct cone *cone_spawn(size_t stack, struct cone_closure) mun_throws(memory);

// Increment the reference count. The coroutine will not be destroyed until `cone_decref`
// is called matching number of times.
void cone_incref(struct cone *);

// Decrement the reference count. If it becomes zero, destroy the coroutine; also,
// if the coroutine has failed with an error, it's printed to stderr (see `mun_error_show`).
// If argument is NULL, last error is rethrown, allowing `cone_decref(cone(...))` without
// additional checks. Does not fail otherwise.
int cone_decref(struct cone *);

// Sleep until a coroutine finishes. If it happens to throw an error in the process,
// rethrow it into the current coroutine instead of printing. Consumes a single reference.
int cone_join(struct cone *);

// Sleep until a file descriptor is ready for reading/writing. Behavior is undefined
// if it already is (call read/write until it returns EAGAIN/EWOULDBLOCK first).
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

#define cone_bind(f, data) ((struct cone_closure){(int(*)(void*))f, data})
#define cone(f, arg) cone_spawn(0, cone_bind(f, arg))
