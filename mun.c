#include "mun.h"
#undef mun_error
#undef mun_error_up

#include <stdio.h>
#include <stdarg.h>

static _Thread_local struct mun_error e;

const struct mun_error *mun_last_error(void) {
    return &e;
}

int mun_error_restore(const struct mun_error *err) {
    e = *err;
    return -1;
}

int mun_error(unsigned n, const char *name, const char *file, const char *func, unsigned line, const char *fmt, ...) {
    e = (struct mun_error){.code = n, .stacklen = 0, .name = name};
    va_list args;
    va_start(args, fmt);
    vsnprintf(e.text, sizeof(e.text), fmt, args);
    va_end(args);
    return mun_error_up(file, func, line);
}

int mun_error_up(const char *file, const char *func, unsigned line) {
    if (e.stacklen >= sizeof(e.stack) / sizeof(struct mun_stacktrace))
        return -1;
    e.stack[e.stacklen++] = (struct mun_stacktrace){file, func, line};
    return -1;
}

void mun_error_show(const char *prefix, const struct mun_error *err) {
    if (err == NULL)
        err = &e;
    fprintf(stderr, "[error %d] %s %s: %s\n          at ", err->code, prefix, err->name, err->text);
    for (unsigned i = 0; i < err->stacklen; i++)
        fprintf(stderr, "%s%s line %u, in %s\n", i ? "             " : "", err->stack[i].file, err->stack[i].line, err->stack[i].func);
    fprintf(stderr, "\n");
}
