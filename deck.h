#pragma once
//
// deck // Lamport mutex
//
#include "mun.h"
#include "cone.h"
#include "mae.h"

// The lock. Zero-initialized, except for `pid`, which must be unique among nodes.
// Finalized with `deck_fini`.
struct deck
{
    uint32_t pid;
    uint32_t time;
    unsigned state;
    struct cone_event wake;
    struct mun_vec(struct deck_mae) rpcs;
    struct mun_vec(struct deck_request) queue;
};

// Finalizer of `struct deck`. Object state is undefined afterwards.
void deck_fini(struct deck *);

// Add another participant in the lock. Behavior is undefined if lock has been requested,
// but not released yet. `request` and `release` must survive until `deck_del` or `deck_fini`.
//
// Errors: `memory`.
//
int deck_add(struct deck *, struct mae *, const char *request, const char *release);

// Forget about an RPC channel. No-op if it was never added.
void deck_del(struct deck *, struct mae *);

// Request the lock and block until it is acquired. Or just block if another coroutine
// has already sent a request.
//
// Errors: `memory`; possibly other errors if peers are misbehaving.
//
int deck_acquire(struct deck *);

// Undo one call to `deck_acquire`. The lock is recursive, so will not be released
// until this function has been called the same number of times as `deck_acquire` has.
int deck_release(struct deck *);

// Return 1 if `deck_acquire` would not block.
int deck_acquired(struct deck *d);
