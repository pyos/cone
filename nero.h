#pragma once
#include "mun.h"
#include "cone.h"
#include "romp.h"

enum
{
    mun_errno_nero_http2 = mun_errno_custom + 31,
};

struct nero_object;

typedef int nero_point(struct nero_object *, struct romp_iovec *in, struct romp_iovec *out);

struct nero_method
{
    const char *name;
    nero_point *func;
};

struct nero_object mun_vec(struct nero_method);

struct nero_service
{
    const char *name;
    struct nero_object *obj;
};

struct nero
{
    int fd;
    unsigned next_req;
    struct cone *writer;
    struct romp_iovec buffer;
    struct cno_connection_t *http;
    struct mun_vec(struct nero_service) services;
    struct mun_vec(struct nero_future) queued;
};

int  nero_sub  (struct nero_object *, const char *name, nero_point);
int  nero_add  (struct nero *, const char *name, struct nero_object *);
int  nero_init (struct nero *);
void nero_fini (struct nero *);
int  nero_run  (struct nero *);
int  nero_stop (struct nero *);
int  nero_call (struct nero *, const char *service, const char *fn, ...);
