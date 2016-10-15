#pragma once
//
// cone // coroutines
//
// Signal-based stack jumping: public API.
//
// NOTE: if a program is linked with cone.o, it must define `comain` instead of `main`.
//       (`comain` is automatically run in a coroutine. This could be done with `main`,
//       sure, but only with great effort and a lot of segfaults.)
//

#if !defined(CONE_EPOLL) && __linux__
// Use {0: select, 1: epoll} to wait for I/O. Default: 1 if supported (i.e. on Linux), else 0.
#define CONE_EPOLL 1
#endif

#if !defined(CONE_XCHG_RSP) && (__linux__ || __APPLE__) && __x86_64__
// Use {0: sigaltstack and setjmp/longjmp, 1: inline x86-64 assembly} to switch stacks.
// (Assembly is, of course, faster.) Default: 1 if supported (i.e. on x86-64 platforms
// that use the System V ABI), else 0.
#define CONE_XCHG_RSP 1
#endif

#ifndef CONE_DEFAULT_STACK
// How much space to allocate for a stack if 0 was passed to `cone_root`/`cone_spawn`.
// Must be at least MINSIGSTKSZ plus 256 bytes, else behavior is undefined.
#define CONE_DEFAULT_STACK 65536
#endif

#include "mun.h"

// A single function bound to some data. Naturally, does not own the pointer; manage
// it on your own.
//
// Initializer: designated (.code required, .data optional); also created by `cone_bind`.
// Finalizer: none.
//
struct cone_closure
{
    int (*code)(void*);
    void *data;
};

// Construct a `cone_closure` while casting the function to the correct type.
#define cone_bind(f, data) ((struct cone_closure){(int(*)(void*))f, data})

// A manually triggered event.
//
// Initializer: zero.
// Finalizer: `mun_vec_fini`; however, must not be destroyed if there are callbacks attached.
//
struct cone_event mun_vec(struct cone_closure);

// Call everything added with `cone_event_add`. If a callback fails, it is not removed
// from the queue, and no further callbacks are called/popped either.
//
// Errors:
//     any: depends on functions in the queue.
//
int cone_event_emit(struct cone_event *);

// The coroutine in which the code is currently executing.
extern _Thread_local struct cone * volatile cone;

// Switch a file descriptor into non-blocking mode.
//
// Errors:
//     `os`: see fcntl(2), particularly its F_GETFL and F_SETFL modes.
//
int cone_unblock(int fd);

// Create a coroutine on a new event loop, then block until all coroutines on it complete.
//
// Errors: see `cone_loop_init`, `cone_spawn`, `cone_loop_run`, `cone_join`.
//
int cone_root(size_t stack, struct cone_closure);

// Create a new coroutine that runs a given function with a single pointer argument.
// `size` is the size of the stack, including space for coroutine metadata; if there is
// not enough, default stack size is used. The returned coroutine has a reference count
// of 2: one reference for the calling function, and one for the event loop, dropped
// automatically when the coroutine terminates.
//
// Errors:
//     `memory`: could not allocate space for the stack.
//     `memory`: ran out of space while setting up the finalizing callbacks.
//     `memory`: ran out of space while scheduling the coroutine to be run.
//
struct cone *cone_spawn(size_t stack, struct cone_closure);

// Create a new coroutine with the default stack size.
#define cone(f, arg) cone_spawn(0, cone_bind(f, arg))

// If the coroutine is currently sleeping, arrange for it to be woken up with an error.
// If it is not, then this is the currently active coroutine, so just throw an error directly.
// No-op if the coroutine has already finished.
//
// Errors:
//     `cancelled`: if `c` is an active coroutine;
//     `memory`: if not, and ran out of space while scheduling for it to run.
//
int cone_cancel(struct cone *);

// Increment the reference count. The coroutine will not be destroyed until `cone_decref`
// is called the matching number of times.
void cone_incref(struct cone *);

// Decrement the reference count. If it becomes zero, destroy the coroutine; also,
// if the coroutine has failed with an error, print that to stderr.
//
// Errors: whatever the last error was if coroutine is NULL. Probably from `cone_spawn`.
//         (This is intended to make `cone_decref(cone_spawn(...))` correct.)
//
int cone_decref(struct cone *);

// Sleep until a coroutine finishes. If it happens to throw an error in the process,
// rethrow it into the current coroutine instead of printing. Consumes a single reference.
// Same as `cone_decref` if the coroutine has already finished.
//
// Errors:
//     `memory`, `cancelled`: see `cone_wait`;
//     any: depends on what the coroutine may throw.
//
int cone_join(struct cone *);

// Sleep until an event is fired manually.
//
// Errors:
//     `memory`: ran out of space while subscribing to the event.
//     `cancelled`: `cone_cancel` was called while this coroutine was sleeping.
//
int cone_wait(struct cone_event *);

// Sleep until a file descriptor is ready for reading/writing. Behavior is undefined
// if it already is (call read/write until it returns EAGAIN/EWOULDBLOCK).
//
// Errors:
//     `assert`: another coroutine is already waiting for the same event.
//     `memory`, `cancelled`: see `cone_wait`.
//
int cone_iowait(int fd, int write);

// Sleep just because. Don't get interrupted by signals.
//
// Errors: `memory`, `cancelled`; see `cone_wait.
//
int cone_sleep(mun_usec delay);

// Wait until the next iteration of the event loop.
//
// Errors:
//     `os`: could not send a ping to the event loop;
//     `memory`, `cancelled`: see `cone_loop_ping` and `cone_wait`.
//
int cone_yield(void);
