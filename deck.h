#pragma once
//
// deck // distributed lock
//
#include "mun.h"
#include "cone.h"
#include "romp.h"
#include "nero.h"

// The lock.
//
// Initializer: designated; `pid` = some unique number, zero-initialize the rest.
// Finalizer: `deck_fini`.
//
struct deck
{
    uint32_t pid;
    uint32_t time;
    unsigned state;
    struct cone_event wake;
    struct mun_vec(struct deck_nero) rpcs;
    struct mun_vec(struct deck_request) queue;
};

// Finalizer of `struct deck`. Single use.
void deck_fini(struct deck *);

// Add another participant in the lock. Behavior is undefined if lock has been requested,
// but not released yet. `request` and `release` must survive until `deck_del` or `deck_fini`.
//
// Errors:
//     `memory`: not enough space to record connection metadata.
//
int deck_add(struct deck *, struct nero *, const char *request, const char *release);

// Forget about an RPC channel. Behavior is undefined if it was never added.
// Other that that, should be OK to call at any point.
void deck_del(struct deck *, struct nero *);

// Request the lock and block until it is acquired. Or just block if another coroutine
// has already sent a request.
//
// Errors:
//     `memory`: could not add the request to the queue;
//     `memory`: see `cone_spawn`, `cone_wait`, and `cone_event_emit`;
//     possibly other errors returned by the remote side. All of them are exceptional.
//
int deck_acquire(struct deck *);

// Undo one call to `deck_acquire`. The lock is recursive, so will not be released
// until this function has been called the same number of times as `deck_acquire` has.
int deck_release(struct deck *);

// Return 1 if `deck_acquire` would not block.
int deck_acquired(struct deck *d);
