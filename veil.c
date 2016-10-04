#include "veil.h"
#undef veil_error
#undef veil_error_up

#include <stdio.h>
#include <stdarg.h>

static _Thread_local char message[128];
static _Thread_local struct veil_error e;
static _Thread_local struct veil_stacktrace frames[32];

const struct veil_error *veil_last_error(void) {
    return &e;
}

int veil_error(unsigned n, const char *name, const char *file, const char *func, unsigned line, const char *fmt, ...) {
    e = (struct veil_error){.code = n, .stacklen = 0, .name = name, .text = message, .stack = frames};
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    return veil_error_up(file, func, line);
}

int veil_error_up(const char *file, const char *func, unsigned line) {
    if (e.stacklen >= sizeof(frames) / sizeof(struct veil_stacktrace))
        return -1;
    frames[e.stacklen++] = (struct veil_stacktrace){file, func, line};
    return -1;
}

void veil_error_show(const char *prefix) {
    fprintf(stderr, "%s: error %d (%s): %s\n", prefix, e.code, e.name, e.text);
    fprintf(stderr, "== traceback == most recent call first ==\n");
    for (unsigned i = 0; i < e.stacklen; i++)
        fprintf(stderr, "%s line %u, in %s\n", frames[i].file, frames[i].line, frames[i].func);
    fprintf(stderr, "== traceback end ==\n");
}
