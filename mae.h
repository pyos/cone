#pragma once
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

struct mae_closure
{
    const char *name;
    const char *isign;
    const char *osign;
    int (*code)(struct mae *, void *, const void *in, void *out);
    void *data;
};

#define mae_closure(name, f, isign, osign, data) \
    ((struct mae_closure){name, isign, osign, (int(*)(struct mae*,void*,const void*,void*))(f), data})

void mae_fini(struct mae *);

// Export N `mae_closure`s atomically.
static inline mun_throws(memory) int mae_add(struct mae *m, const struct mae_closure *cs, size_t n) {
    return mun_vec_extend(&m->exported, cs, n);
}

// Erase one exported function by name. Behavior is undefined if no such function exists.
static inline void mae_del(struct mae *m, const char *name) {
    mun_vec_erase(&m->exported, mun_vec_find(&m->exported, !strcmp(name, _->name)), 1);
}

// Wait for incoming messages and handle them in a loop. The channel must not be destroyed
// while this is running; even after cancelling the coroutine blocked in this function,
// wait until it terminates first.
int mae_run(struct mae *) mun_throws(memory, mae_overflow, mae_protocol);

// Call a remote function, block until a response arrives. `mae_run` must be active
// in another coroutine. siy object packs are used to pass arguments (isign/i) and
// return values (osign/o); see `siy_encode` and `siy_decode` for their descriptions.
int mae_call(struct mae *, const char *f, const char *isign, const void *i, const char *osign, void *o)
    mun_throws(memory, siy_truncated, siy_sign_syntax);
