#pragma once
#include "mun.h"
#include "cone.h"
#include "romp.h"

enum
{
    mun_errno_nero_http2 = mun_errno_custom + 15000,
};

struct nero_closure
{
    const char *name;
    int (*func)(void *, struct romp_iovec *in, struct romp_iovec *out);
    void *data;
};

struct nero
{
    int fd;
    unsigned next_req;
    struct cone *writer;
    struct romp_iovec buffer;
    struct cno_connection_t *http;
    struct mun_vec(struct nero_future) queued;
    struct mun_vec(struct nero_closure) exported;
};

static inline int nero_add(struct nero *n, const struct nero_closure *cbs, size_t count) {
    return mun_vec_extend(&n->exported, cbs, count);
}

int  nero_init (struct nero *);
void nero_fini (struct nero *);
int  nero_run  (struct nero *);
int  nero_stop (struct nero *);
int  nero_call (struct nero *, const char *function, ...);

#define nero_closure(name, f, data) ((struct nero_closure){name, (int(*)(void*,struct romp_iovec*,struct romp_iovec*))(f), data})
