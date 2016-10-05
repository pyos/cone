#include <inttypes.h>

struct mun_vec_vec mun_vec(struct mun_vec);

static inline void mun_vec_vec_fini(struct mun_vec_vec *v) {
    for (size_t i = 0; i < v->size; i++)
        mun_vec_fini(&v->data[i]);
    mun_vec_fini(v);
}

static int test_romp_primitive() {
    int16_t i2 = -12345L, i2dec = 0;
    uint64_t u8 = 9876543210123456789ULL, u8dec = 0;
    double f = 5123456.2435463, fdec = 0;

    struct romp_iovec out = mun_vec_static_initializer(64);
    if (romp_encode(&out, "i2 u8 f", i2, u8, f))
        return mun_error_up();
    if (out.size != 18)
        return mun_error(assert, "2 + 8 + 8 = 18, not %u", out.size);
    struct romp_iovec in = out;
    if (romp_decode(&in, "i2 u8 f", &i2dec, &u8dec, &fdec))
        return mun_error_up();
    if (i2dec != i2 || u8dec != u8 || fdec != f)
        return mun_error(assert, "primitives were corrupted during coding");
    return mun_ok;
}

static int test_romp_vec() {
    struct mun_vec(char) original = {.data = "test", .size = 4};
    struct mun_vec(char) decoded = mun_vec_static_initializer(16);

    struct romp_iovec out = mun_vec_static_initializer(16);
    if (romp_encode(&out, "vi1", &original))
        return mun_error_up();
    struct romp_iovec in = out;
    if (romp_decode(&in, "vi1", &decoded))
        return mun_error_up();
    if (!mun_vec_eq(&original, &decoded))
        return mun_error(assert, "vector was corrupted during coding");
    return mun_ok;
}

export { "romp:primitive", &test_romp_primitive }
     , { "romp:vec[primitive]", &test_romp_vec }
//   , { "romp:vec[vec[primitive]]", &test_romp_vec_vec }

/*
int enctest(struct romp_iovec *out) {
    struct mun_vec str = {.data = "Hello, World!\n", .size = 14};
    struct mun_vec s[] = {{.data = "asd", .size = 3}, {.data = "qwe", .size = 3}, {.data = "zxc", .size = 3}};
    struct vecvec stn = {.data = s, .size = 3};

    if (romp_encode(out, "u1 u2 u4 u8 f vi1 i1 i2 i4 i8 vvi1", u8, u16, u32, u64, dbl, &str, i8, i16, i32, i64, &stn))
        return mun_error_up();
    return mun_ok;
}

int dectest(struct romp_iovec *in) {
    struct mun_vec(char) str = {};
    struct vecvec stn = {};
    if (romp_decode(in, "u1 u2 u4 u8 f vi1 i1 i2 i4 i8 vvi1", &u8, &u16, &u32, &u64, &dbl, &str, &i8, &i16, &i32, &i64, &stn))
        return mun_vec_fini(&str), vecvec_fini(&stn), mun_error_up();
    printf("str = %.*s\n", (int)str.size, str.data);
    printf("vec = [%u]\n", stn.size);
    for (unsigned i = 0; i < stn.size; i++)
        printf("      [%u] = %.*s\n", i, (int)stn.data[i].size, stn.data[i].data);
    return mun_vec_fini(&str), vecvec_fini(&stn), mun_ok;
}
*/