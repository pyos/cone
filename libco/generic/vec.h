#pragma once
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

struct co_vec
{
    void* data;
    size_t size;
    size_t cap;
};

#define co_vec(T) {             \
    union {                     \
        struct co_vec __co_vec; \
        struct {                \
            T* data;            \
            size_t size;        \
            size_t cap;         \
        };                      \
    };                          \
}

#define co_vec_append(vec, it)    __co_vec_insert(&(vec)->__co_vec, sizeof(*(vec)->data), (vec)->size, it)
#define co_vec_insert(vec, i, it) __co_vec_insert(&(vec)->__co_vec, sizeof(*(vec)->data), i, it)
#define co_vec_erase(vec, i)      __co_vec_erase (&(vec)->__co_vec, sizeof(*(vec)->data), i)
#define co_vec_fini(vec)          __co_vec_fini  (&(vec)->__co_vec)

#define __co_vec_elemptr(vec, elemsize) ((char (*)[elemsize]) (vec)->data)

static inline void
__co_vec_fini(struct co_vec *vec) {
    free(vec->data);
    *vec = (struct co_vec){NULL, 0, 0};
}

static inline int
__co_vec_reserve(struct co_vec *vec, size_t elemsize, size_t elems) {
    if (vec->size + elems > vec->cap) {
        size_t ncap = vec->cap + (elems > vec->cap ? elems : vec->cap);
        void *r = realloc(vec->data, elemsize * ncap);
        if (r == NULL)
            return -1;
        vec->data = r;
        vec->cap = ncap;
    }
    return 0;
}

static inline void
__co_vec_shift(struct co_vec *vec, size_t elemsize, size_t start, int offset) {
    if (start < vec->size)
        memmove(__co_vec_elemptr(vec, elemsize) + start + offset,
                __co_vec_elemptr(vec, elemsize) + start,
                elemsize * (vec->size - start));
    vec->size += offset;
}

static inline int
__co_vec_insert(struct co_vec *vec, size_t elemsize, size_t i, const void *elem) {
    if (__co_vec_reserve(vec, elemsize, 1))
        return -1;
    __co_vec_shift(vec, elemsize, i, 1);
    memcpy(__co_vec_elemptr(vec, elemsize) + i, elem, elemsize);
    return 0;
}

static inline void
__co_vec_erase(struct co_vec *vec, size_t elemsize, size_t i) {
    __co_vec_shift(vec, elemsize, i + 1, -1);
}
