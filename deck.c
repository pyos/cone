#include "deck.h"

enum deck_state
{
    DECK_REQUESTED = 0x1,
    DECK_CANCELLED = 0x2,
};

struct deck_nero
{
    uint32_t pid;
    struct nero *rpc;
};

static size_t deck_bisect(struct deck *lk, struct deck_request rq) {
    return mun_vec_bisect(&lk->queue, it, rq.time < it->time || (rq.time == it->time && rq.pid < it->pid));
}

static int deck_maybe_wakeup(struct deck *lk) {
    if (lk->state & DECK_REQUESTED && lk->queue.data[0].pid == lk->pid)
        return lk->state &= ~DECK_REQUESTED, cone_event_emit(&lk->wake);
    return mun_ok;
}

static int deck_clock_server(struct nero *rpc, struct deck *lk, struct romp *in, struct romp *out, struct deck_request *rq) {
    if (romp_decode(in, "u4 u4", &rq->pid, &rq->time))
        return mun_error_up();
    if (lk->time < rq->time)
        lk->time = rq->time;
    lk->time++;
    lk->rpcs.data[mun_vec_find(&lk->rpcs, it, it->rpc == rpc)].pid = rq->pid;
    return romp_encode(out, "u4", lk->time);
}

static int deck_remote_request(struct nero *rpc, struct deck *lk, struct romp *in, struct romp *out) {
    struct deck_request rq = {};
    if (deck_clock_server(rpc, lk, in, out, &rq))
        return mun_error_up();
    return mun_vec_insert(&lk->queue, deck_bisect(lk, rq), &rq);
}

static int deck_remote_release(struct nero *rpc, struct deck *lk, struct romp *in, struct romp *out) {
    struct deck_request rq = {};
    if (deck_clock_server(rpc, lk, in, out, &rq))
        return mun_error_up();
    unsigned i = mun_vec_find(&lk->queue, it, it->pid == rq.pid);
    if (i == lk->queue.size)
        return mun_error(nero_protocol, "pid %u did not request this lock", rq.pid);
    return mun_vec_erase(&lk->queue, i, 1), deck_maybe_wakeup(lk);
}

static int deck_release_first(struct deck *lk, uint32_t i) {
    uint32_t time = lk->time;
    struct deck_request srq = {lk->pid, ++lk->time};
    while (i--) {
        if (nero_call(lk->rpcs.data[i].rpc, lk->fname_release.data, "u4 u4", srq.pid, srq.time, "u4", &time))
            return mun_error_up();
        srq.time = lk->time = (time > lk->time ? time : lk->time) + 1;
    }
    return mun_ok;
}

void deck_fini(struct deck *lk) {
    for (unsigned i = 0; i < lk->rpcs.size; i++) {
        nero_del(lk->rpcs.data[i].rpc, lk->fname_request.data);
        nero_del(lk->rpcs.data[i].rpc, lk->fname_release.data);
    }
    mun_vec_fini(&lk->fname_request);
    mun_vec_fini(&lk->fname_release);
    mun_vec_fini(&lk->rpcs);
    mun_vec_fini(&lk->queue);
    lk->state |= DECK_CANCELLED;
    cone_event_emit(&lk->wake);
}

#define ENSURE_NAME_CREATED(lk, N) do \
    if (!(lk)->fname_##N.data || !strncmp((lk)->name, (lk)->fname_##N.data, (lk)->fname_##N.size)) { \
        mun_vec_erase(&(lk)->fname_##N, 0, (lk)->fname_##N.size);                                    \
        if (mun_vec_reserve(&(lk)->fname_##N, strlen((lk)->name) + sizeof(#N) + 1))                  \
            return mun_error_up();                                                                   \
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
    if (mun_vec_append(&lk->rpcs, &desc))
        return mun_error_up();
    if (nero_add(rpc, methods, sizeof(methods) / sizeof(struct nero_closure)))
        return mun_vec_erase(&lk->rpcs, lk->rpcs.size - 1, 1), mun_error_up();
    return mun_ok;
}

void deck_del(struct deck *lk, struct nero *rpc) {
    unsigned i = mun_vec_find(&lk->rpcs, it, it->rpc == rpc);
    uint32_t pid = lk->rpcs.data[i].pid;
    nero_del(rpc, lk->fname_request.data);
    nero_del(rpc, lk->fname_release.data);
    mun_vec_erase(&lk->rpcs, i, 1);
    if (pid != lk->pid) {
        unsigned j = mun_vec_find(&lk->queue, it, it->pid == pid);
        if (j != lk->queue.size)
            mun_vec_erase(&lk->queue, j, 1);
    }
}

int deck_acquire(struct deck *lk) {
    while (!deck_is_acquired_by_this(lk)) {
        if (lk->state & DECK_CANCELLED)
            return cone_cancel(cone);
        if (!(lk->state & DECK_REQUESTED)) {
            struct deck_request srq = {lk->pid, ++lk->time};
            if (mun_vec_append(&lk->queue, &srq))
                return mun_error_up();
            lk->state |= DECK_REQUESTED;
            for (uint32_t i = 0; i < lk->rpcs.size; i++) {
                uint32_t time = lk->time;
                if (nero_call(lk->rpcs.data[i].rpc, lk->fname_request.data, "u4 u4", srq.pid, srq.time, "u4", &time)) {
                    mun_vec_erase(&lk->queue, deck_bisect(lk, srq), 1);
                    deck_release_first(lk, i);
                    lk->state &= ~DECK_REQUESTED;
                    return mun_error_up();
                } else
                    srq.time = lk->time = (time > lk->time ? time : lk->time) + 1;
            }
            if (deck_maybe_wakeup(lk))
                return mun_error_up();
        } else if (cone_wait(&lk->wake))
            return mun_error_up();
    }
    return mun_ok;
}

int deck_release(struct deck *lk) {
    if (!deck_is_acquired_by_this(lk))
        return mun_error(assert, "not holding this lock");
    mun_vec_erase(&lk->queue, 0, 1);
    return deck_release_first(lk, lk->rpcs.size);
}
