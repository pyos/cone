#pragma once
/*
 * cot / common toolbox
 *       --     -
 */
#include <time.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* --- 128-bit unsigned arithmetic --- */

typedef struct { uint64_t H, L; } cot_u128;

#define COT_U128_MAX ((cot_u128){UINT64_MAX, UINT64_MAX})

static inline cot_u128 cot_u128_add(cot_u128 a, cot_u128 b) {
    return (cot_u128){a.H + b.H + (a.L + b.L < a.L), a.L + b.L};
}

static inline cot_u128 cot_u128_sub(cot_u128 a, cot_u128 b) {
    return (cot_u128){a.H - b.H - (a.L < b.L), a.L - b.L};
}

static inline cot_u128 cot_u128_mul(cot_u128 a, uint32_t b) {
    return (cot_u128){a.H * b + (((a.L >> 32) * b) >> 32), a.L * b};
}

static inline cot_u128 cot_u128_div(cot_u128 a, uint32_t b) {
    uint64_t r = (a.H % b) << 32 | a.L >> 32;
    return (cot_u128){a.H / b, (r / b) << 32 | (((r % b) << 32 | (a.L & UINT32_MAX)) / b)};
}

static inline double cot_u128_to_double(cot_u128 x) {
    return (double)x.H * (1ull << 63) * 2 + x.L;
}

static inline int cot_u128_eq(cot_u128 a, cot_u128 b) {
    return a.H == b.H && a.L == b.L;
}

static inline int cot_u128_lt(cot_u128 a, cot_u128 b) {
    return a.H < b.H || (a.H == b.H && a.L < b.L);
}

static inline int cot_u128_gt(cot_u128 a, cot_u128 b) {
    return a.H > b.H || (a.H == b.H && a.L > b.L);
}

/* --- 128-bit timestamps --- */

typedef cot_u128 cot_nsec;

static inline cot_nsec cot_nsec_from_timespec(struct timespec val) {
    return cot_u128_add(cot_u128_mul((cot_u128){0, val.tv_sec}, 1000000000ull), (cot_u128){0, val.tv_nsec});
}

static inline cot_nsec cot_nsec_now() {
    struct timespec val;
    return clock_gettime(CLOCK_REALTIME, &val) ? COT_U128_MAX : cot_nsec_from_timespec(val);
}

static inline cot_nsec cot_nsec_monotonic() {
    struct timespec val;
    return clock_gettime(CLOCK_MONOTONIC, &val) ? COT_U128_MAX : cot_nsec_from_timespec(val);
}

/* --- error handling --- */

enum cot_errno {
    cot_ok           = 0,
    cot_errno_os     = 1,
    cot_errno_assert = 2,
    cot_errno_memory = 3,
};

struct cot_error_traceback {
    const char *file;
    unsigned line;
};

struct cot_error {
    enum cot_errno code;
    const char *name;
    char text[128];
    struct cot_error_traceback trace[32];
};

const struct cot_error *cot_last_error(void);

int cot_error(enum cot_errno code, const char *file, unsigned line,
              const char *name, const char *fmt, ...) __attribute__((format(printf, 5, 6)));

int cot_error_up(const char *file, int line);

void cot_error_show(const char *prefix);

#define cot_error(id, ...)    cot_error(cot_errno_##id, __FILE__, __LINE__, #id, __VA_ARGS__)
#define cot_error_up()        cot_error_up(__FILE__, __LINE__)
#define cot_error_os()        cot_error(os, "errno %d", errno)

/* --- generic vector type --- */

#define cot_vec(T) { T* data; unsigned size, cap, shift; }

struct cot_vec cot_vec(char);

static inline void cot_vec_fini_s(size_t stride, struct cot_vec *vec) {
    free(vec->data - vec->shift * stride);
    *vec = (struct cot_vec){};
}

static inline void cot_vec_shift_s(size_t stride, struct cot_vec *vec, size_t start, int offset) {
    if (start < vec->size)
        memmove(vec->data + (start + offset) * stride, vec->data + start * stride, (vec->size - start) * stride);
    vec->size += offset;
}

static inline int cot_vec_reserve_s(size_t stride, struct cot_vec *vec, size_t elems) {
    if (vec->size + elems <= vec->cap)
        return cot_ok;
    if (vec->size + elems <= vec->cap + vec->shift) {
        vec->data -= vec->shift * stride;
        vec->size += vec->shift;
        vec->cap  += vec->shift;
        cot_vec_shift_s(stride, vec, vec->shift, -(int)vec->shift);
        vec->shift = 0;
        return cot_ok;
    }
    size_t ncap = vec->cap + (elems > vec->cap ? elems : vec->cap);
    void *r = realloc(vec->data - vec->shift * stride, stride * ncap);
    if (r == NULL)
        return cot_error(memory, "%zu x %zu bytes", ncap, stride);
    *vec = (struct cot_vec){.data = r, .size = vec->size, .cap = ncap};
    return cot_ok;
}

static inline int cot_vec_splice_s(size_t stride, struct cot_vec *vec, size_t i, const void *elems, size_t n) {
    if (cot_vec_reserve_s(stride, vec, n))
        return cot_error_up();
    cot_vec_shift_s(stride, vec, i, n);
    memcpy(vec->data + i * stride, elems, stride);
    return cot_ok;
}

static inline void cot_vec_erase_s(size_t stride, struct cot_vec *vec, size_t i, size_t n) {
    if (i + n == vec->size)
        vec->size -= n;
    else if (i == 0) {
        vec->data  += n * stride;
        vec->size  -= n;
        vec->cap   -= n;
        vec->shift += n;
    } else
        cot_vec_shift_s(stride, vec, i + n, -(int)n);
}

#define cot_vec_strided(vec)              (size_t)sizeof(*(vec)->data), (struct cot_vec*)(vec)
#define cot_vec_fini(vec)                 cot_vec_fini_s(cot_vec_strided(vec))
#define cot_vec_shift(vec, start, offset) cot_vec_shift_s(cot_vec_strided(vec), start, offset)
#define cot_vec_reserve(vec, elems)       cot_vec_reserve_s(cot_vec_strided(vec), elems)
#define cot_vec_splice(vec, i, elems, n)  cot_vec_splice_s(cot_vec_strided(vec), i, elems, n)
#define cot_vec_extend(vec, elems, n)     cot_vec_splice(vec, (vec)->size, elems, n)
#define cot_vec_insert(vec, i, elem)      cot_vec_splice(vec, i, elem, 1)
#define cot_vec_append(vec, elem)         cot_vec_extend(vec, elem, 1)
#define cot_vec_erase(vec, i, n)          cot_vec_erase_s(cot_vec_strided(vec), i, n)
