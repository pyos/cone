#pragma once
#ifndef NERO_MAX_FRAME_SIZE
#define NERO_MAX_FRAME_SIZE 65536
#endif

#include "mun.h"
#include "cone.h"
#include "romp.h"

struct nero_closure
{
    const char *name;
    int (*func)(void *, struct romp_iovec *in, struct romp_iovec *out);
    void *data;
};

struct nero
{
    int fd;
    unsigned next_req, skip;
    struct cone *writer;
    struct romp_iovec buffer;
    struct mun_vec(struct nero_future) queued;
    struct mun_vec(struct nero_closure) exported;
};

static inline int nero_add(struct nero *n, const struct nero_closure *cbs, size_t count) {
    return mun_vec_extend(&n->exported, cbs, count);
}

void nero_fini (struct nero *);
int  nero_run  (struct nero *);
int  nero_call (struct nero *, const char *function, ...);

#define nero_closure(name, f, data) ((struct nero_closure){name, (int(*)(void*,struct romp_iovec*,struct romp_iovec*))(f), data})
