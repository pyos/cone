#include "deck.h"

#define MAKE_NAME(lk, kind) \
    char kind[strlen((lk)->name) + sizeof(#kind)];  \
    strcpy(kind, (lk)->name);                       \
    strcpy(kind + strlen((lk)->name), "/" #kind)

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

static int deck_request_lt(struct deck_request a, struct deck_request b) {
    return a.time < b.time || (a.time == b.time && a.pid < b.pid);
}

static size_t deck_bisect(struct deck *lk, struct deck_request rq) {
    size_t left = 0, right = lk->queue.size;
    while (left != right) {
        size_t mid = (right + left) / 2;
        if (deck_request_lt(rq, lk->queue.data[mid]))
            right = mid;
        else
            left = mid + 1;
    }
    return left;
}

static int deck_maybe_wakeup(struct deck *lk) {
    if (lk->state & DECK_REQUESTED && lk->queue.data[0].pid == lk->pid) {
        lk->state &= ~DECK_REQUESTED;
        return cone_event_emit(&lk->wake);
    }
    return mun_ok;
}

static int deck_decode_request(struct nero *rpc, struct deck *lk, struct romp_iovec in, struct deck_request *rq) {
    if (romp_decode(in, "u4 u4", &rq->pid, &rq->time))
        return mun_error_up();
    if (lk->time < rq->time)
        lk->time = rq->time;
    lk->time++;
    for (unsigned int i = 0; i < lk->rpcs.size; i++) {
        if (lk->rpcs.data[i].rpc == rpc) {
            lk->rpcs.data[i].pid = rq->pid;
            break;
        }
    }
    return mun_ok;
}

static int deck_remote_request(struct nero *rpc, struct deck *lk, struct romp_iovec *in, struct romp_iovec *out) {
    (void)out;
    struct deck_request rq = {};
    if (deck_decode_request(rpc, lk, *in, &rq))
        return mun_error_up();
    return mun_vec_insert(&lk->queue, deck_bisect(lk, rq), &rq);
}

static int deck_remote_release(struct nero *rpc, struct deck *lk, struct romp_iovec *in, struct romp_iovec *out) {
    (void)out;
    struct deck_request rq = {};
    if (deck_decode_request(rpc, lk, *in, &rq))
        return mun_error_up();
    for (unsigned i = 0; i < lk->queue.size; i++)
        if (lk->queue.data[i].pid == rq.pid)
            return mun_vec_erase(&lk->queue, i, 1), deck_maybe_wakeup(lk);
    return mun_error(nero_protocol, "pid %u did not request this lock", rq.pid);
}

static void deck_kill(struct deck *lk, uint32_t i) {
    nero_fini(lk->rpcs.data[i].rpc);  // TODO something more gentle
}

static void deck_release_first(struct deck *lk, uint32_t i) {
    MAKE_NAME(lk, release);
    struct deck_request srq = {lk->pid, ++lk->time};
    while (i--)
        if (nero_call(lk->rpcs.data[i].rpc, release, "u4 u4", srq.pid, srq.time, ""))
            deck_kill(lk, i);
}

void deck_fini(struct deck *lk) {
    MAKE_NAME(lk, request);
    MAKE_NAME(lk, release);
    for (unsigned i = 0; i < lk->rpcs.size; i++) {
        nero_del(lk->rpcs.data[i].rpc, request);
        nero_del(lk->rpcs.data[i].rpc, release);
    }
    mun_vec_fini(&lk->rpcs);
    mun_vec_fini(&lk->queue);
    lk->state |= DECK_CANCELLED;
    cone_event_emit(&lk->wake);
}

int deck_add(struct deck *lk, struct nero *rpc) {
    MAKE_NAME(lk, request);
    MAKE_NAME(lk, release);
    struct nero_closure methods[] = {
        nero_closure(request, &deck_remote_request, lk),
        nero_closure(release, &deck_remote_release, lk),
    };
    struct deck_nero desc = {lk->pid, rpc};
    if (mun_vec_append(&lk->rpcs, &desc))
        return mun_error_up();
    if (nero_add(rpc, methods, sizeof(methods) / sizeof(struct nero_closure)))
        return mun_vec_erase(&lk->rpcs, lk->rpcs.size - 1, 1), mun_error_up();
    return mun_ok;
}

void deck_del(struct deck *lk, struct nero *rpc) {
    MAKE_NAME(lk, request);
    MAKE_NAME(lk, release);
    for (unsigned i = 0; i < lk->rpcs.size; i++) {
        if (lk->rpcs.data[i].rpc == rpc) {
            nero_del(rpc, request);
            nero_del(rpc, release);
            mun_vec_erase(&lk->rpcs, i, 1);
            uint32_t pid = lk->rpcs.data[i].pid;
            if (pid == lk->pid)
                break;
            for (unsigned j = 0; j < lk->queue.size; j++)
                if (lk->queue.data[j].pid == pid)
                    mun_vec_erase(&lk->queue, j, 1);
        }
    }
}

int deck_acquire(struct deck *lk) {
    if (deck_is_acquired_by_this(lk))
        return mun_ok;
    if (lk->state & DECK_CANCELLED)
        return cone_cancel(cone);
    if (!(lk->state & DECK_REQUESTED)) {
        lk->state |= DECK_REQUESTED;
        struct deck_request srq = {lk->pid, ++lk->time};
        if (mun_vec_append(&lk->queue, &srq))
            return mun_error_up();
        MAKE_NAME(lk, request);
        for (uint32_t i = 0; i < lk->rpcs.size; i++) {
            if (nero_call(lk->rpcs.data[i].rpc, request, "u4 u4", srq.pid, srq.time, "")) {
                mun_vec_erase(&lk->queue, deck_bisect(lk, srq), 1);
                deck_kill(lk, i);
                deck_release_first(lk, i);
                return mun_error_up();
            }
        }
        if (deck_maybe_wakeup(lk))
            return mun_error_up();
        if (deck_is_acquired_by_this(lk))
            return mun_ok;
    }
    if (cone_wait(&lk->wake))
        return mun_error_up();
    if (lk->state & DECK_CANCELLED)
        return cone_cancel(cone);
    return mun_ok;
}

int deck_release(struct deck *lk) {
    if (!deck_is_acquired_by_this(lk))
        return mun_error(assert, "not holding this lock");
    mun_vec_erase(&lk->queue, 0, 1);
    deck_release_first(lk, lk->rpcs.size);
    return mun_ok;
}
