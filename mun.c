#include "mun.h"
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#if __APPLE__ && __MACH__
#include <mach/clock.h>
#include <mach/mach.h>

static clock_serv_t mun_mach_clock;

static void __attribute__((constructor)) mun_mach_clock_init(void) {
    host_get_clock_service(mach_host_self(), SYSTEM_CLOCK, &mun_mach_clock);
}

static void __attribute__((destructor)) mun_mach_clock_fini(void) {
    mach_port_deallocate(mach_task_self(), mun_mach_clock);
}

mun_usec mun_usec_now() {
    mach_timespec_t val;
    clock_get_time(mun_mach_clock, &val);
    return (uint64_t)val.tv_sec * 1000000ull + val.tv_nsec / 1000;
}
#endif

static _Thread_local struct mun_error e;

const struct mun_error *mun_last_error(void) {
    return &e;
}

int mun_error_restore(const struct mun_error *err) {
    e = *err;
    return -1;
}

int mun_error_at(int n, const char *name, const char *file, const char *func, unsigned line, const char *fmt, ...) {
    e = (struct mun_error){.code = n < 0 ? -n : n, .stacklen = 0, .name = name};
    va_list args;
    va_start(args, fmt);
    if (n >= 0 || strerror_r(-n, e.text, sizeof(e.text)))
        vsnprintf(e.text, sizeof(e.text), fmt, args);
    va_end(args);
    return mun_error_up_at(file, func, line);
}

int mun_error_up_at(const char *file, const char *func, unsigned line) {
    if (e.stacklen < sizeof(e.stack) / sizeof(e.stack[0]))
        e.stack[e.stacklen++] = (struct mun_stacktrace){file, func, line};
    return -1;
}

static const char
    *mun_error_fmt_head[2] = {" # mun: %s error %d (%s): %s\n",
                              "\033[1;31m # mun:\033[0m %s error \033[1;31m%d\033[0m \033[5m(%s)\033[0m: %s\n"},
    *mun_error_fmt_line[2] = {"   %3u. %s() (%s:%u)\n",
                              "\033[1;33m   %3u.\033[0m %s() \033[5m(%s:%u)\033[0m\n"};

void mun_error_show(const char *prefix, const struct mun_error *err) {
    int ansi = isatty(fileno(stderr)) && !strncmp(getenv("TERM"), "xterm", 5);
    if (err == NULL)
        err = &e;
    fprintf(stderr, mun_error_fmt_head[ansi], prefix, err->code, err->name, err->text);
    for (unsigned i = 0; i < err->stacklen; i++)
        fprintf(stderr, mun_error_fmt_line[ansi], i + 1, err->stack[i].func, err->stack[i].file, err->stack[i].line);
}
