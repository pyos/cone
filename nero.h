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

struct nero
{
    int fd;
    unsigned last_id;
    struct cone *writer;
    struct romp wbuffer;
    struct romp rbuffer;
    struct mun_vec(struct nero_future*) queued;
    struct mun_vec(struct nero_closure) exported;
};

struct nero_closure
{
    const char *name;
    int (*code)(struct nero *, void *, struct romp *in, struct romp *out);
    void *data;
};

static inline int nero_add(struct nero *n, const struct nero_closure *cbs, size_t count) {
    return mun_vec_extend(&n->exported, cbs, count);
}

static inline void nero_del(struct nero *n, const char *name) {
    for (unsigned i = 0; i < n->exported.size; i++)
        if (!strcmp(name, n->exported.data[i].name))
            return (void) mun_vec_erase(&n->exported, i, 1);
}

void nero_fini     (struct nero *);
int  nero_run      (struct nero *);
int  nero_call_var (struct nero *, const char *function, va_list);
int  nero_call     (struct nero *, const char *function, ... /* romp_sign_input, ..., romp_sign_return, ... */);

#define nero_closure(name, f, data) ((struct nero_closure){name, (int(*)(struct nero*,void*,struct romp*,struct romp*))(f), data})
