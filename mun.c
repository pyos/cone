#if defined(_GNU_SOURCE)
// wrong strerror_r
#undef _GNU_SOURCE
#endif

#include "mun.h"
#include <time.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/time.h>

mun_usec mun_usec_now(void) {
    struct timespec val;
    clock_gettime(CLOCK_REALTIME, &val);
    return (mun_usec)val.tv_sec * 1000000ull + val.tv_nsec / 1000;
}

mun_usec mun_usec_monotonic(void) {
    struct timespec val;
    clock_gettime(CLOCK_MONOTONIC, &val);
    return (mun_usec)val.tv_sec * 1000000ull + val.tv_nsec / 1000;
}

static _Thread_local struct mun_error mun_global_err;
static _Thread_local struct mun_error *mun_global_eptr;

struct mun_error *mun_last_error(void) {
    if (mun_global_eptr == NULL)
        mun_global_eptr = &mun_global_err;
    return mun_global_eptr;
}

struct mun_error *mun_set_error_storage(struct mun_error *p) {
    struct mun_error *ret = mun_last_error();
    mun_global_eptr = p;
    return ret;
}

static void mun_error_fmt(const char *fmt, va_list args) {
    struct mun_error *ep = mun_last_error();
    char tmp[sizeof(ep->text)];
    int r = vsnprintf(tmp, sizeof(tmp), fmt, args);
    if (ep->text[0] && r >= 0 && r + 2 < (int)sizeof(tmp)) {
        memmove(tmp + r, ": ", 2);
        memmove(tmp + r + 2, ep->text, sizeof(tmp) - r - 2);
    }
    tmp[sizeof(tmp) - 1] = 0;
    strcpy(ep->text, tmp);
}

int mun_error_at(int n, const char *name, const struct mun_stackframe *frame, const char *fmt, ...) {
    struct mun_error *ep = mun_last_error();
    ep->code = n < 0 ? -n : n;
    ep->stacklen = 0;
    ep->name = name;
    if (n >= 0 || strerror_r(-n, ep->text, sizeof(ep->text))) {
        ep->text[0] = 0;
        va_list args;
        va_start(args, fmt);
        mun_error_fmt(fmt, args);
        va_end(args);
    }
    errno = ep->code;
    return mun_error_up(frame);
}

int mun_error_up(const struct mun_stackframe *frame) {
    struct mun_error *ep = mun_last_error();
    if (ep->stacklen < sizeof(ep->stack) / sizeof(frame))
        ep->stack[ep->stacklen++] = frame;
    return -1;
}

int mun_error_up_ctx(const struct mun_stackframe *frame, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    mun_error_fmt(fmt, args);
    va_end(args);
    return mun_error_up(frame);
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
        err = mun_last_error();
    fprintf(stderr, mun_error_fmt_head[ansi], prefix, err->code, err->name, err->text);
    for (unsigned i = 0; i < err->stacklen; i++)
        fprintf(stderr, mun_error_fmt_line[ansi], i + 1, err->stack[i]->file, err->stack[i]->line, err->stack[i]->func);
}

int mun_vec_reserve_s(size_t s, struct mun_vec *v, size_t n) {
    size_t cap = v->cap & ~MUN_VEC_STATIC_BIT;
    if (n <= cap)
        return 0;
    void *start = (char*)v->data - v->off * s;
    if (v->off) {
        cap += v->off;
        if (v->cap & MUN_VEC_STATIC_BIT ? n <= cap : n + n/5 <= cap)
            return *v = (struct mun_vec){memmove(start, v->data, v->size * s), v->size, v->cap + v->off, 0}, 0;
    }
    cap += cap/2;
    if (cap < 64 / s)
        cap = 64 / s;
    if (cap < n + 4)
        cap = n + 4;
    void *r = malloc(cap * s);
    if (r == NULL)
        return mun_error(ENOMEM, "%zu * %zu bytes", cap, s);
    if (v->size)
        memmove(r, v->data, v->size * s);
    if (!(v->cap & MUN_VEC_STATIC_BIT))
        free(start);
    return *v = (struct mun_vec){r, v->size, cap, 0}, 0;
}
