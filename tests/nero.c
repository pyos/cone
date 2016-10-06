#include <unistd.h>
#include <sys/socket.h>

struct testobj {
    struct nero_object base;
    int32_t n;
};

static int testobj_inc(struct testobj *obj, struct romp_iovec *in, struct romp_iovec *out) {
    int32_t incr = 0;
    if (romp_decode(in, "i4", &incr))
        return mun_error_up();
    if (romp_encode(out, "i4", obj->n += incr))
        return mun_error_up();
    return mun_ok;
}

static int test_nero_server(int *fd) {
    struct nero conn = {.fd = *fd};
    struct testobj x = {.n = 0};
    nero_sub(&x.base, "inc", (nero_point*)&testobj_inc);
    nero_add(&conn, "com.example.nero", &x.base);
    if (nero_init(&conn) || nero_run(&conn))
        return nero_fini(&conn), mun_error_up();
    return nero_fini(&conn), mun_ok;
}

static int test_nero_client(int *fd) {
    struct nero conn = {.fd = *fd};
    if (nero_init(&conn) || nero_run(&conn))
        return nero_fini(&conn), mun_error_up();
    return nero_fini(&conn), mun_ok;
}

static int test_nero_simple() {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds))
        return mun_error_os();
    if (cone_unblock(fds[0]) || cone_unblock(fds[1]))
        return close(fds[0]), close(fds[1]), mun_error_up();
    struct cone *a = cone(&test_nero_server, &fds[0]);
    struct cone *b = cone(&test_nero_client, &fds[1]);
    if (cone_join(a))
        return cone_decref(b), mun_error_up();
    if (cone_join(b))
        return mun_error_up();
    return mun_ok;
}

export { "nero:simple", &test_nero_simple }