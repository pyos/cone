#pragma once
//
// mae // siy calls over file descriptors.
//
#include "mun.h"
#include "cone.h"
#include "siy.h"

enum
{
    mun_errno_mae_overflow = mun_errno_custom + 13000,
    mun_errno_mae_protocol,
    mun_errno_mae_not_exported,
};

// The channel. Zero-initialized, except for `fd`, which must be a bidirectional
// file descriptor (e.g. a socket); finalized with `mae_fini`.
struct mae
{
    int fd;
    unsigned last_id;
    struct cone *writer;
    struct siy wbuffer;
    struct siy rbuffer;
    struct mun_vec(struct mae_future *) queued;
    struct mun_vec(struct mae_closure) exported;
};

// A single exported function. Must do deserialization and serialization on its own
// because there's no way to construct varargs using standard C. Owns neither the name
// nor the data; make sure they are live until `mae_del` or `mae_fini` is called.
// Aggregate-initialized; also see `mae_closure` below.
struct mae_closure
{
    const char *name;
    const char *isign;
    const char *osign;
    int (*code)(struct mae *, void *, const void *in, void *out);
    void *data;
};

// Create a `struct mae_closure` while casting the function to an appropriate type.
#define mae_closure(name, f, isign, osign, data) \
    ((struct mae_closure){name, isign, osign, (int(*)(struct mae*,void*,const void*,void*))(f), data})

// Export `c` functions atomically.
//
// Errors: `memory`.
//
static inline int mae_add(struct mae *n, const struct mae_closure *cs, size_t c) {
    return mun_vec_extend(&n->exported, cs, c);
}

// Erase one exported function by name. Behavior is undefined if no such function exists.
static inline void mae_del(struct mae *n, const char *name) {
    mun_vec_erase(&n->exported, mun_vec_find(&n->exported, !strcmp(name, _->name)), 1);
}

// Finalizer of `struct mae`. Object state is undefined afterwards.
void mae_fini(struct mae *);

// Wait for incoming messages and handle them in a loop. The channel must not be destroyed
// while this is running; even after cancelling the coroutine blocked in this function,
// wait until it terminates first.
//
// Errors:
//   * see read(2);
//   * `mae_protocol`: received an oversized, unknown, or invalid frame;
//   * `mae_overflow`: a local handler wrote too much data into its result vector;
//   * `memory`.
//
int mae_run(struct mae *);

// Call a remote function, block until a response arrives. `mae_run` must be active
// in another coroutine. siy object packs are used to pass arguments (isign/i) and
// return values (osign/o); see `siy_encode` and `siy_decode` for their descriptions.
int mae_call(struct mae *, const char *f, const char *isign, const void *i, const char *osign, void *o);
