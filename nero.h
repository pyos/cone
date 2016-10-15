#pragma once
//
// nero // network romp
//
// As much of an RPC as C allows, anyway.
//
#ifndef NERO_MAX_FRAME_SIZE
// 1. Refuse to send frames bigger than this.
// 2. Close the connection upon receiving a frame bigger than this.
#define NERO_MAX_FRAME_SIZE 65535
#endif

#include "mun.h"
#include "cone.h"
#include "romp.h"

enum
{
    mun_errno_nero_overflow = mun_errno_custom + 13000,
    mun_errno_nero_protocol,
    mun_errno_nero_not_exported,
};

// The channel.
//
// Initializer: designated; must set `fd` and zero-initialize the rest.
// Finalizer: `nero_fini`.
//
struct nero
{
    int fd;
    unsigned last_id;
    struct cone *writer;
    struct romp wbuffer;
    struct romp rbuffer;
    struct mun_vec(struct nero_future *) queued;
    struct mun_vec(struct nero_closure) exported;
};

// A single exported function. Must do deserialization and serialization on its own
// because there's no way to construct varargs using standard C. Owns neither the name
// nor the data; make sure they are live until `nero_del` or `nero_fini` is called.
//
// Initializer: designated; also created by `nero_closure`.
// Finalizer: none.
//
struct nero_closure
{
    const char *name;
    int (*code)(struct nero *, void *, struct romp *in, struct romp *out);
    void *data;
};

// Create a `struct nero_closure` while casting the function to an appropriate type.
#define nero_closure(name, f, data) ((struct nero_closure){name, (int(*)(struct nero*,void*,struct romp*,struct romp*))(f), data})

// Export `c` functions atomically.
//
// Errors:
//     `memory`: no space left to extend the function table.
//
static inline int nero_add(struct nero *n, const struct nero_closure *cs, size_t c) {
    return mun_vec_extend(&n->exported, cs, c);
}

// Erase one exported function by name. Behavior is undefined if no such function exists.
static inline void nero_del(struct nero *n, const char *name) {
    mun_vec_erase(&n->exported, mun_vec_find(&n->exported, !strcmp(name, _->name)), 1);
}

// Finalizer of `struct nero`. Single use.
void nero_fini(struct nero *);

// Wait for incoming messages and handle them in a loop. The channel must not be destroyed
// while this is running; even after cancelling the coroutine blocked in this function,
// wait until it terminates first.
//
// Errors:
//     `os`: see read(2);
//     `memory`: could not allocate space for a read/write buffer;
//     `nero_protocol`: received an oversized, unknown, or invalid frame;
//     `nero_overflow`: a local handler wrote too much data into its result vector.
//
int nero_run(struct nero *);

// Call a remote function, block until a response arrives. `nero_run` must be active
// in another coroutine. Variadic arguments are (romp signature for arguments, arguments,
// romp signature for return values, pointers to locations of return values).
int nero_call(struct nero *, const char *function, ...);
int nero_call_var(struct nero *, const char *function, va_list);
