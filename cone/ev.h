#pragma once

struct cone_closure
{
    int (*code)(void*);
    void *data;
};

#include "vec_closure.h"

struct cone_event_vec
{
    struct cone_vec_closure slots;
};

#define cone_bind(f, data) ((struct cone_closure){(int(*)(void*))f, data})

static inline int
cone_event_emit(struct cone_closure *ev) {
    struct cone_closure cb = *ev;
    *ev = (struct cone_closure){};
    return cb.code && cb.code(cb.data);
}

static inline void
cone_event_vec_fini(struct cone_event_vec *ev) {
    cone_vec_closure_fini(&ev->slots);
}

static inline int
cone_event_vec_connect(struct cone_event_vec *ev, struct cone_closure cb) {
    return cone_vec_closure_insert(&ev->slots, ev->slots.size, &cb);
}

static inline int
cone_event_vec_emit(struct cone_event_vec *ev) {
    while (ev->slots.size) {
        if (cone_event_emit(&ev->slots.data[0]))
            return -1;  // TODO not fail
        cone_vec_closure_erase(&ev->slots, 0, 1);
    }
    return 0;
}
