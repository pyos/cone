#include <inttypes.h>

static int test_mun_usec(char *msg) {
    mun_usec real = mun_usec_now();
    if (real == MUN_USEC_MAX)
        return mun_error(assert, "real-time clock is broken");
    sprintf(msg, "current time is %" PRIu64, real);
    mun_usec m1 = mun_usec_monotonic();
    mun_usec m2 = mun_usec_monotonic();
    if (m1 == MUN_USEC_MAX || m2 == MUN_USEC_MAX)
        return mun_error(assert, "monotonic clock is broken");
    if (m1 > m2)
        return mun_error(assert, "monotonic clock isn't monotonic");
    return 0;
}

static int test_mun_error(char *msg) {
    sprintf(msg, "if mun_error was broken, the test would be unable to report it anyway");
    return 0;
}

static int test_mun_vec(char *msg) {
    sprintf(msg, "this test is in a cone loop, so assuming mun_vec works");
    return 0;
}

static int test_mun_set() {
    struct mun_set(int) s = {};
    for (int i = 0; i < 512; i++)
        if (mun_set_insert(&s, &i) == NULL MUN_RETHROW)
            return -1;
    for (int i = 24; i < 48; i++)
        if (mun_set_erase(&s, &i))
            return mun_error(assert, "erase %d failed", i);
    for (int i = 128; i < 1024; i++)
        if (mun_set_insert(&s, &i) == NULL MUN_RETHROW)
            return -1;
    for (int i = 0; i < 1024; i++) {
        const int *v = mun_set_find(&s, &i);
        if (24 <= i && i < 48 ? v != NULL : (v == NULL || *v != i))
            return mun_error(assert, "invalid result of find(%d): %p (%d)", i, v, v ? *v : -1);
    }
    mun_set_fini(&s);
    return 0;
}

static int test_mun_map() {
    struct mun_map(int, int) m = {};
    for (int i = 0; i < 512; i++)
        if (mun_map_insert(&m, &((struct mun_pair(int, int)){i, i|4096})) == NULL MUN_RETHROW)
            return -1;
    for (int i = 24; i < 48; i++)
        if (mun_map_erase(&m, &i))
            return mun_error(assert, "erase %d failed", i);
    for (int i = 128; i < 1024; i++)
        if (mun_map_insert(&m, &((struct mun_pair(int, int)){i, i|8192})) == NULL MUN_RETHROW)
            return -1;
    for (int i = 0; i < 1024; i++) {
        const struct mun_pair(int, int) *v = mun_map_find(&m, &i);
        if (24 <= i && i < 48 ? v != NULL : (v == NULL || v->a != i || v->b != (i < 512 ? i|4096 : i|8192)))
            return mun_error(assert, "invalid result of find(%d): %p (%d->%d)", i, v, v ? v->a : -1, v ? v->b : -1);
    }
    mun_map_fini(&m);
    return 0;
}

export {"mun:usec",  &test_mun_usec}
     , {"mun:error", &test_mun_error}
     , {"mun:vec",   &test_mun_vec}
     , {"mun:set",   &test_mun_set}
     , {"mun:map",   &test_mun_map}
