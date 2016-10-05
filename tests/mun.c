static __uint128_t rand128(unsigned *seed) {
    __uint128_t r = 0;
    for (unsigned i = 0; i < 128; i += __builtin_ffs(RAND_MAX))
        r |= (__uint128_t)rand_r(seed) << i;
    return r;
}

#define u128_fuzz(N, op, f) {                          \
    unsigned seed = mun_nsec_now().L;                  \
    for (unsigned i = 0; i < (N); i++) {               \
        __uint128_t a = rand128(&seed);                \
        __uint128_t b = rand128(&seed) & UINT32_MAX;   \
        __uint128_t c = a op b;                        \
        mun_u128 r = f((mun_u128){a >> 64, a}, b);     \
        if (r.H != c >> 64 || r.L != (c & UINT64_MAX)) \
            return mun_error(assert, "!=");            \
    }                                                  \
    return mun_ok;                                     \
}

static int u128_mul() u128_fuzz(100000, *, mun_u128_mul);
static int u128_div() u128_fuzz(100000, /, mun_u128_div);

export { "u128 * u32", &u128_mul }
     , { "u128 / u32", &u128_div }
