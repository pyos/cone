#pragma once
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

struct cone_vec~T~
{
    ~T~* data;
    unsigned size;
    unsigned cap;
    unsigned shift;
};

static inline void
cone_vec~T~_fini(struct cone_vec~T~ *vec) {
    free(vec->data - vec->shift);
    *vec = (struct cone_vec~T~){};
}

static inline void
cone_vec~T~_shift(struct cone_vec~T~ *vec, size_t start, int offset) {
    if (start < vec->size)
        memmove(&vec->data[start + offset], &vec->data[start], sizeof(~T~) * (vec->size - start));
    vec->size += offset;
}

static inline int
cone_vec~T~_reserve(struct cone_vec~T~ *vec, size_t elems) {
    if (vec->size + elems <= vec->cap)
        return 0;
    if (vec->shift) {
        vec->data -= vec->shift;
        vec->size += vec->shift;
        vec->cap  += vec->shift;
        cone_vec~T~_shift(vec, vec->shift, -(int)vec->shift);
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
cone_vec~T~_insert(struct cone_vec~T~ *vec, size_t i, const ~T~ *elem) {
    if (cone_vec~T~_reserve(vec, 1))
        return -1;
    cone_vec~T~_shift(vec, i, 1);
    vec->data[i] = *elem;
    return 0;
}

static inline void
cone_vec~T~_erase(struct cone_vec~T~ *vec, size_t i, size_t n) {
    if (i + n == vec->size)
        vec->size -= n;
    else if (i == 0) {
        vec->data  += n;
        vec->size  -= n;
        vec->cap   -= n;
        vec->shift += n;
    } else
        cone_vec~T~_shift(vec, i + n, -(int)n);
}
