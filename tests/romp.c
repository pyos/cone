#include <inttypes.h>

struct mun_vec_vec mun_vec(struct mun_vec);

static inline void mun_vec_vec_fini(struct mun_vec_vec *v) {
    for (size_t i = 0; i < v->size; i++)
        mun_vec_fini(&v->data[i]);
    mun_vec_fini(v);
}

static int test_romp_primitive() {
    struct romp_iovec out = {};
    int8_t  expect_i1 = -8;             uint8_t  expect_u1 = -expect_i1;
    int16_t expect_i2 = -(16    << 8);  uint16_t expect_u2 = -expect_i2;
    int32_t expect_i4 = -(32l   << 16); uint32_t expect_u4 = -expect_i4;
    int64_t expect_i8 = -(64ull << 32); uint64_t expect_u8 = -expect_i8;
    double  expect_f = 123.456789123456789e8;
    if (romp_encode(&out, "u1 u2 u4 u8 i1 i2 i4 i8 f", expect_u1, expect_u2, expect_u4, expect_u8,
                    expect_i1, expect_i2, expect_i4, expect_i8, expect_f))
        return mun_vec_fini(&out), mun_error_up();

    struct romp_iovec in = out;
    int8_t  i1 = 0; uint8_t  u1 = 0;
    int16_t i2 = 0; uint16_t u2 = 0;
    int32_t i4 = 0; uint32_t u4 = 0;
    int64_t i8 = 0; uint64_t u8 = 0;
    double f = 0;
    if (romp_decode(&in, "u1 u2 u4 u8 i1 i2 i4 i8 f", &u1, &u2, &u4, &u8, &i1, &i2, &i4, &i8, &f))
        return mun_vec_fini(&out), mun_error_up();
    mun_vec_fini(&out);
#define CHECK(var, fmt) if (var != expect_##var) return mun_error(assert, #var ": %" fmt " != %" fmt, var, expect_##var);
    CHECK(u1, PRIu8);  CHECK(i1, PRIi8);
    CHECK(u2, PRIu16); CHECK(i2, PRIi16);
    CHECK(u4, PRIu32); CHECK(i4, PRIi32);
    CHECK(u8, PRIu64); CHECK(i8, PRIi64);
    CHECK(f, "f");
#undef CHECK
    return mun_ok;
}

static int test_romp_vec() {
    char raw_vi1[] = "some string";
    uint32_t raw_vu4[] = {1, 2, 3, 4, 5};

    struct vi1t mun_vec(char);
    struct vu4t mun_vec(uint32_t);

    struct romp_iovec out = {};
    struct vi1t expect_vi1 = {.data = raw_vi1, .size = sizeof(raw_vi1)};
    struct vu4t expect_vu4 = {.data = raw_vu4, .size = sizeof(raw_vu4) / sizeof(*raw_vu4)};
    if (romp_encode(&out, "vi1 vu4", &expect_vi1, &expect_vu4))
        return mun_vec_fini(&out), mun_error_up();

    struct romp_iovec in = out;
    struct vi1t vi1 = {};
    struct vu4t vu4 = {};
    if (romp_decode(&in, "vi1 vu4", &vi1, &vu4))
        goto error;
#define CHECK(var) \
    if (var.size != expect_##var.size || memcmp(var.data, expect_##var.data, var.size * sizeof(*var.data))) { \
        mun_error(assert, #var " was corrupted during coding"); \
        goto error; \
    }
    CHECK(vi1);
    CHECK(vu4);
#undef CHECK
    return mun_vec_fini(&out), mun_vec_fini(&vi1), mun_vec_fini(&vu4), mun_ok;
error:
    return mun_vec_fini(&out), mun_vec_fini(&vi1), mun_vec_fini(&vu4), mun_error_up();
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