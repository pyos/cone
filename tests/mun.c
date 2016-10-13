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
    for (int i = 1; i < 512; i++)
        if (mun_set_insert(&s, &i) == NULL MUN_RETHROW)
            return -1;
    for (int i = 24; i < 48; i++)
        if (mun_set_erase(&s, &i))
            return mun_error(assert, "erase %d failed", i);
    for (int i = 128; i < 1024; i++)
        if (mun_set_insert(&s, &i) == NULL MUN_RETHROW)
            return -1;
    for (int i = 1; i < 1024; i++) {
        const int *v = mun_set_find(&s, &i);
        if (24 <= i && i < 48 ? v != NULL : (v == NULL || *v != i))
            return mun_error(assert, "invalid result of find(%d): %p (%d)", i, v, v ? *v : -1);
    }
    return 0;
}

export {"mun:usec",  &test_mun_usec}
     , {"mun:error", &test_mun_error}
     , {"mun:vec",   &test_mun_vec}
     , {"mun:set",   &test_mun_set}
