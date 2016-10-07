#ifdef export
static int pass() { return mun_ok; }
static int fail() { return mun_error(assert, "ok"); }
export { "pass", &pass }
     , { "fail", &fail }
#else

#include "../mun.h"
#include "../cone.h"
#include <stdio.h>
#include <stdlib.h>

#define export struct { const char *name; int (*impl)(char *); } __tests[] = {
#define __s1(x) #x
#define __s2(x) __s1(x)
#include __s2(SRC)
};

int comain()
{
    char buf[2048];
    for (unsigned i = 0; i < sizeof(__tests) / sizeof(*__tests); i++) {
        buf[0] = 0;
        printf("\033[33;1m * \033[0m\033[1m%s\033[0m\n", __tests[i].name);
        if (!cone_root(0, cone_bind(__tests[i].impl, buf)))
            printf("\033[A\033[32;1m + \033[0m\033[1m%s\033[0m\033[K%s%s\n", __tests[i].name, buf[0] ? ": " : "", buf);
        else
            mun_error_show("test failed with", NULL);
    }
    return 0;
}
#endif
