#pragma once
#if !defined(CONE_EPOLL) && __linux__
#define CONE_EPOLL 1
#endif
#if !defined(CONE_XCHG_RSP) && __linux__ && __x86_64__
#define CONE_XCHG_RSP 1
#endif
#ifndef CONE_DEFAULT_STACK
#define CONE_DEFAULT_STACK 65536
#endif

#include "mun.h"

struct cone_closure
{
    int (*code)(void*);
    void *data;
};

struct cone_event mun_vec(struct cone_closure);
int  cone_event_add(struct cone_event *, struct cone_closure);
void cone_event_del(struct cone_event *, struct cone_closure);
int  cone_event_emit(struct cone_event *);

extern _Thread_local struct cone * volatile cone;
int          cone_unblock (int fd);
int          cone_root    (size_t stack, struct cone_closure);
struct cone *cone_spawn   (size_t stack, struct cone_closure);
int          cone_cancel  (struct cone *);
void         cone_incref  (struct cone *);
int          cone_decref  (struct cone *);
int          cone_join    (struct cone *);
int          cone_wait    (struct cone_event *);
int          cone_iowait  (int fd, int write);
int          cone_sleep   (mun_usec delay);
int          cone_yield   (void);

#define cone_bind(f, data) ((struct cone_closure){(int(*)(void*))f, data})
#define cone(f, arg) cone_spawn(0, cone_bind(f, arg))
