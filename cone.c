/*
 * cone / coroutines
 *        --     --
 */
#include "cone.h"

_Thread_local struct cone * volatile cone;

extern int comain(int argc, const char **argv);

struct cone_comain {
    int retcode;
    int argc;
    const char **argv;
};

static int cone_comain(struct cone_comain *c) {
    c->retcode = comain(c->argc, c->argv);
    return 0;
}

extern int main(int argc, const char **argv) {
    struct cone_comain c = {1, argc, argv};
    if (cone_main(0, cone_bind(&cone_comain, &c)) || c.retcode == -1)
        cot_error_show("cot:main");
    return c.retcode == -1 ? 1 : c.retcode;
}
