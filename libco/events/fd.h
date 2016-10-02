#pragma once
#include "vec.h"
#include "time.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#if !defined(COROUTINE_EPOLL) && defined(__linux__)
#define COROUTINE_EPOLL 1
#endif

#if COROUTINE_EPOLL
#include <sys/epoll.h>
#else
#include <sys/select.h>
#endif

#ifndef COROUTINE_FD_BUCKETS
#define COROUTINE_FD_BUCKETS 127
#endif

#ifndef COROUTINE_FD_INTERVAL
#define COROUTINE_FD_INTERVAL 30000000000ull
#endif

struct co_fd_monitor
{
    int fd;
    struct co_closure cbs[2];
    struct co_fd_monitor *link;
#if COROUTINE_EPOLL
    struct epoll_event params;
#endif
};

struct co_event_fd
{
    struct co_fd_monitor *fds[COROUTINE_FD_BUCKETS];
#if COROUTINE_EPOLL
    int epoll;
#endif
};

static inline struct co_fd_monitor **
co_fd_monitor_bucket(struct co_event_fd *set, int fd) {
    struct co_fd_monitor **b = &set->fds[fd % COROUTINE_FD_BUCKETS];
    while (*b && (*b)->fd != fd) b = &(*b)->link;
    return b;
}

static inline struct co_fd_monitor *
co_fd_monitor(struct co_event_fd *set, int fd) {
    struct co_fd_monitor **b = co_fd_monitor_bucket(set, fd);
    if (!*b && (*b = (struct co_fd_monitor *) calloc(1, sizeof(struct co_fd_monitor)))) {
        (*b)->fd = fd;
    #if COROUTINE_EPOLL
        (*b)->params = (struct epoll_event){EPOLLRDHUP | EPOLLHUP | EPOLLET | EPOLLONESHOT, {.ptr = *b}};
        if (epoll_ctl(set->epoll, EPOLL_CTL_ADD, fd, &(*b)->params))
            return free(*b), (*b = NULL);
    #endif
    }
    return *b;
}

static inline void
co_fd_monitor_dealloc(struct co_event_fd *set, struct co_fd_monitor *ev) {
#if COROUTINE_EPOLL
    epoll_ctl(set->epoll, EPOLL_CTL_DEL, ev->fd, NULL);
#endif
    *co_fd_monitor_bucket(set, ev->fd) = ev->link;
    free(ev);
}

static inline int
co_fd_monitor_emit(struct co_fd_monitor *ev, int write) {
    struct co_closure cb = ev->cbs[write];
    ev->cbs[write] = (struct co_closure){};
    return cb.function && cb.function(cb.data);
}

static inline int
co_event_fd_init(struct co_event_fd *set) {
#if COROUTINE_EPOLL
    return (set->epoll = epoll_create1(0)) < 0 ? -1 : 0;
#else
    (void)set;
    return 0;
#endif
}

static inline void
co_event_fd_fini(struct co_event_fd *set) {
    for (int i = 0; i < COROUTINE_FD_BUCKETS; i++)
        while (set->fds[i])
            co_fd_monitor_dealloc(set, set->fds[i]);
#if COROUTINE_EPOLL
    close(set->epoll);
#endif
}

static inline int
co_event_fd_connect(struct co_event_fd *set, int fd, int write, struct co_closure cb) {
    struct co_fd_monitor *ev = co_fd_monitor(set, fd);
    if (ev == NULL || ev->cbs[write].function)
        return -1;
    ev->cbs[write] = cb;
#if COROUTINE_EPOLL
    ev->params.events |= write ? EPOLLOUT : EPOLLIN;
    return epoll_ctl(set->epoll, EPOLL_CTL_MOD, fd, &ev->params);
#else
    return 0;
#endif
}

static inline int
co_event_fd_disconnect(struct co_event_fd *set, int fd, int write, struct co_closure cb) {
    struct co_fd_monitor *ev = co_fd_monitor(set, fd);
    if (ev == NULL || ev->cbs[write].function != cb.function || ev->cbs[write].data != cb.data)
        return -1;
    ev->cbs[write] = (struct co_closure){};
#if COROUTINE_EPOLL
    ev->params.events &= ~(write ? EPOLLOUT : EPOLLIN);
    return epoll_ctl(set->epoll, EPOLL_CTL_MOD, fd, &ev->params);
#else
    return 0;
#endif
}

static inline int
co_event_fd_emit(struct co_event_fd *set, struct co_nsec timeout) {
    if (co_u128_eq(timeout, CO_U128(0)))
        return -1;
    if (co_u128_gt(timeout, CO_U128(COROUTINE_FD_INTERVAL)))
        timeout = CO_U128(COROUTINE_FD_INTERVAL);
#if COROUTINE_EPOLL
    struct epoll_event evs[32];
    int got = epoll_wait(set->epoll, evs, 32, co_u128_div(timeout, 1000000ul).L);
    if (got < 0)
        return errno == EINTR ? 0 : -1;
    for (size_t i = 0; i < (size_t)got; i++) {
        struct co_fd_monitor *c = (struct co_fd_monitor*)evs[i].data.ptr;
        if (evs[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP))
            co_fd_monitor_emit(c, 0);
        if (evs[i].events & (EPOLLOUT | EPOLLERR | EPOLLHUP))
            co_fd_monitor_emit(c, 1);
        if (c->cbs[0].function == NULL && c->cbs[1].function == NULL)
            co_fd_monitor_dealloc(set, c);
    }
#else
    fd_set fds[2];
    FD_ZERO(&fds[0]);
    FD_ZERO(&fds[1]);
    int max_fd = 0;
    for (size_t i = 0; i < COROUTINE_FD_BUCKETS; i++) {
        for (struct co_fd_monitor *c = set->fds[i], *next = NULL; c; c = next) {
            next = c->link;
            if (c->cbs[0].function == NULL && c->cbs[1].function == NULL) {
                co_fd_monitor_dealloc(set, c);
                continue;
            }
            if (max_fd <= c->fd)
                max_fd = c->fd + 1;
            for (int i = 0; i < 2; i++)
                if (c->cbs[i].function)
                    FD_SET(c->fd, &fds[i]);
        }
    }
    struct timeval us = {co_u128_div(timeout, 1000000000ull).L, timeout.L % 1000000000ull / 1000};
    if (select(max_fd, &fds[0], &fds[1], NULL, &us) < 0)
        return errno == EINTR ? 0 : 1;
    for (size_t i = 0; i < COROUTINE_FD_BUCKETS; i++)
        for (struct co_fd_monitor *c = set->fds[i]; c; c = c->link)
            for (int i = 0; i < 2; i++)
                if (FD_ISSET(c->fd, &fds[i]))
                    co_fd_monitor_emit(c, i);
#endif
    return 0;
}
