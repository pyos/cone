#pragma once
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

struct co_vec~T~
{
    ~T~* data;
    unsigned size;
    unsigned cap;
    unsigned shift;
};

static inline void
co_vec~T~_fini(struct co_vec~T~ *vec) {
    free(vec->data - vec->shift);
    *vec = (struct co_vec~T~){};
}

static inline void
co_vec~T~_shift(struct co_vec~T~ *vec, size_t start, int offset) {
    if (start < vec->size)
        memmove(&vec->data[start + offset], &vec->data[start], sizeof(~T~) * (vec->size - start));
    vec->size += offset;
}

static inline int
co_vec~T~_reserve(struct co_vec~T~ *vec, size_t elems) {
    if (vec->size + elems <= vec->cap)
        return 0;
    if (vec->shift) {
        vec->data -= vec->shift;
        vec->size += vec->shift;
        vec->cap  += vec->shift;
        co_vec~T~_shift(vec, vec->shift, -(int)vec->shift);
        vec->shift = 0;
        if (vec->size + elems <= vec->cap)
            return 0;
    }
    size_t ncap = vec->cap + (elems > vec->cap ? elems : vec->cap);
    ~T~ *r = (~T~*) realloc(vec->data, sizeof(~T~) * ncap);
    if (r == NULL)
        return -1;
    vec->data = r;
    vec->cap = ncap;
    return 0;
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
co_vec~T~_erase(struct co_vec~T~ *vec, size_t i, size_t n) {
    if (i + n == vec->size)
        vec->size -= n;
    else if (i == 0) {
        vec->data  += n;
        vec->size  -= n;
        vec->cap   -= n;
        vec->shift += n;
    } else
        co_vec~T~_shift(vec, i + n, -(int)n);
}
