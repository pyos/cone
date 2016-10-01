#pragma once
#include "closure.h"
#include "../generic/vec_call_at.h"

struct co_event_schedule
{
    struct co_vec_call_at queue;
};

struct co_event_scheduler
{
    struct co_event_schedule *parent;
    struct co_nsec_offset delay;
};

static inline void
co_event_schedule_fini(struct co_event_schedule *ev) {
    co_vec_call_at_fini(&ev->queue);
}

static inline struct co_event_scheduler
co_event_schedule_after(struct co_event_schedule *ev, struct co_nsec_offset t) {
    return (struct co_event_scheduler){ev, t};
}

static inline struct co_nsec_offset
co_event_schedule_emit(struct co_event_schedule *ev) {
    struct co_nsec now = co_nsec_monotonic();
    while (ev->queue.size) {
        struct co_call_at next = ev->queue.data[0];
        if (co_u128_gt(next.time, now) && co_u128_gt(next.time, now = co_nsec_monotonic()))
            return co_u128_sub(next.time, now);
        co_vec_call_at_erase(&ev->queue, 0);
        if (next.cb.function(next.cb.data))
            return CO_U128_MAX;
    }
    return (struct co_nsec_offset){};
}

static inline int
co_event_scheduler_connect(struct co_event_scheduler *sc, struct co_closure cb) {
    struct co_event_schedule *ev = sc->parent;
    struct co_call_at tcb = {cb, co_u128_add(co_nsec_monotonic(), sc->delay)};
    for (size_t i = 0; i < ev->queue.size; i++)
        if (co_u128_lt(tcb.time, ev->queue.data[i].time))
            return co_vec_call_at_insert(&ev->queue, i, &tcb);
    return co_vec_call_at_append(&ev->queue, &tcb);
}
