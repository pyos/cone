#include <unistd.h>

static int test_sleep_for(mun_usec *us) {
    return cone_sleep(*us);
}

static int test_concurrent_sleep_for(char *msg, mun_usec us_a, mun_usec us_b, int cancel_b) {
    struct cone *a = cone(&test_sleep_for, &us_a);
    struct cone *b = cone(&test_sleep_for, &us_b);
    if (a == NULL || b == NULL MUN_RETHROW)
        return cone_drop(a), cone_drop(b), -1;
    mun_usec start = mun_usec_monotonic();
    if (cone_join(a) || (cancel_b && cone_cancel(b)) MUN_RETHROW)
        return cone_drop(b), -1;
    if (cone_join(b) MUN_RETHROW)
        if (!cancel_b || mun_last_error()->code != mun_errno_cancelled)
            return -1;
    mun_usec end = mun_usec_monotonic();
    sprintf(msg, "%fs", (end - start) / 1000000.0);
    return 0;
}

static int test_concurrent_sleep(char *msg) {
    return test_concurrent_sleep_for(msg, 500000, 1000000, 0);
}

static int test_cancelled_sleep(char *msg) {
    return test_concurrent_sleep_for(msg, 100000, 1000000, 1);
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

static int test_nop(void *unused) {
    (void)unused;
    return 0;
}

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

export { "cone:sleep (0.5s concurrent with 1s)", &test_concurrent_sleep }
     , { "cone:sleep (0.1s concurrent with 1s cancelled after 0.1s)", &test_cancelled_sleep }
     , { "cone:yield", &test_yield }
     , { "cone:spawn", &test_spawn }
     , { "cone:reader + writer", &test_rdwr }
