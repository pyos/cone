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
    return mae_add(conn, methods, 1) || mae_run(conn) MUN_RETHROW;
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

static int test_mae_run(int (*af)(struct mae *), int (*bf)(struct mae *), char *msg) {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) MUN_RETHROW_OS)
        return -1;
    if (cone_unblock(fds[0]) || cone_unblock(fds[1]) MUN_RETHROW)
        return close(fds[0]), close(fds[1]), -1;
    struct mae ns[] = {{.fd = fds[0]}, {.fd = fds[1]}};
    struct cone *cs[] = {cone(af, &ns[0]), cone(bf, &ns[1])};
    for (unsigned i = 2; i--;) {
        if (cone_join(cs[i]) MUN_RETHROW) {
            while (i--) {
                cone_decref(cs[i]);
                mae_fini(&ns[i]);
            }
            return -1;
        }
        if (ns[i].exported.size)
            msg += sprintf(msg, "c%u=%d ", i, *(int32_t*)ns[i].exported.data[0].data);
        mae_fini(&ns[i]);
    }
    return 0;
}

static int test_mae_client_impl(struct mae *n) {
    struct cone *u = cone(&test_mae_server, n);
    if (test_mae_ok_call(n) MUN_RETHROW)
        return cone_cancel(u), cone_decref(u), -1;
    cone_yield();
    return cone_cancel(u), cone_join(u);
}

static int test_mae_server_client(char *msg) {
    return test_mae_run(&test_mae_server, &test_mae_client_impl, msg);
}

static int test_mae_client_client(char *msg) {
    return test_mae_run(&test_mae_client_impl, &test_mae_client_impl, msg);
}

static int test_mae_many_clients_impl(struct mae *n) {
    const unsigned N = 4096;
    struct cone *u = cone(&test_mae_server, n);
    struct cone *cs[N];
    int err = 0;
    for (unsigned i = 0; i < N; i++)
        cs[i] = cone(&test_mae_ok_call, n);
    for (unsigned i = 0; i < N; i++)
        err |= cone_join(cs[i]);
    return cone_cancel(u), err | cone_join(u);
}

static int test_mae_many_clients(char *msg) {
    return test_mae_run(&test_mae_many_clients_impl, &test_mae_many_clients_impl, msg);
}

static int test_mae_invalid_client(struct mae *n) {
    struct cone *u = cone(&test_mae_server, n);
    if (test_mae_bad_call(n) MUN_RETHROW)
        return cone_cancel(u), cone_decref(u), -1;
    return cone_cancel(u), cone_join(u);
}

static int test_mae_invalid_args(char *msg) {
    return test_mae_run(&test_mae_server, &test_mae_invalid_client, msg);
}

static int test_mae_confused_client(struct mae *n) {
    struct cone *u = cone(&test_mae_server, n);
    if (test_mae_nonexistent_call(n) MUN_RETHROW)
        return cone_cancel(u), cone_decref(u), -1;
    return cone_cancel(u), cone_join(u);
}

static int test_mae_invalid_function(char *msg) {
    return test_mae_run(&test_mae_server, &test_mae_confused_client, msg);
}

export { "mae:server + client", &test_mae_server_client }
     , { "mae:client + client", &test_mae_client_client }
     , { "mae:concurrent coroutines", &test_mae_many_clients }
     , { "mae:server + bad arguments", &test_mae_invalid_args }
     , { "mae:server + bad function", &test_mae_invalid_function }