#include "../nero.h"
#include <unistd.h>
#include <sys/socket.h>

static int test_counter_add(struct nero *rpc, uint32_t *n, struct romp *in, struct romp *out) {
    (void)rpc;
    int32_t incr = 0;
    if (romp_decode(in, "i4", &incr) MUN_RETHROW)
        return -1;
    *n += incr;
    return romp_encode(out, "i4", n) MUN_RETHROW;
}

static int test_nero_server(struct nero *conn) {
    int32_t counter = 0;
    struct nero_closure inc = nero_closure("add", &test_counter_add, &counter);
    int ret = nero_add(conn, &inc, 1) || nero_run(conn) MUN_RETHROW;
    return nero_fini(conn), ret;
}

static int test_nero_ok_call(struct nero *conn) {
    int32_t result = 0;
    return nero_call(conn, "add", "i4", &(int32_t){1}, "i4", &result) MUN_RETHROW;
}

static int test_nero_bad_call(struct nero *conn) {
    int32_t result = 0;
    if (!nero_call(conn, "add", "", NULL, "i4", &result) MUN_RETHROW)
        return mun_error(assert, "nero_call should have failed");
    if (mun_last_error()->code != mun_errno_romp)
        return -1;
    return 0;
}

static int test_nero_nonexistent_call(struct nero *conn) {
    if (!nero_call(conn, "something", "", NULL, "", NULL) MUN_RETHROW)
        return mun_error(assert, "nero_call should have failed");
    if (mun_last_error()->code != mun_errno_nero_not_exported)
        return -1;
    return 0;
}

static int test_nero_run(int (*af)(struct nero *), int (*bf)(struct nero *)) {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) MUN_RETHROW_OS)
        return -1;
    if (cone_unblock(fds[0]) || cone_unblock(fds[1]) MUN_RETHROW)
        return close(fds[0]), close(fds[1]), 0;
    struct nero an = {.fd = fds[0]};
    struct nero bn = {.fd = fds[1]};
    struct cone *a = cone(af, &an);
    struct cone *b = cone(bf, &bn);
    if (cone_join(a) MUN_RETHROW)
        return cone_decref(b), nero_fini(&an), nero_fini(&bn), -1;
    if (cone_join(b) MUN_RETHROW)
        return nero_fini(&an), nero_fini(&bn), -1;
    return nero_fini(&an), nero_fini(&bn), 0;
}

static int test_nero_client_impl(struct nero *n) {
    struct cone *u = cone(&test_nero_server, n);
    if (test_nero_ok_call(n) MUN_RETHROW)
        return cone_cancel(u), cone_decref(u), -1;
    cone_yield();
    return cone_cancel(u), cone_join(u);
}

static int test_nero_server_client() {
    return test_nero_run(&test_nero_server, &test_nero_client_impl);
}

static int test_nero_client_client() {
    return test_nero_run(&test_nero_client_impl, &test_nero_client_impl);
}

static int test_nero_many_clients_impl(struct nero *n) {
    const unsigned N = 2048;
    struct cone *u = cone(&test_nero_server, n);
    struct cone *cs[N];
    int err = 0;
    for (unsigned i = 0; i < N; i++)
        cs[i] = cone(&test_nero_ok_call, n);
    for (unsigned i = 0; i < N; i++)
        err |= cone_join(cs[i]);
    return cone_cancel(u), err | cone_join(u);
}

static int test_nero_many_clients() {
    return test_nero_run(&test_nero_many_clients_impl, &test_nero_many_clients_impl);
}

static int test_nero_invalid_client(struct nero *n) {
    struct cone *u = cone(&test_nero_server, n);
    if (test_nero_bad_call(n) MUN_RETHROW)
        return cone_cancel(u), cone_decref(u), -1;
    return cone_cancel(u), cone_join(u);
}

static int test_nero_invalid_args() {
    return test_nero_run(&test_nero_server, &test_nero_invalid_client);
}

static int test_nero_confused_client(struct nero *n) {
    struct cone *u = cone(&test_nero_server, n);
    if (test_nero_nonexistent_call(n) MUN_RETHROW)
        return cone_cancel(u), cone_decref(u), -1;
    return cone_cancel(u), cone_join(u);
}

static int test_nero_invalid_function() {
    return test_nero_run(&test_nero_server, &test_nero_confused_client);
}

export { "nero:server + client", &test_nero_server_client }
     , { "nero:client + client", &test_nero_client_client }
     , { "nero:client overload", &test_nero_many_clients }
     , { "nero:server + bad arguments", &test_nero_invalid_args }
     , { "nero:server + bad function", &test_nero_invalid_function }