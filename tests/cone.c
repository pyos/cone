#include <unistd.h>

struct args { unsigned N; int fd; const char *data; size_t size; };

static int writer(struct args *aptr) {
    struct args args = *aptr;
    for (ssize_t wr, mod = 0; args.N--; mod = (mod + wr) % args.size)
        if ((wr = write(args.fd, args.data + mod, args.size - mod)) < 0 MUN_RETHROW_OS)
            return -1;
    return 0;
}

static int reader(struct args *aptr) {
    struct args args = *aptr;
    char buf[args.size];
    for (ssize_t rd, mod = 0; args.N--; mod = (mod + rd) % args.size) {
        if ((rd = read(args.fd, buf, args.size - mod)) < 0 MUN_RETHROW_OS)
            return -1;
        if (memcmp(args.data + mod, buf, rd))
            return mun_error(assert, "recvd bad data");
    }
    return 0;
}

static int test_rdwr() {
    int fds[2];
    if (pipe(fds) MUN_RETHROW_OS)
        return -1;
    if (cone_unblock(fds[0]) || cone_unblock(fds[1]) MUN_RETHROW)
        return close(fds[0]), close(fds[1]), -1;
    struct args ar = { 100000, fds[0], "Hello, World! Hello, World! Hello, World! Hello, World!", 55 };
    struct args aw = { 100000, fds[1], "Hello, World! Hello, World! Hello, World! Hello, World!", 55 };
    struct cone *r = cone(reader, &ar);
    struct cone *w = cone(writer, &aw);
    if (cone_join(r) || cone_join(w) MUN_RETHROW)
        return close(fds[0]), close(fds[1]), -1;
    return close(fds[0]), close(fds[1]), 0;
}

static int test_sleep(char *msg) {
    mun_usec a = mun_usec_monotonic();
    if (sleep(1))
        return mun_error(assert, "did not sleep");
    mun_usec b = mun_usec_monotonic();
    if (b - a < 1000000)
        return mun_error(assert, "slept for less than wanted");
    sprintf(msg, "wanted 1s, got %fs", (b - a) / 1000000.0);
    return 0;
}

static int test_concurrent_sleep(char *msg) {
    struct cone *a = cone(&test_sleep, msg);
    struct cone *b = cone(&test_sleep, msg);
    if (a == NULL || b == NULL MUN_RETHROW)
        return cone_decref(a), cone_decref(b), -1;
    mun_usec start = mun_usec_monotonic();
    if (cone_join(a) MUN_RETHROW)
        return cone_decref(b), -1;
    if (cone_join(b) MUN_RETHROW)
        return -1;
    if (mun_usec_monotonic() - start > 1100000)
        return mun_error(assert, "slept too much");
    return 0;
}

static int test_yield(char *msg) {
    const unsigned N = 1000000;
    mun_usec a = mun_usec_monotonic();
    for (unsigned i = 0; i < N; i++)
        cone_yield();
    mun_usec b = mun_usec_monotonic();
    sprintf(msg, "%f us/yield", (double)(b - a) / N);
    return 0;
}

static int test_nop() { return 0; }
static int test_spawn(char *msg) {
    const unsigned N = 1000000;
    mun_usec a = mun_usec_monotonic();
    struct cone *c;
    for (unsigned i = 0; i < N; i++)
        if ((c = cone(&test_nop, NULL)) == NULL || cone_join(c) MUN_RETHROW)
            return -1;
    mun_usec b = mun_usec_monotonic();
    sprintf(msg, "%f us/cone", (double)(b - a) / N);
    return 0;
}

export { "cone:sleep", &test_sleep }
     , { "cone:concurrent sleep", &test_concurrent_sleep }
     , { "cone:yield", &test_yield }
     , { "cone:spawn", &test_spawn }
     , { "cone:reader + writer", &test_rdwr }
