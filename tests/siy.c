#include "../siy.h"
#include <inttypes.h>

#define CHECK_FIELD(src, dst, f, fmt) \
    do if (src.f != dst.f) return mun_error(assert, #f ": expected " fmt ", got " fmt, src.f, dst.f); while (0)

static char *test_dump(const struct siy *m, char *msg) {
    for (unsigned i = 0; i < m->size; i++)
        msg += sprintf(msg, "%02X ", m->data[i]);
    return msg;
}

static char *test_dump_sign(const struct siy_sign *s, char *msg) {
    msg += sprintf(msg, "0x%X(%u@%u)", s->sign, s->size, s->align);
    if (s->consumes) {
        for (size_t i = 0; i < s->consumes; i += s[i + 1].consumes + 1) {
            *msg++ = i ? ',' : '[';
            msg = test_dump_sign(&s[i + 1], msg);
        }
        *msg++ = ']';
    }
    *msg = 0;
    return msg;
}

static int test_siy_signature(char *msg) {
    struct siy_sign si[8];
    if (siy_signature("u4 (u4 u2) u2", si, sizeof(si) / sizeof(si[0])))
        return -1;
    test_dump_sign(si, msg);
    if (si[0].size != 16)
        return mun_error(assert, "invalid size %u; expected 8", si[0].size);
    if (si[0].align != 4)
        return mun_error(assert, "invalid alignment %u; expected 4", si[0].align);
    return 0;
}

static int test_siy_primitive(char *msg) {
    struct T { int16_t a; uint64_t b; double c; uint8_t d; };
    struct T x = {-12345L, 9876543210123456789ULL, 5123456.2435463, 0xff};
    struct T y = {};
    struct siy out = mun_vec_init_static(uint8_t, 64);
    if (siy_encode(&out, "i2 u8 f u1", &x) MUN_RETHROW)
        return -1;
    test_dump(&out, msg);
    if (siy_decode(&out, "i2 u8 f u1", &y) MUN_RETHROW)
        return -1;
    CHECK_FIELD(x, y, a, "%d");
    CHECK_FIELD(x, y, b, "%" PRIu64);
    CHECK_FIELD(x, y, c, "%f");
    CHECK_FIELD(x, y, d, "%u");
    return 0;
}

static int test_siy_struct(char *msg) {
    struct V {
        uint16_t e;
        uint16_t f;
    };
    struct T
    {
        uint8_t  a;
        struct {
            uint16_t b;
            uint64_t c;
        };
        uint8_t  d;
        struct V *vp;
        struct V v;
    };
    struct V vx = {7, 8}, vy = {};
    struct T x = {1, {2, 3}, 4, &vx, {5, 6}}, y = {.vp = &vy};
    struct siy out = mun_vec_init_static(uint8_t, 64);
    if (siy_encode(&out, "(u1 (u2 u8) u1 *(u2 u2) (u2 u2))", &x) MUN_RETHROW)
        return -1;
    if (msg)
        test_dump(&out, msg);
    if (siy_decode(&out, "(u1 (u2 u8) u1 *(u2 u2) (u2 u2))", &y) MUN_RETHROW)
        return -1;
    if (msg) {
        CHECK_FIELD(x, y, a, "%u");
        CHECK_FIELD(x, y, b, "%u");
        CHECK_FIELD(x, y, c, "%" PRIu64);
        CHECK_FIELD(x, y, d, "%u");
        CHECK_FIELD(x, y, v.e, "%u");
        CHECK_FIELD(x, y, v.f, "%u");
        CHECK_FIELD(x, y, vp->e, "%u");
        CHECK_FIELD(x, y, vp->f, "%u");
    }
    return 0;
}

static int test_siy_struct_timed(char *msg) {
    mun_usec start = mun_usec_monotonic();
    for (unsigned i = 0; i < 1000000; i++)
        if (test_siy_struct(NULL))
            return -1;
    mun_usec end = mun_usec_monotonic();
    sprintf(msg, "%f us/(encode+decode)", (double)(end - start) / 1000);
    return 0;
}

static int test_siy_vec(char *msg) {
    struct mun_vec(char) x = mun_vec_init_str("text");
    struct mun_vec(char) y = mun_vec_init_static(char, 16);
    struct siy out = mun_vec_init_static(uint8_t, 16);
    if (siy_encode(&out, "vi1", &x) MUN_RETHROW)
        return -1;
    test_dump(&out, msg);
    if (siy_decode(&out, "vi1", &y) MUN_RETHROW)
        return -1;
    if (x.size != y.size || memcmp(x.data, y.data, y.size))
        return mun_error(assert, "vectors differ");
    return 0;
}

static int test_siy_vec_struct(char *msg) {
    struct T { uint8_t a; uint32_t b; };
    struct T xs[] = {{1, 2}, {3, 4}, {5, 6}};
    struct mun_vec(struct T) x = mun_vec_init_array(xs);
    struct mun_vec(struct T) y = mun_vec_init_static(struct T, 4);
    struct siy out = mun_vec_init_static(uint8_t, 32);
    if (siy_encode(&out, "v(u1 u4)", &x) MUN_RETHROW)
        return -1;
    test_dump(&out, msg);
    if (siy_decode(&out, "v(u1 u4)", &y) MUN_RETHROW)
        return -1;
    CHECK_FIELD(x, y, size, "%zu");
    for (unsigned i = 0; i < y.size; i++) {
        CHECK_FIELD(x.data[i], y.data[i], a, "%u");
        CHECK_FIELD(x.data[i], y.data[i], b, "%u");
    }
    return 0;
}

static int test_siy_vec_vec(char *msg) {
    char arena[32];
    struct charvec mun_vec(char);
    struct charvec xs[] = {mun_vec_init_str("abc"), mun_vec_init_str("defgh")};
    struct mun_vec(struct charvec) x = mun_vec_init_array(xs);
    struct mun_vec(struct charvec) y = mun_vec_init_static(struct charvec, sizeof(arena) / 8);
    for (unsigned i = 0; i < y.size; i++)
        y.data[i] = (struct charvec) mun_vec_init_borrow(arena + 8 * i, 8);

    struct siy out = mun_vec_init_static(uint8_t, 32);
    if (siy_encode(&out, "v(vi1)", &x) MUN_RETHROW)
        return -1;
    test_dump(&out, msg);
    if (siy_decode(&out, "v(vi1)", &y) MUN_RETHROW)
        return -1;
    CHECK_FIELD(x, y, size, "%zu");
    for (unsigned i = 0; i < y.size; i++)
        if (x.data[i].size != y.data[i].size || memcmp(x.data[i].data, y.data[i].data, y.data[i].size))
            return mun_error(assert, "inner vectors differ");
    return 0;
}

export { "siy:signature", &test_siy_signature }
     , { "siy:value", &test_siy_primitive }
     , { "siy:struct", &test_siy_struct }
     , { "siy:vec[value]", &test_siy_vec }
     , { "siy:vec[struct]", &test_siy_vec_struct }
     , { "siy:vec[vec[value]]", &test_siy_vec_vec }
     , { "siy:struct timed", &test_siy_struct_timed }
