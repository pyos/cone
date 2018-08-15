#include <unistd.h>
#include <sys/socket.h>

static int test_sleep_for(mun_usec *us) {
    return cone_sleep(*us);
}

static int test_concurrent_sleep_for(char *msg, mun_usec us_a, mun_usec us_b, int cancel_b) {
    struct cone *a = cone(&test_sleep_for, &us_a);
    struct cone *b = cone(&test_sleep_for, &us_b);
    if (a == NULL || b == NULL MUN_RETHROW)
        return cone_drop(a), cone_drop(b), -1;
    mun_usec start = mun_usec_monotonic();
    if (cone_join(a, 0) || (cancel_b && cone_cancel(b)) MUN_RETHROW)
        return cone_drop(b), -1;
    if (cone_join(b, 0) MUN_RETHROW)
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
    if (cone_join(r, 0) || cone_join(w, 0) MUN_RETHROW)
        return close(fds[0]), close(fds[1]), -1;
    return close(fds[0]), close(fds[1]), 0;
}

struct io_starvation_state {
    int stop;
    struct cone_event a;
    struct cone_event b;
};

static int test_io_starvation_a(struct io_starvation_state *st) {
    cone_atom a = 0;
    while (!st->stop)
        if (cone_wake(&st->b, -1) || cone_wait(&st->a, &a, 0) MUN_RETHROW)
            return -1;
    return 0;
};

static int test_io_starvation_b(struct io_starvation_state *st) {
    cone_atom a = 0;
    while (!st->stop)
        if (cone_wake(&st->a, -1) || cone_wait(&st->b, &a, 0) MUN_RETHROW)
            return -1;
    return 0;
};

static int test_io_starvation_c(struct io_starvation_state *st) {
    if (cone_yield() MUN_RETHROW)
        return -1;
    st->stop = 1;
    if (cone_wake(&st->a, -1) || cone_wake(&st->b, -1) MUN_RETHROW)
        return -1;
    return 0;
}

static int test_io_starvation() {
    struct io_starvation_state st = {};
    struct cone *a = cone(test_io_starvation_a, &st);
    struct cone *b = cone(test_io_starvation_b, &st);
    struct cone *c = cone(test_io_starvation_c, &st);
    sleep(1);
    return cone_cancel(c) || cone_cancel(a) || cone_cancel(b) || cone_join(c, 0) || cone_join(a, 0) || cone_join(b, 0) MUN_RETHROW;
}

struct concurrent_rw_state {
    int fds[2];
    int result;
};

static int test_concurrent_rw_r(struct concurrent_rw_state *st) {
    if (cone_iowait(st->fds[0], 0) MUN_RETHROW)
        return -1;
    st->result |= 1;
    return 0;
}

static int test_concurrent_rw_w(struct concurrent_rw_state *st) {
    if (cone_iowait(st->fds[0], 1) MUN_RETHROW)
        return -1;
    st->result |= 2;
    return 0;
}

static int test_concurrent_rw() {
    struct concurrent_rw_state st = {};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, st.fds) MUN_RETHROW_OS)
        return -1;
    struct cone *a = cone(test_concurrent_rw_r, &st);
    struct cone *b = cone(test_concurrent_rw_w, &st);
    struct cone *c = cone(test_concurrent_rw_r, &st);
    if (cone_join(b, 0) MUN_RETHROW)
        goto fail;
    if (st.result != 2 && mun_error(assert, "reader also finished, but data hasn't been written yet"))
        goto fail;
    if (write(st.fds[1], "x", 1) < 0 MUN_RETHROW_OS)
        goto fail;
    if (cone_join(a, 0) MUN_RETHROW || (st.result != 3 && mun_error(assert, "wut")))
        goto fail2;
    if (cone_join(c, 0) MUN_RETHROW)
        goto fail3;
    close(st.fds[0]);
    close(st.fds[1]);
    return 0;
fail:
    cone_cancel(a);
    cone_join(a, CONE_NORETHROW);
fail2:
    cone_cancel(c);
    cone_join(c, CONE_NORETHROW);
fail3:
    close(st.fds[0]);
    close(st.fds[1]);
    return -1;
}

export { "cone:sleep (0.5s concurrent with 1s)", &test_concurrent_sleep }
     , { "cone:sleep (0.1s concurrent with 1s cancelled after 0.1s)", &test_cancelled_sleep }
     , { "cone:reader + writer", &test_rdwr }
     , { "cone:reader + writer on one fd", &test_concurrent_rw }
     , { "cone:io starvation", &test_io_starvation }
     , { "cone:yield", &test_yield }
     , { "cone:spawn", &test_spawn }
     , { "cone:spawn many", &test_spawn_many }
