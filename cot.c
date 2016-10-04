/*
 * cot / common toolbox
 *       --     -
 */
#include "cot.h"
#undef cot_error
#undef cot_error_up

#include <stdio.h>
#include <stdarg.h>

static _Thread_local struct cot_error e = {.code = cot_ok, .name = ""};

const struct cot_error *cot_last_error(void) {
    return &e;
}

int cot_error(enum cot_errno code, const char *file, unsigned line, const char *name, const char *fmt, ...) {
    e.code = code;
    e.name = name;
    e.trace[0] = (struct cot_error_traceback){file, line};
    e.trace[1] = (struct cot_error_traceback){};
    va_list args;
    va_start(args, fmt);
    vsnprintf(e.text, sizeof(e.text), fmt, args);
    va_end(args);
    return -1;
}

int cot_error_up(const char *file, int line) {
    for (size_t i = 0; i < sizeof(e.trace) / sizeof(struct cot_error_traceback) - 1; i++) {
        if (e.trace[i].file == NULL) {
            e.trace[i] = (struct cot_error_traceback){file, line};
            break;
        }
    }
    return -1;
}

void cot_error_show(const char *prefix) {
    fprintf(stderr, "[%s] error %d (`%s'): %s\n", prefix ? prefix : "cot", e.code, e.name, e.text);
    for (unsigned i = 0; e.trace[i].file; i++)
        fprintf(stderr, "  #%u\t%s:%u\n", i, e.trace[i].file, e.trace[i].line);
}
