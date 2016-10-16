#include "../romp.h"
#include <inttypes.h>

#define CHECK_FIELD(src, dst, f, fmt) \
    do if (src.f != dst.f) return mun_error(assert, #f ": expected " fmt ", got " fmt, src.f, dst.f); while (0)

static int test_romp_primitive() {
    struct T { int16_t a; uint64_t b; double c; };
    struct T x = {-12345L, 9876543210123456789ULL, 5123456.2435463};
    struct T y = {};
    struct romp out = mun_vec_init_static(uint8_t, 64);
    if (romp_encode(&out, "i2 u8 f", &x) MUN_RETHROW)
        return -1;
    if (out.size != 18)
        return mun_error(assert, "2 + 8 + 8 = 18, not %u", out.size);
    if (romp_decode(&out, "i2 u8 f", &y) MUN_RETHROW)
        return -1;
    CHECK_FIELD(x, y, a, "%d");
    CHECK_FIELD(x, y, b, "%" PRIu64);
    CHECK_FIELD(x, y, c, "%f");
    return 0;
}

static int test_romp_struct() {
    struct T
    {
        uint8_t  a;
        uint16_t b;
        uint64_t c;
        uint8_t  d;
        struct {
            uint16_t e;
            uint16_t f;
        };
    };
    struct T x = {1, 2, 3, 4, {5, 6}};
    struct T y = {};
    struct romp out = mun_vec_init_static(uint8_t, 64);
    if (romp_encode(&out, "(u1 u2 u8 u1 (u2 u2))", &x) MUN_RETHROW)
        return -1;
    if (romp_decode(&out, "(u1 u2 u8 u1 (u2 u2))", &y) MUN_RETHROW)
        return -1;
    CHECK_FIELD(x, y, a, "%u");
    CHECK_FIELD(x, y, b, "%u");
    CHECK_FIELD(x, y, c, "%" PRIu64);
    CHECK_FIELD(x, y, d, "%u");
    CHECK_FIELD(x, y, e, "%u");
    CHECK_FIELD(x, y, f, "%u");
    return 0;
}

static int test_romp_vec() {
    struct mun_vec(char) x = mun_vec_init_str("text");
    struct mun_vec(char) y = mun_vec_init_static(char, 16);
    struct romp out = mun_vec_init_static(uint8_t, 16);
    if (romp_encode(&out, "vi1", &x) || romp_decode(&out, "vi1", &y) MUN_RETHROW)
        return -1;
    if (x.size != y.size || memcmp(x.data, y.data, y.size))
        return mun_error(assert, "vectors differ");
    return 0;
}

static int test_romp_vec_struct() {
    struct T { uint8_t a; uint32_t b; };
    struct T xs[] = {{1, 2}, {3, 4}, {5, 6}};
    struct mun_vec(struct T) x = mun_vec_init_array(xs);
    struct mun_vec(struct T) y = mun_vec_init_static(struct T, 4);
    struct romp out = mun_vec_init_static(uint8_t, 32);
    if (romp_encode(&out, "v(u1 u4)", &x) MUN_RETHROW)
        return -1;
    if (romp_decode(&out, "v(u1 u4)", &y) MUN_RETHROW)
        return -1;
    CHECK_FIELD(x, y, size, "%u");
    for (unsigned i = 0; i < y.size; i++) {
        CHECK_FIELD(x.data[i], y.data[i], a, "%u");
        CHECK_FIELD(x.data[i], y.data[i], b, "%u");
    }
    return 0;
}

static int test_romp_vec_vec() {
    char arena[32];
    struct charvec mun_vec(char);
    struct charvec xs[] = {mun_vec_init_str("abc"), mun_vec_init_str("defgh")};
    struct mun_vec(struct charvec) x = mun_vec_init_array(xs);
    struct mun_vec(struct charvec) y = mun_vec_init_static(struct charvec, sizeof(arena) / 8);
    for (unsigned i = 0; i < y.size; i++)
        y.data[i] = (struct charvec) mun_vec_init_borrow(arena + 8 * i, 8);

    struct romp out = mun_vec_init_static(uint8_t, 32);
    if (romp_encode(&out, "v(vi1)", &x) MUN_RETHROW)
        return -1;
    if (romp_decode(&out, "v(vi1)", &y) MUN_RETHROW)
        return -1;
    CHECK_FIELD(x, y, size, "%u");
    for (unsigned i = 0; i < y.size; i++)
        if (x.data[i].size != y.data[i].size || memcmp(x.data[i].data, y.data[i].data, y.data[i].size))
            return mun_error(assert, "inner vectors differ");
    return 0;
}

export { "romp:value", &test_romp_primitive }
     , { "romp:struct", &test_romp_struct }
     , { "romp:vec[value]", &test_romp_vec }
     , { "romp:vec[struct]", &test_romp_vec_struct }
     , { "romp:vec[vec[value]]", &test_romp_vec_vec }
