static __uint128_t rand128(unsigned *seed) {
    __uint128_t r = 0;
    for (unsigned i = 0; i < 128; i += 8 * sizeof(int) - 1)
        r |= (__uint128_t)rand_r(seed) << i;
    return r;
}

#define u128_fuzz_generic(N, a_, b_, op, expr) {         \
    unsigned seed = mun_nsec_now().L;                    \
    for (unsigned i = 0; i < (N); i++) {                 \
        a_ a = rand128(&seed);                           \
        b_ b = rand128(&seed);                           \
        __uint128_t c = a op b;                          \
        mun_u128 r = expr;                               \
        if (r.H != (c >> 64) || r.L != (c & UINT64_MAX)) \
            return mun_error(assert, "!=");              \
    }                                                    \
    return mun_ok;                                       \
}

#define u128_fuzz1(N, op, f) u128_fuzz_generic(N, __uint128_t, uint32_t, op, f((mun_u128){a>>64,a}, b))
#define u128_fuzz2(N, op, f) u128_fuzz_generic(N, __uint128_t, __uint128_t, op, f((mun_u128){a>>64,a},(mun_u128){b>>64,b}))

static int u128_add() u128_fuzz2(100000, +, mun_u128_add);
static int u128_sub() u128_fuzz2(100000, -, mun_u128_sub);
static int u128_mul() u128_fuzz1(100000, *, mun_u128_mul);
static int u128_div() u128_fuzz1(100000, /, mun_u128_div);

export { "u128 + u128", &u128_add }
     , { "u128 - u128", &u128_sub }
     , { "u128 * u32", &u128_mul }
     , { "u128 / u32", &u128_div }
