#pragma once

struct co_closure
{
    int (*function)(void*);
    void *data;
};

#include "../generic/vec_closure.h"

struct co_event_vec
{
    struct co_vec_closure slots;
};

#define co_bind(f, data) ((struct co_closure){(int(*)(void*))f, data})

static inline int
co_event_emit(struct co_closure *ev) {
    struct co_closure cb = *ev;
    *ev = (struct co_closure){};
    return cb.function && cb.function(cb.data);
}

static inline void
co_event_vec_fini(struct co_event_vec *ev) {
    co_vec_closure_fini(&ev->slots);
}

static inline int
co_event_vec_connect(struct co_event_vec *ev, struct co_closure cb) {
    return co_vec_closure_append(&ev->slots, &cb);
}

static inline int
co_event_vec_emit(struct co_event_vec *ev) {
    struct co_vec_closure r = ev->slots;
    ev->slots = (struct co_vec_closure){};
    for (size_t i = 0; i < r.size; i++)
        if (co_event_emit(&r.data[i]))
            return co_vec_closure_fini(&r), -1;
    return co_vec_closure_fini(&r), 0;
}
