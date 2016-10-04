#pragma once
#include "cot.h"

struct cone_closure
{
    int (*code)(void*);
    void *data;
};

extern _Thread_local struct cone * volatile cone;
int          cone_unblock (int fd);
int          cone_root    (size_t stack, struct cone_closure);
struct cone *cone_spawn   (size_t stack, struct cone_closure);
void         cone_incref  (struct cone *);
int          cone_decref  (struct cone *);
int          cone_join    (struct cone *);
int          cone_iowait  (int fd, int write);
int          cone_sleep   (cot_nsec delay);
int          cone_yield   (void);

#define cone_bind(f, data) ((struct cone_closure){(int(*)(void*))f, data})
#define cone(f, arg) cone_spawn(0, cone_bind(f, arg))
