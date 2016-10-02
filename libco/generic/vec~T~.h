#pragma once
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

struct co_vec~T~
{
    ~T~* data;
    size_t size;
    size_t cap;
};

static inline void
co_vec~T~_fini(struct co_vec~T~ *vec) {
    free(vec->data);
    *vec = (struct co_vec~T~){NULL, 0, 0};
}

static inline int
co_vec~T~_reserve(struct co_vec~T~ *vec, size_t elems) {
    if (vec->size + elems <= vec->cap)
        return 0;
    size_t ncap = vec->cap + (elems > vec->cap ? elems : vec->cap);
    ~T~ *r = (~T~*) realloc(vec->data, sizeof(~T~) * ncap);
    if (r == NULL)
        return -1;
    vec->data = r;
    vec->cap = ncap;
    return 0;
}

static inline void
co_vec~T~_shift(struct co_vec~T~ *vec, size_t start, int offset) {
    if (start < vec->size)
        memmove(&vec->data[start + offset], &vec->data[start], sizeof(~T~) * (vec->size - start));
    vec->size += offset;
}

static inline int
co_vec~T~_insert(struct co_vec~T~ *vec, size_t i, const ~T~ *elem) {
    if (co_vec~T~_reserve(vec, 1))
        return -1;
    co_vec~T~_shift(vec, i, 1);
     vec->data[i] = *elem;
    return 0;
}

static inline int
co_vec~T~_append(struct co_vec~T~ *vec, const ~T~ *x) {
    return co_vec~T~_insert(vec, vec->size, x);
}

static inline void
co_vec~T~_erase(struct co_vec~T~ *vec, size_t i) {
    co_vec~T~_shift(vec, i + 1, -1);
}
