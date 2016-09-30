#pragma once
#include "../generic/callback.h"
#include "../generic/vec.h"

struct co_event_vec
{
    struct co_vec(struct co_callback) slots;
};

static inline void
co_event_vec_fini(struct co_event_vec *ev) {
    co_vec_fini(&ev->slots);
}

static inline int
co_event_vec_connect(struct co_event_vec *ev, struct co_callback cb) {
    return co_vec_append(&ev->slots, &cb);
}

static inline int
co_event_vec_disconnect(struct co_event_vec *ev, struct co_callback cb) {
    for (size_t i = 0; i < ev->slots.size; i++) {
        if (ev->slots.data[i].function == cb.function && ev->slots.data[i].data == cb.data) {
            co_vec_erase(&ev->slots, i);
            return 0;
        }
    }
    return -1;
}

static inline int
co_event_vec_emit(struct co_event_vec *ev) {
    size_t size;
    while ((size = ev->slots.size)) {
        struct co_callback r[size];
        memcpy(r, ev->slots.data, size * sizeof(struct co_callback));
        for (size_t i = 0; i < size; i++)
            if (r[i].function(r[i].data))
                return -1;  // TODO drop first i callbacks
        ev->slots.size = 0;
    }
    return 0;
}
