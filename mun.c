#include "mun.h"
#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/time.h>

mun_usec mun_usec_now(void) {
    struct timeval val;
    gettimeofday(&val, NULL);
    return (mun_usec)val.tv_sec * 1000000ull + val.tv_usec;
}

mun_usec mun_usec_monotonic(void) {
    struct timespec val;
    clock_gettime(CLOCK_MONOTONIC, &val);
    return (mun_usec)val.tv_sec * 1000000ull + val.tv_nsec / 1000;
}

static _Thread_local struct mun_error e;

struct mun_error *mun_last_error(void) {
    return &e;
}

int mun_error_at(int n, const char *name, struct mun_stackframe frame, const char *fmt, ...) {
    errno = e.code = n < 0 ? -n : n;
    e.stacklen = 0;
    e.name = name;
    va_list args;
    va_start(args, fmt);
    // NOTE this requires an XSI, not GNU, version of strerror_r(3). No _GNU_SOURCE allowed!
    if (n >= 0 || strerror_r(-n, e.text, sizeof(e.text)))
        vsnprintf(e.text, sizeof(e.text), fmt, args);
    va_end(args);
    return mun_error_up(frame);
}

int mun_error_up(struct mun_stackframe frame) {
    if (e.stacklen < sizeof(e.stack) / sizeof(frame))
        e.stack[e.stacklen++] = frame;
    return -1;
}

static const char
    *mun_error_fmt_head[2] = {" # mun: %s error %d (%s): %s\n",
                              "\033[1;31m # mun:\033[0m %s error \033[1;31m%d\033[0m \033[3m(%s)\033[0m: %s\n"},
    *mun_error_fmt_line[2] = {"   %3u. %s:%u (%s)\n",
                              "\033[1;33m   %3u.\033[0m %s:%u \033[3m(%s)\033[0m\n"};

void mun_error_show(const char *prefix, const struct mun_error *err) {
    const char *term = getenv("TERM");
    int ansi = term && isatty(fileno(stderr)) && !strncmp(term, "xterm", 5);
    if (err == NULL)
        err = &e;
    fprintf(stderr, mun_error_fmt_head[ansi], prefix, err->code, err->name, err->text);
    for (unsigned i = 0; i < err->stacklen; i++)
        fprintf(stderr, mun_error_fmt_line[ansi], i + 1, err->stack[i].file, err->stack[i].line, err->stack[i].func);
}
