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

#ifndef COROUTINE_TIMEOUT
#define COROUTINE_TIMEOUT 30000000000ull
#endif

struct co_event_fd_sub
{
    int fd;
    struct co_closure cbs[2];
    struct co_event_fd_sub *link;
};

struct co_event_fd
{
    struct co_event_fd_sub *fds[127];
#if COROUTINE_EPOLL
    int epoll;
#endif
};

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
    for (size_t i = 0; i < sizeof(set->fds) / sizeof(set->fds[0]); i++)
        for (struct co_event_fd_sub *c; (c = set->fds[i]) != NULL; free(c))
            set->fds[i] = c->link;
#if COROUTINE_EPOLL
    close(set->epoll);
#endif
}

static inline struct co_event_fd_sub **
co_event_fd_bucket(struct co_event_fd *set, int fd) {
    struct co_event_fd_sub **b = &set->fds[fd % (sizeof(set->fds) / sizeof(set->fds[0]))];
    while (*b && (*b)->fd != fd) b = &(*b)->link;
    return b;
}

static inline struct co_event_fd_sub *
co_event_fd_open(struct co_event_fd *set, int fd) {
    struct co_event_fd_sub **b = co_event_fd_bucket(set, fd);
    if (!*b && (*b = (struct co_event_fd_sub *) calloc(1, sizeof(struct co_event_fd_sub)))) {
        (*b)->fd = fd;
    #if COROUTINE_EPOLL
        struct epoll_event params = {EPOLLRDHUP|EPOLLHUP|EPOLLET|EPOLLIN|EPOLLOUT, {.ptr = *b}};
        if (epoll_ctl(set->epoll, EPOLL_CTL_ADD, fd, &params))
            *b = (free(*b), NULL);
    #endif
    }
    return *b;
}

static inline void
co_event_fd_close(struct co_event_fd *set, struct co_event_fd_sub *ev) {
#if COROUTINE_EPOLL
    epoll_ctl(set->epoll, EPOLL_CTL_DEL, ev->fd, NULL);
#endif
    *co_event_fd_bucket(set, ev->fd) = ev->link;
    free(ev);
}

static inline int
co_event_fd_connect(struct co_event_fd *set, int fd, int write, struct co_closure cb) {
    struct co_event_fd_sub *ev = co_event_fd_open(set, fd);
    if (ev == NULL || ev->cbs[write].function)
        return -1;
    ev->cbs[write] = cb;
    return 0;
}

static inline int
co_event_fd_emit(struct co_event_fd *set, struct co_nsec timeout) {
    if (co_u128_eq(timeout, CO_U128(0)))
        return -1;
    if (co_u128_gt(timeout, CO_U128(COROUTINE_TIMEOUT)))
        timeout = CO_U128(COROUTINE_TIMEOUT);
#if COROUTINE_EPOLL
    struct epoll_event evs[32];
    int got = epoll_wait(set->epoll, evs, 32, co_u128_div(timeout, 1000000ul).L);
    if (got < 0)
        return errno == EINTR ? 0 : -1;
    for (size_t i = 0; i < (size_t)got; i++) {
        struct co_event_fd_sub *c = (struct co_event_fd_sub*)evs[i].data.ptr;
        if (evs[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP))
            co_event_emit(&c->cbs[0]);
        if (evs[i].events & (EPOLLOUT | EPOLLERR | EPOLLHUP))
            co_event_emit(&c->cbs[1]);
        if (c->cbs[0].function == NULL && c->cbs[1].function == NULL)
            co_event_fd_close(set, c);
    }
#else
    fd_set fds[2];
    FD_ZERO(&fds[0]);
    FD_ZERO(&fds[1]);
    int max_fd = 0;
    for (size_t i = 0; i < sizeof(set->fds) / sizeof(set->fds[0]); i++) {
        for (struct co_event_fd_sub *c = set->fds[i], *next = NULL; c; c = next) {
            next = c->link;
            if (c->cbs[0].function == NULL && c->cbs[1].function == NULL) {
                co_event_fd_close(set, c);
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
    for (size_t i = 0; i < sizeof(set->fds) / sizeof(set->fds[0]); i++)
        for (struct co_event_fd_sub *c = set->fds[i]; c; c = c->link)
            for (int i = 0; i < 2; i++)
                if (FD_ISSET(c->fd, &fds[i]))
                    co_event_emit(&c->cbs[i]);
#endif
    return 0;
}
