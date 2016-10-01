#pragma once
#include "closure.h"

#include <stdlib.h>
#include <unistd.h>
#include <sys/epoll.h>

#if !defined(COROUTINE_EPOLL) && defined(__linux__)
#define COROUTINE_EPOLL 1
#endif

#ifndef COROUTINE_FD_BUCKETS
#define COROUTINE_FD_BUCKETS 127
#endif

#ifndef COROUTINE_FD_RETRY_INTERVAL
#define COROUTINE_FD_RETRY_INTERVAL CO_U128(30000000000ull)
#endif

struct co_event_fdx2
{
    int fd;
#if COROUTINE_EPOLL
    int epoll;
    struct epoll_event params;
#endif
    struct co_closure cbs[2];
    struct co_event_fdx2 *link;
};

struct co_event_fd
{
    int write;
    struct co_event_fdx2 *parent;
};

struct co_fd_set
{
#if COROUTINE_EPOLL
    int epoll;
#endif
    struct co_event_fdx2 *fds[COROUTINE_FD_BUCKETS];
};

static inline struct co_event_fdx2 *
co_event_fdx2_alloc(struct co_fd_set *set, int fd) {
    struct co_event_fdx2 *x = (struct co_event_fdx2 *)malloc(sizeof(struct co_event_fdx2));
    if (x == NULL)
        return NULL;
    *x = (struct co_event_fdx2){
        .fd     = fd,
    #if COROUTINE_EPOLL
        .epoll  = set->epoll,
        .params = {EPOLLRDHUP | EPOLLHUP | EPOLLET | EPOLLONESHOT, {.ptr = x}},
    #endif
        .link   = set->fds[fd % COROUTINE_FD_BUCKETS],
    };
    set->fds[fd % COROUTINE_FD_BUCKETS] = x;
#if COROUTINE_EPOLL
    epoll_ctl(set->epoll, EPOLL_CTL_ADD, fd, &x->params);
#endif
    return x;
}

static inline void
co_event_fdx2_dealloc(struct co_fd_set *set, struct co_event_fdx2 *ev) {
#if COROUTINE_EPOLL
    epoll_ctl(ev->epoll, EPOLL_CTL_DEL, ev->fd, NULL);
#endif
    struct co_event_fdx2 **b = &set->fds[ev->fd % COROUTINE_FD_BUCKETS];
    while (*b != ev) b = &(*b)->link;
    *b = ev->link;
    free(ev);
}

static inline int
co_event_fdx2_emit(struct co_event_fdx2 *ev, int write) {
    struct co_closure cb = ev->cbs[write];
    ev->cbs[write] = (struct co_closure){};  // fd already unregistered due to EPOLLONESHOT
    return cb.function && cb.function(cb.data);
}

static inline struct co_event_fd
co_event_fd(struct co_fd_set *set, int fd, int write) {
    struct co_event_fdx2 *ev = set->fds[fd % COROUTINE_FD_BUCKETS];
    while (ev && ev->fd != fd) ev = ev->link;
    return (struct co_event_fd){write, ev != NULL ? ev : co_event_fdx2_alloc(set, fd)};
}

static inline struct co_event_fd
co_event_fd_read(struct co_fd_set *set, int fd) {
    return co_event_fd(set, fd, 0);
}

static inline struct co_event_fd
co_event_fd_write(struct co_fd_set *set, int fd) {
    return co_event_fd(set, fd, 1);
}

static inline int
co_event_fd_connect(struct co_event_fd *ev, struct co_closure cb) {
    if (!ev->parent || ev->parent->cbs[ev->write].function)
        return -1;
    ev->parent->cbs[ev->write] = cb;
#if COROUTINE_EPOLL
    ev->parent->params.events |= ev->write ? EPOLLOUT : EPOLLIN;
    return epoll_ctl(ev->parent->epoll, EPOLL_CTL_MOD, ev->parent->fd, &ev->parent->params);
#else
    return 0;
#endif
}

static inline int
co_event_fd_disconnect(struct co_event_fd *ev, struct co_closure cb) {
    if (!ev->parent || ev->parent->cbs[ev->write].function != cb.function || ev->parent->cbs[ev->write].data != cb.data)
        return -1;
    ev->parent->cbs[ev->write] = (struct co_closure){};
#if COROUTINE_EPOLL
    ev->parent->params.events &= ~(ev->write ? EPOLLOUT : EPOLLIN);
    return epoll_ctl(ev->parent->epoll, EPOLL_CTL_MOD, ev->parent->fd, &ev->parent->params);
#else
    return 0;
#endif
}

static inline int
co_fd_set_init(struct co_fd_set *set) {
#if COROUTINE_EPOLL
    *set = (struct co_fd_set){.epoll = epoll_create1(0)};
    return set->epoll < 0 ? -1 : 0;
#else
    *set = (struct co_fd_set){};
    return 0;
#endif
}

static inline void
co_fd_set_fini(struct co_fd_set *set) {
    for (int i = 0; i < COROUTINE_FD_BUCKETS; i++)
        while (set->fds[i])
            co_event_fdx2_dealloc(set, set->fds[i]);
#if COROUTINE_EPOLL
    close(set->epoll);
#endif
}

static inline int
co_fd_set_emit(struct co_fd_set *set, struct co_nsec_offset timeout) {
    if (co_u128_eq(timeout, CO_U128(0)) || co_u128_gt(timeout, COROUTINE_FD_RETRY_INTERVAL))
        timeout = COROUTINE_FD_RETRY_INTERVAL;
#if COROUTINE_EPOLL
    struct epoll_event evs[32];
    int got = epoll_wait(set->epoll, evs, 32, co_u128_div(timeout, 1000000ul).L);
    if (got <= 0)
        return got;
    for (size_t i = 0; i < (size_t)got; i++) {
        struct co_event_fdx2 *c = (struct co_event_fdx2*)evs[i].data.ptr;
        if (evs[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP))
            co_event_fdx2_emit(c, 0);
        if (evs[i].events & (EPOLLOUT | EPOLLERR | EPOLLHUP))
            co_event_fdx2_emit(c, 1);
        if (c->cbs[0].function == NULL && c->cbs[1].function == NULL)
            co_event_fdx2_dealloc(set, c);
    }
#else
    int max_fd = 0;
    fd_set r, w;
    FD_ZERO(&r);
    FD_ZERO(&w);
    for (size_t i = 0; i < COROUTINE_FD_BUCKETS; i++) {
        for (const struct co_event_fdx2 *c = set->fds[i]; c; c = c->link) {
            if (max_fd <= c->fd)
                max_fd = c->fd + 1;
            if (c->cbs[0].function)
                FD_SET(c->fd, &r);
            if (c->cbs[1].function)
                FD_SET(c->fd, &w);
        }
    }
    struct timeval us = {co_u128_div(timeout, 1000000000ull).L, timeout.L % 1000000000ull / 1000};
    int got = select(max_fd, &r, &w, NULL, &us);
    if (got <= 0)
        return got;
    for (size_t i = 0; i < COROUTINE_FD_BUCKETS; i++) {
        for (struct co_event_fdx2 *c = set->fds[i]; c; c = c->link) {
            if (FD_ISSET(c->fd, &r))
                co_event_fdx2_emit(c, 0);
            if (FD_ISSET(c->fd, &w))
                co_event_fdx2_emit(c, 1);
            if (c->cbs[0].function == NULL && c->cbs[1].function == NULL)
                co_event_fdx2_dealloc(set, c);
        }
    }
#endif
    return 0;
}
