#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "coro.h"

_Thread_local struct coro * volatile coro_current;

struct co_amain_ctx
{
    int ret;
    int argc;
    const char **argv;
};

extern int amain(int argc, const char **argv);
static int __co_run_amain(struct co_amain_ctx *c) {
    c->ret = amain(c->argc, c->argv);
    return 0;
}

int main(int argc, const char **argv) {
    struct co_amain_ctx c = {1, argc, argv};
    if (coro_main(co_callback_bind(&__co_run_amain, &c)))
        return 1;
    return c.ret;
}
