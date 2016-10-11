#include "../romp.h"
#include <inttypes.h>

static int test_romp_primitive() {
    int16_t i2 = -12345L, i2dec = 0;
    uint64_t u8 = 9876543210123456789ULL, u8dec = 0;
    double f = 5123456.2435463, fdec = 0;

    struct romp out = mun_vec_init_static(char, 64);
    if (romp_encode(&out, "i2 u8 f", i2, u8, f) MUN_RETHROW)
        return -1;
    if (out.size != 18)
        return mun_error(assert, "2 + 8 + 8 = 18, not %u", out.size);
    struct romp in = out;
    if (romp_decode(&in, "i2 u8 f", &i2dec, &u8dec, &fdec) MUN_RETHROW)
        return -1;
    if (i2dec != i2 || u8dec != u8 || fdec != f)
        return mun_error(assert, "primitives were corrupted during coding");
    return 0;
}

static int test_romp_vec() {
    struct mun_vec(char) original = mun_vec_init_str("text");
    struct mun_vec(char) decoded = mun_vec_init_static(char, 16);

    struct romp out = mun_vec_init_static(char, 16);
    if (romp_encode(&out, "vi1", &original) MUN_RETHROW)
        return -1;
    struct romp in = out;
    if (romp_decode(&in, "vi1", &decoded) MUN_RETHROW)
        return -1;
    if (!mun_vec_eq(&original, &decoded))
        return mun_error(assert, "vector was corrupted during coding");
    return 0;
}

static int test_romp_vec_vec() {
    char arena[32];
    struct charvec mun_vec(char);
    struct charvec raw[] = {mun_vec_init_str("abc"), mun_vec_init_str("defgh")};
    struct mun_vec(struct charvec) original = mun_vec_init_array(raw);
    struct mun_vec(struct charvec) decoded = mun_vec_init_static(struct mun_vec, sizeof(arena) / 8);
    for (unsigned i = 0; i < decoded.size; i++)
        decoded.data[i] = (struct charvec)mun_vec_init_borrow(arena + 8 * i, 8);

    struct romp out = mun_vec_init_static(char, 32);
    if (romp_encode(&out, "vvi1", &original) MUN_RETHROW)
        return -1;
    struct romp in = out;
    if (romp_decode(&in, "vvi1", &decoded) MUN_RETHROW)
        return -1;
    if (decoded.size != original.size)
        return mun_error(assert, "sizes of outer vectors differ");
    for (unsigned i = 0; i < decoded.size; i++)
        if (!mun_vec_eq(&original.data[i], &decoded.data[i]))
            return mun_error(assert, "inner vectors differ");
    return 0;
}

export { "romp:primitive", &test_romp_primitive }
     , { "romp:vec[primitive]", &test_romp_vec }
     , { "romp:vec[vec[primitive]]", &test_romp_vec_vec }
