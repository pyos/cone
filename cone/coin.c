#include "cone.h"

extern int comain(int argc, const char **argv);

struct cone_comain
{
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
    return cone_main(0, cone_bind(&cone_comain, &c)) ? 1 : c.retcode;
}
