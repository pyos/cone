#pragma once
#include "../generic/closure.h"
#include "../generic/time.h"

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

struct co_fd_duplex;

struct co_event_fd
{
    struct co_closure cb;
    struct co_fd_duplex *parent;
};

struct co_fd_duplex
{
    int fd;
#if COROUTINE_EPOLL
    int epoll;
    struct epoll_event params;
#endif
    struct co_event_fd read;
    struct co_event_fd write;
    struct co_fd_duplex *link;
};

struct co_fd_set
{
#if COROUTINE_EPOLL
    int epoll;
#endif
    struct co_fd_duplex *fds[COROUTINE_FD_BUCKETS];
};

static inline int
co_event_fd_connect(struct co_event_fd *ev, struct co_closure cb) {
    if (ev->cb.function)
        return -1;
    ev->cb = cb;
#if COROUTINE_EPOLL
    ev->parent->params.events |= ev == &ev->parent->read ? EPOLLIN : EPOLLOUT;
    return epoll_ctl(ev->parent->epoll, EPOLL_CTL_MOD, ev->parent->fd, &ev->parent->params);
#else
    return 0;
#endif
}

static inline int
co_event_fd_disconnect(struct co_event_fd *ev, struct co_closure cb) {
    if (ev->cb.function != cb.function || ev->cb.data != cb.data)
        return -1;
    ev->cb = (struct co_closure){};
#if COROUTINE_EPOLL
    ev->parent->params.events &= ~(ev == &ev->parent->read ? EPOLLIN : EPOLLOUT);
    return epoll_ctl(ev->parent->epoll, EPOLL_CTL_MOD, ev->parent->fd, &ev->parent->params);
#else
    return 0;
#endif
}

static inline int
co_event_fd_emit(struct co_event_fd *ev) {
    struct co_closure cb = ev->cb;
    ev->cb = (struct co_closure){};  // fd already unregistered due to EPOLLONESHOT
    return cb.function && cb.function(cb.data);
}

static inline struct co_fd_duplex *
co_fd_duplex_alloc(struct co_fd_set *set, int fd) {
    struct co_fd_duplex *x = (struct co_fd_duplex *)malloc(sizeof(struct co_fd_duplex));
    if (x == NULL)
        return NULL;
    *x = (struct co_fd_duplex){
        .fd     = fd,
    #if COROUTINE_EPOLL
        .epoll  = set->epoll,
        .params = {EPOLLRDHUP | EPOLLHUP | EPOLLET | EPOLLONESHOT, {.ptr = x}},
    #endif
        .read   = {.parent = x},
        .write  = {.parent = x},
        .link   = set->fds[fd % COROUTINE_FD_BUCKETS],
    };
    set->fds[fd % COROUTINE_FD_BUCKETS] = x;
#if COROUTINE_EPOLL
    epoll_ctl(set->epoll, EPOLL_CTL_ADD, fd, &x->params);
#endif
    return x;
}

static inline void
co_fd_duplex_dealloc(struct co_fd_set *set, struct co_fd_duplex *ev) {
#if COROUTINE_EPOLL
    epoll_ctl(ev->epoll, EPOLL_CTL_DEL, ev->fd, NULL);
#endif
    struct co_fd_duplex **b = &set->fds[ev->fd % COROUTINE_FD_BUCKETS];
    while (*b != ev) b = &(*b)->link;
    *b = ev->link;
    free(ev);
}

static inline struct co_fd_duplex *
co_fd_duplex_find(const struct co_fd_set *set, int fd) {
    struct co_fd_duplex *ev = set->fds[fd % COROUTINE_FD_BUCKETS];
    while (ev && ev->fd != fd) ev = ev->link;
    return ev;
}

static inline struct co_fd_duplex *
co_fd_duplex(struct co_fd_set *set, int fd) {
    struct co_fd_duplex *ev = co_fd_duplex_find(set, fd);
    return ev != NULL ? ev : co_fd_duplex_alloc(set, fd);
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
            co_fd_duplex_dealloc(set, set->fds[i]);
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
    if (got > 0) {
        for (size_t i = 0; i < (size_t)got; i++) {
            struct co_fd_duplex *c = (struct co_fd_duplex*)evs[i].data.ptr;
            if (evs[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP))
                co_event_fd_emit(&c->read);
            if (evs[i].events & (EPOLLOUT | EPOLLERR | EPOLLHUP))
                co_event_fd_emit(&c->write);
            if (c->read.cb.function == NULL && c->write.cb.function == NULL)
                co_fd_duplex_dealloc(set, c);
        }
    }
#else
    int max_fd = 0;
    fd_set r, w;
    FD_ZERO(&r);
    FD_ZERO(&w);
    for (size_t i = 0; i < COROUTINE_FD_BUCKETS; i++) {
        for (const struct co_fd_duplex *c = set->fds[i]; c; c = c->link) {
            if (max_fd <= c->fd)
                max_fd = c->fd + 1;
            if (c->read.cb.function)
                FD_SET(c->fd, &r);
            if (c->write.cb.function)
                FD_SET(c->fd, &w);
        }
    }
    struct timeval us = {co_u128_div(timeout, 1000000000ull).L, timeout.L % 1000000000ull / 1000};
    int got = select(max_fd, &r, &w, NULL, &us);
    if (got > 0) {
        for (size_t i = 0; i < COROUTINE_FD_BUCKETS; i++) {
            for (struct co_fd_duplex *c = set->fds[i]; c; c = c->link) {
                if (FD_ISSET(c->fd, &r))
                    co_event_fd_emit(&c->read);
                if (FD_ISSET(c->fd, &w))
                    co_event_fd_emit(&c->write);
                if (c->read.cb.function == NULL && c->write.cb.function == NULL)
                    co_fd_duplex_dealloc(set, c);
            }
        }
    }
#endif
    return got < 0 ? -1 : 0;
}
