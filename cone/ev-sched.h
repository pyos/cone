#pragma once
#include "ev.h"
#include "u128.h"

#include <time.h>

#define cone_nsec cone_u128

struct cone_call_at
{
    struct cone_closure f;
    struct cone_nsec time;
};

#include "vec_call_at.h"

struct cone_event_schedule
{
    struct cone_vec_call_at queue;
};

static inline struct cone_nsec
cone_nsec_from_timespec(struct timespec val) {
    return cone_u128_add(cone_u128_mul(CONE_U128(val.tv_sec), 1000000000ull), CONE_U128(val.tv_nsec));
}

static inline struct cone_nsec
cone_nsec_monotonic() {
    struct timespec val;
    return clock_gettime(CLOCK_MONOTONIC, &val) ? CONE_U128_MAX : cone_nsec_from_timespec(val);
}

static inline void
cone_event_schedule_fini(struct cone_event_schedule *ev) {
    cone_vec_call_at_fini(&ev->queue);
}

static inline struct cone_nsec
cone_event_schedule_emit(struct cone_event_schedule *ev) {
    while (ev->queue.size) {
        struct cone_nsec now = cone_nsec_monotonic();
        struct cone_call_at next = ev->queue.data[0];
        if (cone_u128_gt(next.time, now))
            return cone_u128_sub(next.time, now);
        cone_vec_call_at_erase(&ev->queue, 0, 1);
        if (cone_event_emit(&next.f))
            return CONE_U128(0);  // TODO not fail
    }
    return CONE_U128_MAX;
}

static inline int
cone_event_schedule_connect(struct cone_event_schedule *ev, struct cone_nsec delay, struct cone_closure cb) {
    struct cone_call_at r = {cb, cone_u128_add(cone_nsec_monotonic(), delay)};
    size_t left = 0, right = ev->queue.size;
    while (left != right) {
        size_t mid = (right + left) / 2;
        if (cone_u128_lt(r.time, ev->queue.data[mid].time))
            right = mid;
        else
            left = mid + 1;
    }
    return cone_vec_call_at_insert(&ev->queue, left, &r);
}
