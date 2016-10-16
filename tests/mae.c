#include "../mae.h"
#include <unistd.h>
#include <sys/socket.h>

static int test_counter_add(struct mae *rpc, int32_t *state, int32_t *incr, int32_t *ret) {
    (void)rpc;
    *ret = (*state += *incr);
    return 0;
}

static int test_mae_server(struct mae *conn) {
    int32_t counter = 0;
    struct mae_closure methods[] = {
        mae_closure("add", &test_counter_add, "i4", "i4", &counter),
    };
    int ret = mae_add(conn, methods, 1) || mae_run(conn) MUN_RETHROW;
    return mae_fini(conn), ret;
}

static int test_mae_ok_call(struct mae *conn) {
    int32_t result = 0;
    return mae_call(conn, "add", "i4", &(int32_t){1}, "i4", &result) MUN_RETHROW;
}

static int test_mae_bad_call(struct mae *conn) {
    int32_t result = 0;
    if (!mae_call(conn, "add", "", NULL, "i4", &result) MUN_RETHROW)
        return mun_error(assert, "mae_call should have failed");
    if (mun_last_error()->code != mun_errno_siy)
        return -1;
    return 0;
}

static int test_mae_nonexistent_call(struct mae *conn) {
    if (!mae_call(conn, "something", "", NULL, "", NULL) MUN_RETHROW)
        return mun_error(assert, "mae_call should have failed");
    if (mun_last_error()->code != mun_errno_mae_not_exported)
        return -1;
    return 0;
}

static int test_mae_run(int (*af)(struct mae *), int (*bf)(struct mae *)) {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) MUN_RETHROW_OS)
        return -1;
    if (cone_unblock(fds[0]) || cone_unblock(fds[1]) MUN_RETHROW)
        return close(fds[0]), close(fds[1]), 0;
    struct mae an = {.fd = fds[0]};
    struct mae bn = {.fd = fds[1]};
    struct cone *a = cone(af, &an);
    struct cone *b = cone(bf, &bn);
    if (cone_join(a) MUN_RETHROW)
        return cone_decref(b), mae_fini(&an), mae_fini(&bn), -1;
    if (cone_join(b) MUN_RETHROW)
        return mae_fini(&an), mae_fini(&bn), -1;
    return mae_fini(&an), mae_fini(&bn), 0;
}

static int test_mae_client_impl(struct mae *n) {
    struct cone *u = cone(&test_mae_server, n);
    if (test_mae_ok_call(n) MUN_RETHROW)
        return cone_cancel(u), cone_decref(u), -1;
    cone_yield();
    return cone_cancel(u), cone_join(u);
}

static int test_mae_server_client() {
    return test_mae_run(&test_mae_server, &test_mae_client_impl);
}

static int test_mae_client_client() {
    return test_mae_run(&test_mae_client_impl, &test_mae_client_impl);
}

static int test_mae_many_clients_impl(struct mae *n) {
    const unsigned N = 2048;
    struct cone *u = cone(&test_mae_server, n);
    struct cone *cs[N];
    int err = 0;
    for (unsigned i = 0; i < N; i++)
        cs[i] = cone(&test_mae_ok_call, n);
    for (unsigned i = 0; i < N; i++)
        err |= cone_join(cs[i]);
    return cone_cancel(u), err | cone_join(u);
}

static int test_mae_many_clients() {
    return test_mae_run(&test_mae_many_clients_impl, &test_mae_many_clients_impl);
}

static int test_mae_invalid_client(struct mae *n) {
    struct cone *u = cone(&test_mae_server, n);
    if (test_mae_bad_call(n) MUN_RETHROW)
        return cone_cancel(u), cone_decref(u), -1;
    return cone_cancel(u), cone_join(u);
}

static int test_mae_invalid_args() {
    return test_mae_run(&test_mae_server, &test_mae_invalid_client);
}

static int test_mae_confused_client(struct mae *n) {
    struct cone *u = cone(&test_mae_server, n);
    if (test_mae_nonexistent_call(n) MUN_RETHROW)
        return cone_cancel(u), cone_decref(u), -1;
    return cone_cancel(u), cone_join(u);
}

static int test_mae_invalid_function() {
    return test_mae_run(&test_mae_server, &test_mae_confused_client);
}

export { "mae:server + client", &test_mae_server_client }
     , { "mae:client + client", &test_mae_client_client }
     , { "mae:client overload", &test_mae_many_clients }
     , { "mae:server + bad arguments", &test_mae_invalid_args }
     , { "mae:server + bad function", &test_mae_invalid_function }