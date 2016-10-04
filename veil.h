#pragma once
#include <time.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t H, L; } veil_u128;

#define COT_U128_MAX ((veil_u128){UINT64_MAX, UINT64_MAX})

static inline veil_u128 veil_u128_add(veil_u128 a, veil_u128 b) {
    return (veil_u128){a.H + b.H + (a.L + b.L < a.L), a.L + b.L};
}

static inline veil_u128 veil_u128_sub(veil_u128 a, veil_u128 b) {
    return (veil_u128){a.H - b.H - (a.L < b.L), a.L - b.L};
}

static inline veil_u128 veil_u128_mul(veil_u128 a, uint32_t b) {
    return (veil_u128){a.H * b + (((a.L >> 32) * b) >> 32), a.L * b};
}

static inline veil_u128 veil_u128_div(veil_u128 a, uint32_t b) {
    uint64_t r = (a.H % b) << 32 | a.L >> 32;
    return (veil_u128){a.H / b, (r / b) << 32 | (((r % b) << 32 | (a.L & UINT32_MAX)) / b)};
}

static inline double veil_u128_to_double(veil_u128 x) {
    return (double)x.H * (1ull << 63) * 2 + x.L;
}

static inline int veil_u128_eq(veil_u128 a, veil_u128 b) {
    return a.H == b.H && a.L == b.L;
}

static inline int veil_u128_lt(veil_u128 a, veil_u128 b) {
    return a.H < b.H || (a.H == b.H && a.L < b.L);
}

static inline int veil_u128_gt(veil_u128 a, veil_u128 b) {
    return a.H > b.H || (a.H == b.H && a.L > b.L);
}

typedef veil_u128 veil_nsec;

static inline veil_nsec veil_nsec_from_timespec(struct timespec val) {
    return veil_u128_add(veil_u128_mul((veil_u128){0, val.tv_sec}, 1000000000ull), (veil_u128){0, val.tv_nsec});
}

static inline veil_nsec veil_nsec_now() {
    struct timespec val;
    return clock_gettime(CLOCK_REALTIME, &val) ? COT_U128_MAX : veil_nsec_from_timespec(val);
}

static inline veil_nsec veil_nsec_monotonic() {
    struct timespec val;
    return clock_gettime(CLOCK_MONOTONIC, &val) ? COT_U128_MAX : veil_nsec_from_timespec(val);
}

enum
{
    veil_ok = 0,
    veil_errno_os,
    veil_errno_assert,
    veil_errno_memory,
    veil_errno_custom = 128,
};

struct veil_stacktrace
{
    const char *file;
    const char *func;
    unsigned line;
};

struct veil_error
{
    unsigned code;
    unsigned stacklen;
    const char *name;
    const char *text;
    const struct veil_stacktrace *stack;
};

const struct veil_error *veil_last_error(void);

int veil_error(unsigned, const char *name, const char *file, const char *func, unsigned line,
               const char *fmt, ...) __attribute__((format(printf, 6, 7)));

int veil_error_up(const char *file, const char *func, unsigned line);

void veil_error_show(const char *prefix);

#define veil_error(id, ...) veil_error(veil_errno_##id, #id, __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__)
#define veil_error_os()     veil_error(os, "errno %d", errno)
#define veil_error_up()     veil_error_up(__FILE__, __FUNCTION__, __LINE__)

#define veil_vec(T) { T* data; unsigned size, cap, shift; }

struct veil_vec veil_vec(char);

static inline void veil_vec_fini_s(size_t stride, struct veil_vec *vec) {
    free(vec->data - vec->shift * stride);
    *vec = (struct veil_vec){};
}

static inline void veil_vec_shift_s(size_t stride, struct veil_vec *vec, size_t start, int offset) {
    if (start < vec->size)
        memmove(vec->data + (start + offset) * stride, vec->data + start * stride, (vec->size - start) * stride);
    vec->size += offset;
}

static inline int veil_vec_reserve_s(size_t stride, struct veil_vec *vec, size_t elems) {
    if (vec->size + elems <= vec->cap)
        return veil_ok;
    if (vec->size + elems <= vec->cap + vec->shift) {
        vec->data -= vec->shift * stride;
        vec->size += vec->shift;
        vec->cap  += vec->shift;
        veil_vec_shift_s(stride, vec, vec->shift, -(int)vec->shift);
        vec->shift = 0;
        return veil_ok;
    }
    size_t ncap = vec->cap + (elems > vec->cap ? elems : vec->cap);
    void *r = realloc(vec->data - vec->shift * stride, stride * ncap);
    if (r == NULL)
        return veil_error(memory, "%zu x %zu bytes", ncap, stride);
    *vec = (struct veil_vec){.data = r, .size = vec->size, .cap = ncap};
    return veil_ok;
}

static inline int veil_vec_splice_s(size_t stride, struct veil_vec *vec, size_t i, const void *elems, size_t n) {
    if (veil_vec_reserve_s(stride, vec, n))
        return veil_error_up();
    veil_vec_shift_s(stride, vec, i, n);
    memcpy(vec->data + i * stride, elems, n * stride);
    return veil_ok;
}

static inline void veil_vec_erase_s(size_t stride, struct veil_vec *vec, size_t i, size_t n) {
    if (i + n == vec->size)
        vec->size -= n;
    else if (i == 0) {
        vec->data  += n * stride;
        vec->size  -= n;
        vec->cap   -= n;
        vec->shift += n;
    } else
        veil_vec_shift_s(stride, vec, i + n, -(int)n);
}

#define veil_vec_strided(vec)              (size_t)sizeof(*(vec)->data), (struct veil_vec*)(vec)
#define veil_vec_fini(vec)                 veil_vec_fini_s(veil_vec_strided(vec))
#define veil_vec_shift(vec, start, offset) veil_vec_shift_s(veil_vec_strided(vec), start, offset)
#define veil_vec_reserve(vec, elems)       veil_vec_reserve_s(veil_vec_strided(vec), elems)
#define veil_vec_splice(vec, i, elems, n)  veil_vec_splice_s(veil_vec_strided(vec), i, elems, n)
#define veil_vec_extend(vec, elems, n)     veil_vec_splice(vec, (vec)->size, elems, n)
#define veil_vec_insert(vec, i, elem)      veil_vec_splice(vec, i, elem, 1)
#define veil_vec_append(vec, elem)         veil_vec_extend(vec, elem, 1)
#define veil_vec_erase(vec, i, n)          veil_vec_erase_s(veil_vec_strided(vec), i, n)
