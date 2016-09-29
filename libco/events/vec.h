#pragma once
#include "../generic/vec.h"
#include "base.h"

struct co_event_vec
{
    struct co_event as_event;
    struct co_vec(struct co_callback) slots;
};

static inline int
co_event_vec_connect(struct co_event_vec *ev, struct co_callback cb) {
    return co_vec_append(&ev->slots, &cb);
}

static inline int
co_event_vec_disconnect(struct co_event_vec *ev, struct co_callback cb) {
    for (size_t i = 0; i < ev->slots.size; i++) {
        if (ev->slots.data[i].function != cb.function || ev->slots.data[i].data != cb.data)
            continue;
        co_vec_erase(&ev->slots, i);
        return 0;
    }
    return -1;
}

static inline int
co_event_vec_emit(struct co_event_vec *ev) {
    size_t size;
    while ((size = ev->slots.size)) {
        struct co_vec(struct co_callback) r = co_vec_move(&ev->slots);
        for (size_t i = 0; i < size; i++)
            if (r.data[i].function(r.data[i].data))
                return -1;
    }
    return 0;
}

static inline int
co_event_vec_move(struct co_event_vec *ev, struct co_event *target) {
    for (size_t i = 0; i < ev->slots.size; i++) {
        if (target->connect(target, ev->slots.data[i])) {
            while (i--)
                co_vec_erase(&ev->slots, i);
            return -1;
        }
    }
    co_vec_fini(&ev->slots);
    return 0;
}

static inline void
co_event_vec_init(struct co_event_vec *ev) {
    *ev = (struct co_event_vec){co_event_impl(&co_event_vec_connect, &co_event_vec_disconnect), {}};
}

static inline void
co_event_vec_fini(struct co_event_vec *ev) {
    co_vec_fini(&ev->slots);
}
