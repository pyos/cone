#pragma once
#include <time.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t H, L; } mun_u128;

#define MUN_U128_MAX ((mun_u128){UINT64_MAX, UINT64_MAX})

static inline mun_u128 mun_u128_add(mun_u128 a, mun_u128 b) {
    return (mun_u128){a.H + b.H + (a.L + b.L < a.L), a.L + b.L};
}

static inline mun_u128 mun_u128_sub(mun_u128 a, mun_u128 b) {
    return (mun_u128){a.H - b.H - (a.L < b.L), a.L - b.L};
}

static inline mun_u128 mun_u128_mul(mun_u128 a, uint32_t b) {
    return (mun_u128){a.H * b + (((a.L >> 32) * b + ((a.L & UINT32_MAX) * b >> 32)) >> 32), a.L * b};
}

static inline mun_u128 mun_u128_div(mun_u128 a, uint32_t b) {
    uint64_t r = (a.H % b) << 32 | a.L >> 32;
    return (mun_u128){a.H / b, (r / b) << 32 | (((r % b) << 32 | (a.L & UINT32_MAX)) / b)};
}

static inline double mun_u128_to_double(mun_u128 x) {
    return (double)x.H * (1ull << 63) * 2 + x.L;
}

static inline int mun_u128_eq(mun_u128 a, mun_u128 b) {
    return a.H == b.H && a.L == b.L;
}

static inline int mun_u128_lt(mun_u128 a, mun_u128 b) {
    return a.H < b.H || (a.H == b.H && a.L < b.L);
}

static inline int mun_u128_gt(mun_u128 a, mun_u128 b) {
    return a.H > b.H || (a.H == b.H && a.L > b.L);
}


typedef mun_u128 mun_nsec;

static inline mun_nsec mun_nsec_from_timespec(struct timespec val) {
    return mun_u128_add(mun_u128_mul((mun_u128){0, val.tv_sec}, 1000000000ull), (mun_u128){0, val.tv_nsec});
}

static inline mun_nsec mun_nsec_now() {
    struct timespec val;
    return clock_gettime(CLOCK_REALTIME, &val) ? MUN_U128_MAX : mun_nsec_from_timespec(val);
}

static inline mun_nsec mun_nsec_monotonic() {
    struct timespec val;
    return clock_gettime(CLOCK_MONOTONIC, &val) ? MUN_U128_MAX : mun_nsec_from_timespec(val);
}


enum
{
    mun_ok = 0,
    mun_errno_os,
    mun_errno_assert,
    mun_errno_memory,
    mun_errno_not_implemented,
    mun_errno_custom = 128,
};

struct mun_stacktrace
{
    const char *file;
    const char *func;
    unsigned line;
};

struct mun_error
{
    unsigned code;
    unsigned stacklen;
    const char *name;
    char text[128];
    struct mun_stacktrace stack[16];
};

const struct mun_error *mun_last_error(void);
int  mun_error_restore(const struct mun_error *);
int  mun_error(unsigned, const char *name, const char *file, const char *func, unsigned line,
                const char *fmt, ...) __attribute__((format(printf, 6, 7)));
int  mun_error_up(const char *file, const char *func, unsigned line);
void mun_error_show(const char *prefix);

#define mun_error(id, ...) mun_error(mun_errno_##id, #id, __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__)
#define mun_error_os()     mun_error(os, "errno %d", errno)
#define mun_error_up()     mun_error_up(__FILE__, __FUNCTION__, __LINE__)


enum
{
    MUN_VEC_STATIC = 0x1,
};

#define mun_vec(T) { T* data; unsigned size, cap, shift, flags; }
#define mun_vec_init_static(T, n) {.data = (void*)(char[n * sizeof(T)]){}, .cap = n, .flags = MUN_VEC_STATIC}
#define mun_vec_init_borrow(ptr, n) {.data = (void*)ptr, .size = n, .cap = n, .flags = MUN_VEC_STATIC}
#define mun_vec_init_array(array) mun_vec_init_borrow(array, sizeof(array) / sizeof((array)[0]))
#define mun_vec_init_str(string) mun_vec_init_borrow(string, strlen(string))

struct mun_vec mun_vec(char);

static inline void mun_vec_fini_s(size_t stride, struct mun_vec *vec) {
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
        return mun_ok;
    if (vec->size + n <= vec->cap) {
        memmove(vec->data - vec->shift * stride, vec->data, stride * vec->size);
        vec->data -= vec->shift * stride;
        vec->shift = 0;
        return mun_ok;
    }
    if (vec->flags & MUN_VEC_STATIC)
        return mun_error(memory, "static vector of %u cannot fit %zu", vec->cap, vec->size + n);
    size_t ncap = vec->cap + (n > vec->cap ? n : vec->cap);
    void *r = realloc(vec->data - vec->shift * stride, stride * ncap);
    if (r == NULL)
        return mun_error(memory, "%zu x %zu bytes", ncap, stride);
    *vec = (struct mun_vec){.data = r, .size = vec->size, .cap = ncap};
    return mun_ok;
}

static inline int mun_vec_splice_s(size_t stride, struct mun_vec *vec, size_t i, const void *elems, size_t n) {
    if (mun_vec_reserve_s(stride, vec, n))
        return mun_error_up();
    mun_vec_shift_s(stride, vec, i, n);
    memcpy(vec->data + i * stride, elems, n * stride);
    return mun_ok;
}

static inline void mun_vec_erase_s(size_t stride, struct mun_vec *vec, size_t i, size_t n) {
    if (i + n == vec->size)
        vec->size -= n;
    else if (i)
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
