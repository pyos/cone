#include <unistd.h>
#include <sys/socket.h>

static int test_yield(char *msg) {
    const unsigned N = 1000000;
    mun_usec a = mun_usec_monotonic();
    for (unsigned i = 0; i < N; i++)
        cone_yield();
    mun_usec b = mun_usec_monotonic();
    sprintf(msg, "%f us/yield", (double)(b - a) / N);
    return 0;
}

static int test_nop(void *unused) {
    (void)unused;
    return 0;
}

static int test_spawn(char *msg) {
    const unsigned N = 1000000;
    mun_usec a = mun_usec_monotonic();
    struct cone *c;
    for (unsigned i = 0; i < N; i++)
        if ((c = cone(&test_nop, NULL)) == NULL || cone_join(c, 0) MUN_RETHROW)
            return -1;
    mun_usec b = mun_usec_monotonic();
    sprintf(msg, "%f us/cone", (double)(b - a) / N);
    return 0;
}

static int test_spawn_many(char *msg) {
    const unsigned N = 1000000;
    struct mun_vec(struct cone *) spawned = {};
    if (mun_vec_reserve(&spawned, N) MUN_RETHROW)
        return -1;
    mun_usec a = mun_usec_monotonic();
    struct cone *c;
    for (unsigned i = 0; i < N; i++) {
        if ((c = cone(&test_nop, NULL)) == NULL MUN_RETHROW)
            goto fail;
        if (mun_vec_append(&spawned, &c) MUN_RETHROW) {
            cone_drop(c);
            goto fail;
        }
    }
    mun_usec b = mun_usec_monotonic();
    for mun_vec_iter(&spawned, it)
        cone_drop(*it);
    sprintf(msg, "%f us/cone", (double)(b - a) / N);
    return 0;
fail:
    for mun_vec_iter(&spawned, it)
        cone_drop(*it);
    return -1;
}

export { "perf:yield", &test_yield }
     , { "perf:spawn", &test_spawn }
     , { "perf:spawn many", &test_spawn_many }
