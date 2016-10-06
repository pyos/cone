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

void mun_error_show(const char *prefix) {
    fprintf(stderr, "%s: error %d (%s): %s\n", prefix, e.code, e.name, e.text);
    fprintf(stderr, "== traceback == most recent call first ==\n");
    for (unsigned i = 0; i < e.stacklen; i++)
        fprintf(stderr, "%s line %u, in %s\n", e.stack[i].file, e.stack[i].line, e.stack[i].func);
    fprintf(stderr, "== traceback end ==\n");
}
