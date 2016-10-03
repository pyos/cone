#pragma once
#include "vec.h"
#include "../generic/u128.h"

#include <time.h>

#define co_nsec co_u128

struct co_call_at
{
    struct co_closure cb;
    struct co_nsec time;
};

#include "../generic/vec_call_at.h"

struct co_event_schedule
{
    struct co_vec_call_at queue;
};

static inline struct co_nsec
co_nsec_from_timespec(struct timespec val) {
    return co_u128_add(co_u128_mul(CO_U128(val.tv_sec), 1000000000ull), CO_U128(val.tv_nsec));
}

static inline struct co_nsec
co_nsec_monotonic() {
    struct timespec val;
    return clock_gettime(CLOCK_MONOTONIC, &val) ? CO_U128_MAX : co_nsec_from_timespec(val);
}

static inline void
co_event_schedule_fini(struct co_event_schedule *ev) {
    co_vec_call_at_fini(&ev->queue);
}

static inline struct co_nsec
co_event_schedule_emit(struct co_event_schedule *ev) {
    struct co_nsec now = co_nsec_monotonic();
    while (ev->queue.size) {
        struct co_call_at next = ev->queue.data[0];
        if (co_u128_gt(next.time, now) && co_u128_gt(next.time, now = co_nsec_monotonic()))
            return co_u128_sub(next.time, now);
        co_vec_call_at_erase(&ev->queue, 0);
        if (co_event_emit(&next.cb))
            return CO_U128(0);  // TODO not fail
    }
    return CO_U128_MAX;
}

static inline int
co_event_schedule_connect(struct co_event_schedule *ev, struct co_nsec delay, struct co_closure cb) {
    struct co_call_at tcb = {cb, co_u128_add(co_nsec_monotonic(), delay)};
    size_t left = 0, right = ev->queue.size;
    while (left != right) {
        size_t mid = (right + left) / 2;
        if (co_u128_lt(tcb.time, ev->queue.data[mid].time))
            right = mid;
        else
            left = mid + 1;
    }
    return co_vec_call_at_insert(&ev->queue, left, &tcb);
}
