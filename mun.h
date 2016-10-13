#pragma once
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

typedef int64_t mun_usec;

#define MUN_USEC_MAX INT64_MAX

static inline mun_usec mun_usec_now() {
    struct timeval val;
    return gettimeofday(&val, NULL) ? MUN_USEC_MAX : (mun_usec)val.tv_sec * 1000000ull + val.tv_usec;
}

#if __APPLE__ && __MACH__
mun_usec mun_usec_monotonic();
#else
static inline mun_usec mun_usec_monotonic() {
    struct timespec t;
    return clock_gettime(CLOCK_MONOTONIC, &t) ? MUN_USEC_MAX : (mun_usec)t.tv_sec*1000000ull + t.tv_nsec/1000;
}
#endif

enum
{
    mun_errno_cancelled       = ECANCELED,
    mun_errno_assert          = EINVAL,
    mun_errno_memory          = ENOMEM,
    mun_errno_not_implemented = ENOSYS,
    mun_errno_custom          = 100000,
};

struct mun_stackframe
{
    const char *file;
    const char *func;
    unsigned line;
};

struct mun_error
{
    int code;
    unsigned stacklen;
    const char *name;
    char text[128];
    struct mun_stackframe stack[16];
};

struct mun_error *mun_last_error(void);
int  mun_error_at(int, const char *name, struct mun_stackframe, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
int  mun_error_up_at(struct mun_stackframe);
void mun_error_show(const char *prefix, const struct mun_error *err);

#define MUN_CURRENT_FRAME ((struct mun_stackframe){__FILE__, __FUNCTION__, __LINE__})
#define mun_error(id, ...) mun_error_at(mun_errno_##id, #id, MUN_CURRENT_FRAME, __VA_ARGS__)
#define MUN_RETHROW        ? mun_error_up_at(MUN_CURRENT_FRAME) : 0
#define MUN_RETHROW_OS     ? mun_error_at(-errno, "errno", MUN_CURRENT_FRAME, "OS error") : 0

enum
{
    MUN_VEC_STATIC = 0x1,
};

#define mun_vec(T) { T* data; unsigned size, cap, shift, flags; }
#define mun_vec_init_static(T, n) {.data = (void*)(T[n]){}, .cap = n, .flags = MUN_VEC_STATIC}
#define mun_vec_init_borrow(ptr, n) {.data = (void*)ptr, .size = n, .cap = n, .flags = MUN_VEC_STATIC}
#define mun_vec_init_array(array) mun_vec_init_borrow(array, sizeof(array) / sizeof((array)[0]))
#define mun_vec_init_str(string) mun_vec_init_borrow(string, strlen(string))

struct mun_vec mun_vec(char);

static inline void mun_vec_fini_s(size_t stride, struct mun_vec *vec) {
    if (vec->flags & MUN_VEC_STATIC)
        return;
    free(vec->data - vec->shift * stride);
    *vec = (struct mun_vec){};
}

static inline int mun_vec_eq_s(size_t stride1, const struct mun_vec *a, size_t stride2, const struct mun_vec *b) {
    return stride1 == stride2 && a->size == b->size && !memcmp(a->data, b->data, a->size * stride1);
}

static inline void mun_vec_shift_s(size_t stride, struct mun_vec *vec, size_t start, int offset) {
    if (start < vec->size)
        memmove(vec->data + (start + offset) * stride, vec->data + start * stride, (vec->size - start) * stride);
    vec->size += offset;
}

static inline int mun_vec_reserve_s(size_t stride, struct mun_vec *vec, size_t n) {
    if (vec->size + n <= vec->cap - vec->shift)
        return 0;
    if (vec->size + n <= vec->cap) {
        memmove(vec->data - vec->shift * stride, vec->data, stride * vec->size);
        vec->data -= vec->shift * stride;
        vec->shift = 0;
        return 0;
    }
    if (vec->flags & MUN_VEC_STATIC)
        return mun_error(memory, "static vector of %u cannot fit %zu", vec->cap, vec->size + n);
    struct mun_vec v2 = {.size = vec->size, .cap = vec->cap + (n > vec->cap ? n : vec->cap)};
    if (!(v2.data = malloc(v2.cap * stride)))
        return mun_error(memory, "%u x %zu bytes", v2.cap, stride);
    memmove(v2.data, vec->data, vec->size * stride);
    free(vec->data - vec->shift * stride);
    *vec = v2;
    return 0;
}

static inline int mun_vec_splice_s(size_t stride, struct mun_vec *vec, size_t i, const void *elems, size_t n) {
    if (mun_vec_reserve_s(stride, vec, n))
        return -1;
    mun_vec_shift_s(stride, vec, i, n);
    memcpy(vec->data + i * stride, elems, n * stride);
    return 0;
}

static inline void mun_vec_erase_s(size_t stride, struct mun_vec *vec, size_t i, size_t n) {
    if (i)
        mun_vec_shift_s(stride, vec, i + n, -(int)n);
    else {
        vec->data  += n * stride;
        vec->size  -= n;
        vec->shift += n;
    }
}

#define mun_vec_strided(vec)              (size_t)sizeof(*(vec)->data), (struct mun_vec*)(vec)
#define mun_vec_eq(a, b)                  mun_vec_eq_s(mun_vec_strided(a), mun_vec_strided(b))
#define mun_vec_fini(vec)                 mun_vec_fini_s(mun_vec_strided(vec))
#define mun_vec_shift(vec, start, offset) mun_vec_shift_s(mun_vec_strided(vec), start, offset)
#define mun_vec_reserve(vec, elems)       mun_vec_reserve_s(mun_vec_strided(vec), elems)
#define mun_vec_splice(vec, i, elems, n)  mun_vec_splice_s(mun_vec_strided(vec), i, elems, n)
#define mun_vec_extend(vec, elems, n)     mun_vec_splice(vec, (vec)->size, elems, n)
#define mun_vec_insert(vec, i, elem)      mun_vec_splice(vec, i, elem, 1)
#define mun_vec_append(vec, elem)         mun_vec_extend(vec, elem, 1)
#define mun_vec_erase(vec, i, n)          mun_vec_erase_s(mun_vec_strided(vec), i, n)

#define mun_vec_find(vec, eqexpr) ({                   \
    unsigned __i = 0;                                  \
    __typeof__((vec)->data[0]) *_ = &(vec)->data[0];   \
    while (__i < (vec)->size && !(eqexpr)) __i++, _++; \
    __i;                                               \
})

#define mun_vec_bisect(vec, ltexpr) ({                                       \
    unsigned __L = 0, __R = (vec)->size, __M;                                \
    while (__L != __R) {                                                     \
        __typeof__((vec)->data[0]) *_ = &(vec)->data[__M = (__L + __R) / 2]; \
        if (ltexpr) __R = __M; else __L = __M + 1;                           \
    }                                                                        \
    __L;                                                                     \
})
