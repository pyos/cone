#pragma once
//
// nero // network romp // an RPC. Or as much of an RPC as C allows, anyway.
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

// The channel. Zero-initialized, except for `fd`, which must be a bidirectional
// file descriptor (e.g. a socket); finalized with `nero_fini`.
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
// Aggregate-initialized; also see `nero_closure` below.
struct nero_closure
{
    const char *name;
    const char *isign;
    const char *osign;
    int (*code)(struct nero *, void *, const void *in, void *out);
    void *data;
};

// Create a `struct nero_closure` while casting the function to an appropriate type.
#define nero_closure(name, f, isign, osign, data) \
    ((struct nero_closure){name, isign, osign, (int(*)(struct nero*,void*,const void*,void*))(f), data})

// Export `c` functions atomically.
//
// Errors: `memory`.
//
static inline int nero_add(struct nero *n, const struct nero_closure *cs, size_t c) {
    return mun_vec_extend(&n->exported, cs, c);
}

// Erase one exported function by name. Behavior is undefined if no such function exists.
static inline void nero_del(struct nero *n, const char *name) {
    mun_vec_erase(&n->exported, mun_vec_find(&n->exported, !strcmp(name, _->name)), 1);
}

// Finalizer of `struct nero`. Object state is undefined afterwards.
void nero_fini(struct nero *);

// Wait for incoming messages and handle them in a loop. The channel must not be destroyed
// while this is running; even after cancelling the coroutine blocked in this function,
// wait until it terminates first.
//
// Errors:
//   * see read(2);
//   * `nero_protocol`: received an oversized, unknown, or invalid frame;
//   * `nero_overflow`: a local handler wrote too much data into its result vector;
//   * `memory`.
//
int nero_run(struct nero *);

// Call a remote function, block until a response arrives. `nero_run` must be active
// in another coroutine. romp object packs are used to pass arguments (isign/i) and
// return values (osign/o); see `romp_encode` and `romp_decode` for their descriptions.
int nero_call(struct nero *, const char *f, const char *isign, const void *i, const char *osign, void *o);
