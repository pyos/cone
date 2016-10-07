#pragma once
#ifndef NERO_MAX_FRAME_SIZE
#define NERO_MAX_FRAME_SIZE 65535
#endif

#include "mun.h"
#include "cone.h"
#include "romp.h"

enum
{
    mun_errno_nero_overflow = mun_errno_custom + 13000,
    mun_errno_nero_protocol,
    mun_errno_nero_not_exported,
};

struct nero_closure
{
    const char *name;
    int (*code)(void *, struct romp_iovec *in, struct romp_iovec *out);
    void *data;
};

struct nero
{
    int fd;
    unsigned last_id;
    struct cone *writer;
    struct romp_iovec wbuffer;
    struct romp_iovec rbuffer;
    struct mun_vec(struct nero_future*) queued;
    struct mun_vec(struct nero_closure) exported;
};

static inline int nero_add(struct nero *n, const struct nero_closure *cbs, size_t count) {
    return mun_vec_extend(&n->exported, cbs, count);
}

void nero_fini     (struct nero *);
int  nero_run      (struct nero *);
int  nero_call_var (struct nero *, const char *function, va_list);
int  nero_call     (struct nero *, const char *function, ... /* romp_sign_input, ..., romp_sign_return, ... */);

#define nero_closure(name, f, data) ((struct nero_closure){name, (int(*)(void*,struct romp_iovec*,struct romp_iovec*))(f), data})
