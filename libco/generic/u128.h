#pragma once
#include <stdint.h>

struct co_u128
{
    uint64_t H, L;
};

#define CO_U128(x) ((struct co_u128){.L = x})
#define CO_U128_MAX ((struct co_u128){UINT64_MAX, UINT64_MAX})

static inline struct co_u128
co_u128_add(struct co_u128 a, struct co_u128 b) {
    return (struct co_u128){.H = a.H + b.H + (a.L + b.L < a.L), .L = a.L + b.L};
}

static inline struct co_u128
co_u128_sub(struct co_u128 a, struct co_u128 b) {
    return (struct co_u128){.H = a.H - b.H - (a.L < b.L), .L = a.L - b.L};
}

static inline struct co_u128
co_u128_mul(struct co_u128 a, uint32_t b) {
    return (struct co_u128){.H = a.H * b + (((a.L >> 32) * b) >> 32), .L = a.L * b};
}

static inline struct co_u128
co_u128_div(struct co_u128 a, uint32_t b) {
    uint64_t r = (a.H % b) << 32 | a.L >> 32;
    return (struct co_u128){.H = a.H / b, .L = (r / b) << 32 | (((r % b) << 32 | (a.L & UINT32_MAX)) / b)};
}

static inline double
co_u128_to_double(struct co_u128 x) {
    return (double)x.H * (1ull << 63) * 2 + x.L;
}

static inline int
co_u128_eq(struct co_u128 a, struct co_u128 b) {
    return a.H == b.H && a.L == b.L;
}

static inline int
co_u128_lt(struct co_u128 a, struct co_u128 b) {
    return a.H < b.H || (a.H == b.H && a.L < b.L);
}

static inline int
co_u128_gt(struct co_u128 a, struct co_u128 b) {
    return a.H > b.H || (a.H == b.H && a.L > b.L);
}
