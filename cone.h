#pragma once
//
// cone // stack switching through POSIX signal abuse
//
#if !defined(CONE_EPOLL) && __linux__
// Use {0: select, 1: epoll} to wait for I/O.
#define CONE_EPOLL 1
#endif

#if !defined(CONE_XCHG_RSP) && (__linux__ || __APPLE__) && __x86_64__
// Use {0: sigaltstack+sigaction+setjmp+longjmp, 1: x86-64 SysV ABI assembly} to switch stacks.
#define CONE_XCHG_RSP 1
#endif

#ifndef CONE_DEFAULT_STACK
// How much space to allocate for a stack if 0 was passed to `cone_root`/`cone_spawn`.
// Must be at least MINSIGSTKSZ plus 256 bytes, else behavior is undefined.
#define CONE_DEFAULT_STACK 65536
#endif

#include "mun.h"

typedef volatile _Atomic unsigned cone_atom;

// A single function bound to some data. Naturally, does not own the pointer; manage
// it on your own. Aggregate-initialized; also see `cone_bind`.
struct cone_closure
{
    int (*code)(void*);
    void *data;
};

// Construct a `cone_closure` while casting the function to the correct type.
#define cone_bind(f, data) ((struct cone_closure){(int(*)(void*))f, data})

// A manually triggered event. Zero-initialized; finalized with `mun_vec_fini`; must not
// be destroyed if there are callbacks attached, else program state is indeterminate.
struct cone_event mun_vec(struct cone_closure);

// The coroutine in which the code is currently executing.
extern _Thread_local struct cone * volatile cone;

// Switch a file descriptor into non-blocking mode.
//
// Errors: see fcntl(2), particularly the F_GETFL and F_SETFL modes.
//
int cone_unblock(int fd);

// Create a coroutine on a new event loop, then block until all coroutines on it complete.
//
// Errors:
//   * `memory`;
//   * anything thrown by the main coroutine;
//   * see pipe(2), fcntl(2);
//   * see epoll_create(2) and epoll_wait(2) if CONE_EPOLL is 1, select(2) otherwise.
//
int cone_root(size_t stack, struct cone_closure);

// Create a new coroutine that runs a given function with a single pointer argument.
// `size` is the size of the stack, including space for coroutine metadata; if there is
// not enough, default stack size is used. The returned coroutine has a reference count
// of 2: one reference for the calling function, and one for the event loop, dropped
// automatically when the coroutine terminates.
//
// Errors: `memory`.
//
struct cone *cone_spawn(size_t stack, struct cone_closure);

// Create a new coroutine with the default stack size.
#define cone(f, arg) cone_spawn(0, cone_bind(f, arg))

// Arrange for the coroutine to be woken up with an error, even if the event it was waiting
// for did not yet occur. No-op if the coroutine has already finished.
//
// Errors:
//   * `cancelled` if the argument is the currently running coroutine;
//   * `memory`.
//
int cone_cancel(struct cone *);

// Increment the reference count. The coroutine will not be destroyed until `cone_decref`
// is called matching number of times.
void cone_incref(struct cone *);

// Decrement the reference count. If it becomes zero, destroy the coroutine; also,
// if the coroutine has failed with an error, it's printed to stderr (see `mun_error_show`).
//
// Errors: if argument is NULL, rethrows the last error (in case it was from `cone_spawn`).
//
int cone_decref(struct cone *);

// Sleep until a coroutine finishes. If it happens to throw an error in the process,
// rethrow it into the current coroutine instead of printing. Consumes a single reference.
//
// Errors: see `cone_wait`; also, anything thrown by the awaited coroutine.
//
int cone_join(struct cone *);

// If the value at the address is the same as the one passed as an argument, sleep until
// `cone_wake` is called with the same event. Otherwise, return 1. This compare-and-sleep
// operation is atomic, but only within a single event loop; if multiple coroutines
// from different loops touch the same `cone_atom`, behavior is undefined.
//
// Errors:
//   * `cancelled`: `cone_cancel` was called while this coroutine was sleeping.
//   * `memory`.
//
int cone_wait(struct cone_event *, cone_atom *, unsigned);

// Wake up at most N coroutines paused with `cone_wait`.
//
// Errors: `memory`.
//
int cone_wake(struct cone_event *, size_t);

// Sleep until a file descriptor is ready for reading/writing. Behavior is undefined
// if it already is (call read/write until it returns EAGAIN/EWOULDBLOCK first).
//
// Errors: same as `cone_wait`, plus `assert` if another coroutine is waiting for the same event.
//
int cone_iowait(int fd, int write);

// Sleep just because. Unlike normal system calls, does not interact with signals.
//
// Errors: same as `cone_wait`.
//
int cone_sleep(mun_usec delay);

// Wait until the next iteration of the event loop.
//
// Errors: same as `cone_wait`.
//
int cone_yield(void);
