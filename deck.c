#include "deck.h"
#if DECK_DEBUG
#include <stdio.h>
#include <inttypes.h>
#define deck_debug_msg(t, pid, fmt, ...) fprintf(stderr, "[%" PRIu64 "|%u] %u: " fmt "\n", mun_usec_now(), t, pid, ##__VA_ARGS__)
#else
#define deck_debug_msg(t, pid, fmt, ...)
#endif

enum deck_state
{
    DECK_RECURSION = 0x00FFFFFFul,
    DECK_REQUESTED = 0x01000000ul,
    DECK_ACKED     = 0x02000000ul,
    DECK_CANCELLED = 0x04000000ul,
};

struct deck_nero
{
    uint32_t pid;
    struct nero *rpc;
};

struct deck_request
{
    uint32_t pid;
    uint32_t time;
};

struct deck_reqptr
{
    struct deck *lk;
    struct nero *rpc;
    const char *method;
    struct deck_request rq;
};

void deck_fini(struct deck *lk) {
    lk->state |= DECK_CANCELLED;
    for (unsigned i = 0; i < lk->rpcs.size; i++) {
        nero_del(lk->rpcs.data[i].rpc, lk->fname_request.data);
        nero_del(lk->rpcs.data[i].rpc, lk->fname_release.data);
    }
    mun_vec_fini(&lk->fname_request);
    mun_vec_fini(&lk->fname_release);
    mun_vec_fini(&lk->rpcs);
    mun_vec_fini(&lk->queue);
    cone_event_emit(&lk->wake);
}

int deck_acquired(struct deck *lk) {
    return lk->state & DECK_ACKED && lk->queue.data[0].pid == lk->pid;
}

static int deck_wake(struct deck *lk) {
    if (!(lk->state & DECK_REQUESTED) || !deck_acquired(lk))
        return 0;
    lk->state &= ~DECK_REQUESTED;
    lk->time++;
    deck_debug_msg(lk->time, lk->pid, "acquire");
    return cone_event_emit(&lk->wake);
}

static unsigned deck_bisect(struct deck *lk, struct deck_request rq) {
    return mun_vec_bisect(&lk->queue, rq.time < _->time || (rq.time == _->time && rq.pid < _->pid));
}

static int deck_remote_clock(struct nero *rpc, struct deck *lk, struct romp *in, struct romp *out, struct deck_request *rq) {
    if (romp_decode(in, "u4 u4", &rq->pid, &rq->time))
        return -1;
    lk->rpcs.data[mun_vec_find(&lk->rpcs, _->rpc == rpc)].pid = rq->pid;
    return romp_encode(out, "u4", lk->time = (rq->time > lk->time ? rq->time : lk->time) + 1);
}

static int deck_remote_request(struct nero *rpc, struct deck *lk, struct romp *in, struct romp *out) {
    struct deck_request rq = {};
    if (deck_remote_clock(rpc, lk, in, out, &rq) MUN_RETHROW)
        return -1;
    deck_debug_msg(lk->time, rq.pid, "request");
    return mun_vec_insert(&lk->queue, deck_bisect(lk, rq), &rq);
}

static int deck_remote_release(struct nero *rpc, struct deck *lk, struct romp *in, struct romp *out) {
    struct deck_request rq = {};
    if (deck_remote_clock(rpc, lk, in, out, &rq) MUN_RETHROW)
        return -1;
    unsigned i = mun_vec_find(&lk->queue, _->pid == rq.pid);
    if (i == lk->queue.size)
        return mun_error(nero_protocol, "%u: %u did not request this lock", lk->pid, rq.pid);
    deck_debug_msg(lk->time, rq.pid, "release");
    mun_vec_erase(&lk->queue, i, 1);
    return i == 0 ? deck_wake(lk) : 0;
}

static int deck_call_one(struct deck_reqptr *rp) {
    uint32_t time = 0;
    if (nero_call(rp->rpc, rp->method, "u4 u4", rp->rq.pid, rp->rq.time, "u4", &time) MUN_RETHROW)
        return -1;
    rp->lk->time = (time > rp->lk->time ? time : rp->lk->time) + 1;
    return 0;
}

static int deck_call_all(struct deck *lk, const char *method, struct deck_request rq) {
    int fail = 0;
    struct deck_reqptr reqs[lk->rpcs.size];
    for (unsigned i = 0; i < lk->rpcs.size; i++)
        reqs[i] = (struct deck_reqptr){lk, lk->rpcs.data[i].rpc, method, rq};
    struct cone *tasks[lk->rpcs.size];
    for (unsigned i = 0; i < lk->rpcs.size; i++)
        if ((tasks[i] = cone(deck_call_one, &reqs[i])) == NULL MUN_RETHROW)
            fail = -1;
    for (unsigned i = 0; i < lk->rpcs.size; i++)
        if (!fail)
            fail = cone_join(tasks[i]) MUN_RETHROW;
        else if (tasks[i] != NULL)
            cone_cancel(tasks[i]), cone_decref(tasks[i]);
    return fail;
}

#define ENSURE_NAME_CREATED(lk, N) do \
    if (!(lk)->fname_##N.data || !strncmp((lk)->name, (lk)->fname_##N.data, (lk)->fname_##N.size)) { \
        mun_vec_erase(&(lk)->fname_##N, 0, (lk)->fname_##N.size);                                    \
        if (mun_vec_reserve(&(lk)->fname_##N, strlen((lk)->name) + sizeof(#N) + 1) MUN_RETHROW)      \
            return -1;                                                                               \
        strcpy((lk)->fname_##N.data, (lk)->name);                                                    \
        strcpy((lk)->fname_##N.data + strlen((lk)->name), "/" #N);                                   \
    } while (0)

int deck_add(struct deck *lk, struct nero *rpc) {
    ENSURE_NAME_CREATED(lk, request);
    ENSURE_NAME_CREATED(lk, release);
    struct nero_closure methods[] = {
        nero_closure(lk->fname_request.data, &deck_remote_request, lk),
        nero_closure(lk->fname_release.data, &deck_remote_release, lk),
    };
    struct deck_nero desc = {lk->pid, rpc};
    if (mun_vec_append(&lk->rpcs, &desc) MUN_RETHROW)
        return -1;
    if (nero_add(rpc, methods, sizeof(methods) / sizeof(struct nero_closure)) MUN_RETHROW)
        return mun_vec_erase(&lk->rpcs, lk->rpcs.size - 1, 1), -1;
    return 0;
}

void deck_del(struct deck *lk, struct nero *rpc) {
    unsigned i = mun_vec_find(&lk->rpcs, _->rpc == rpc);
    uint32_t pid = lk->rpcs.data[i].pid;
    nero_del(rpc, lk->fname_request.data);
    nero_del(rpc, lk->fname_release.data);
    mun_vec_erase(&lk->rpcs, i, 1);
    if (pid != lk->pid) {
        unsigned j = mun_vec_find(&lk->queue, _->pid == pid);
        if (j != lk->queue.size)
            mun_vec_erase(&lk->queue, j, 1);
    }
}

static int deck_release_impl(struct deck *lk, int cancelling) {
    struct deck_request crq = {lk->pid, ++lk->time};
    deck_debug_msg(lk->time, lk->pid, "%s", cancelling ? "cancel" : "release");
    mun_vec_erase(&lk->queue, mun_vec_find(&lk->queue, _->pid == lk->pid), 1);
    return deck_call_all(lk, lk->fname_release.data, crq);
}

int deck_acquire(struct deck *lk) {
    while (!deck_acquired(lk)) {
        if (lk->state & DECK_CANCELLED)
            return cone_cancel(cone);
        if (!(lk->state & DECK_REQUESTED)) {
            struct deck_request arq = {lk->pid, ++lk->time};
            if (mun_vec_append(&lk->queue, &arq) MUN_RETHROW)
                return -1;
            deck_debug_msg(lk->time, lk->pid, "request");
            lk->state |= DECK_REQUESTED;
            if (deck_call_all(lk, lk->fname_request.data, arq))
                return lk->state &= ~DECK_REQUESTED, deck_release_impl(lk, 1), -1;
            lk->state |= DECK_ACKED;
            if (deck_wake(lk) MUN_RETHROW)
                return -1;
        } else if (cone_wait(&lk->wake) MUN_RETHROW)
            return -1;
    }
    lk->state++;
    return 0;
}

int deck_release(struct deck *lk) {
    if (!deck_acquired(lk))
        return mun_error(assert, "not holding this lock");
    return --lk->state & DECK_RECURSION ? 0 : deck_release_impl(lk, 0);
}
