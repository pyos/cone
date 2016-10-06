#pragma once
#include "mun.h"

struct cone_closure
{
    int (*code)(void*);
    void *data;
};

struct cone_cond mun_vec(struct cone_closure);

extern _Thread_local struct cone * volatile cone;
int          cone_unblock (int fd);
int          cone_root    (size_t stack, struct cone_closure);
struct cone *cone_spawn   (size_t stack, struct cone_closure);
int          cone_cancel  (struct cone *);
void         cone_incref  (struct cone *);
int          cone_decref  (struct cone *);
int          cone_join    (struct cone *);
int          cone_wait    (struct cone_cond *);
int          cone_notify  (struct cone_cond *);
int          cone_iowait  (int fd, int write);
int          cone_sleep   (mun_nsec delay);
int          cone_yield   (void);

#define cone_bind(f, data) ((struct cone_closure){(int(*)(void*))f, data})
#define cone(f, arg) cone_spawn(0, cone_bind(f, arg))
