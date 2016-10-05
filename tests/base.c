#ifdef export
static int pass() { return mun_ok; }
export { "pass", &pass }
#else

#include "../mun.h"
#include "../cone.h"
#include "../romp.h"
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
        if (!__tests[i].impl(buf))
            printf("\033[A\033[32;1m + \033[0m\033[1m%s\033[0m\033[K%s%s\n", __tests[i].name, buf[0] ? ": " : "", buf);
        else {
            const struct mun_error *e = mun_last_error();
            printf("\033[A\033[31;1m - \033[0m\033[1m%s\033[0m\033[K: \033[5m%s\033[0m\n", __tests[i].name, e->text);
            printf("   \033[31;1merror %u (%s)\033[0m at ", e->code, e->name);
            for (unsigned i = 0; i < e->stacklen; i++) {
                printf("\033[34m%s\033[0m line \033[36m%u\033[0m \033[5m(%s)\033[0m\n", e->stack[i].file, e->stack[i].line, e->stack[i].func);
                if (i != e->stacklen - 1)
                    printf("   \033[8merror %u (%s) at \033[28m", e->code, e->name);
            }
        }
    }
    return 0;
}
#endif
