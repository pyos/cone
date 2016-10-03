#pragma once
#include <stdint.h>

struct cone_u128
{
    uint64_t H, L;
};

#define CONE_U128(x) ((struct cone_u128){0, (uint64_t)x})
#define CONE_U128_MAX ((struct cone_u128){UINT64_MAX, UINT64_MAX})

static inline struct cone_u128
cone_u128_add(struct cone_u128 a, struct cone_u128 b) {
    return (struct cone_u128){.H = a.H + b.H + (a.L + b.L < a.L), .L = a.L + b.L};
}

static inline struct cone_u128
cone_u128_sub(struct cone_u128 a, struct cone_u128 b) {
    return (struct cone_u128){.H = a.H - b.H - (a.L < b.L), .L = a.L - b.L};
}

static inline struct cone_u128
cone_u128_mul(struct cone_u128 a, uint32_t b) {
    return (struct cone_u128){.H = a.H * b + (((a.L >> 32) * b) >> 32), .L = a.L * b};
}

static inline struct cone_u128
cone_u128_div(struct cone_u128 a, uint32_t b) {
    uint64_t r = (a.H % b) << 32 | a.L >> 32;
    return (struct cone_u128){.H = a.H / b, .L = (r / b) << 32 | (((r % b) << 32 | (a.L & UINT32_MAX)) / b)};
}

static inline double
cone_u128_to_double(struct cone_u128 x) {
    return (double)x.H * (1ull << 63) * 2 + x.L;
}

static inline int
cone_u128_eq(struct cone_u128 a, struct cone_u128 b) {
    return a.H == b.H && a.L == b.L;
}

static inline int
cone_u128_lt(struct cone_u128 a, struct cone_u128 b) {
    return a.H < b.H || (a.H == b.H && a.L < b.L);
}

static inline int
cone_u128_gt(struct cone_u128 a, struct cone_u128 b) {
    return a.H > b.H || (a.H == b.H && a.L > b.L);
}
