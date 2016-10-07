#include <unistd.h>
#include <sys/socket.h>

static int test_inc(uint32_t *n, struct romp_iovec *in, struct romp_iovec *out) {
    int32_t incr = 0;
    if (romp_decode(in, "i4", &incr))
        return mun_error_up();
    if (romp_encode(out, "i4", *n += incr))
        return mun_error_up();
    return mun_ok;
}

static int test_nero_half(int *fd) {
    int32_t counter = 0;
    struct cone *bgrunner;
    struct nero conn = {.fd = *fd};
    struct nero_closure inc = nero_closure("inc", &test_inc, &counter);
    if (nero_add(&conn, &inc, 1) || (bgrunner = cone(&nero_run, &conn)) == NULL)
        return nero_fini(&conn), mun_error_up();
    int32_t result = 0;
    if (nero_call(&conn, "inc", "i4", 5, "i4", &result) || nero_call(&conn, "inc", "i4", 6, "i4", &result))
        return cone_cancel(bgrunner), cone_decref(bgrunner), nero_fini(&conn), mun_error_up();
    if (cone_cancel(bgrunner) || (cone_join(bgrunner) && mun_last_error()->code != mun_errno_cancelled))
        return nero_fini(&conn), mun_error_up();
    sleep(1);
    nero_fini(&conn);
    if (result != 11)
        return mun_error(assert, "recvd %d, not 11 = 0 + 5 + 6", result);
    if (counter != 11)
        return mun_error(assert, "counted %d, not 11 = 0 + 5 + 6", counter);
    return mun_ok;
}

static int test_nero_simple() {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
        return mun_error_os();
    if (cone_unblock(fds[0]) || cone_unblock(fds[1]))
        return close(fds[0]), close(fds[1]), mun_error_up();
    struct cone *a = cone(&test_nero_half, &fds[0]);
    struct cone *b = cone(&test_nero_half, &fds[1]);
    if (cone_join(a))
        return cone_decref(b), mun_error_up();
    if (cone_join(b))
        return mun_error_up();
    return mun_ok;
}

export { "nero:simple", &test_nero_simple }