#include "mun.h"
#undef mun_error
#undef mun_error_up
#ifndef MUN_ANSI_TERM
#    define MUN_ANSI_TERM 1
#endif
#if MUN_ANSI_TERM
#    define ANSI_ITALIC "\033[5m"
#    define ANSI_RED    "\033[31;1m"
#    define ANSI_YELLOW "\033[33;1m"
#    define ANSI_BLUE   "\033[34;1m"
#    define ANSI_RESET  "\033[0m"
#else
#    define ANSI_ITALIC
#    define ANSI_RED
#    define ANSI_YELLOW
#    define ANSI_BLUE
#    define ANSI_RESET
#endif

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

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
    if (n != mun_errno_os || strerror_r((e.code |= errno) & ~mun_errno_os, e.text, sizeof(e.text)))
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
    fprintf(stderr, ANSI_RED " # mun:" ANSI_RESET " %s error " ANSI_RED "%u" ANSI_RESET " "
                    ANSI_ITALIC "(%s)" ANSI_RESET ": %s\n", prefix, err->code & ~mun_errno_os, err->name, err->text);
    for (unsigned i = 0; i < err->stacklen; i++)
        fprintf(stderr, "      " ANSI_YELLOW "@ " ANSI_RESET "%s() " ANSI_ITALIC "(%s:%u)" ANSI_RESET "\n",
                        err->stack[i].func, err->stack[i].file, err->stack[i].line);
}
