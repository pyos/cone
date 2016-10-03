#pragma once
#include "ev.h"
#include "ev-sched.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#if !defined(CONE_EPOLL) && __linux__
#define CONE_EPOLL 1
#endif

#ifndef CONE_IO_TIMEOUT
#define CONE_IO_TIMEOUT 30000000000ull
#endif

#if CONE_EPOLL
#include <sys/epoll.h>
#else
#include <sys/select.h>
#endif

struct cone_event_fd_sub
{
    int fd;
    struct cone_closure cbs[2];
    struct cone_event_fd_sub *link;
};

struct cone_event_fd
{
    int epoll;
    struct cone_event_fd_sub *fds[127];
};

static inline int
cone_event_fd_init(struct cone_event_fd *set) {
#if CONE_EPOLL
    return (set->epoll = epoll_create1(0)) < 0 ? -1 : 0;
#else
    set->epoll = -1;
    return 0;
#endif
}

static inline void
cone_event_fd_fini(struct cone_event_fd *set) {
    for (size_t i = 0; i < sizeof(set->fds) / sizeof(set->fds[0]); i++)
        for (struct cone_event_fd_sub *c; (c = set->fds[i]) != NULL; free(c))
            set->fds[i] = c->link;
    if (set->epoll >= 0)
        close(set->epoll);
}

static inline struct cone_event_fd_sub **
cone_event_fd_bucket(struct cone_event_fd *set, int fd) {
    struct cone_event_fd_sub **b = &set->fds[fd % (sizeof(set->fds) / sizeof(set->fds[0]))];
    while (*b && (*b)->fd != fd) b = &(*b)->link;
    return b;
}

static inline struct cone_event_fd_sub *
cone_event_fd_open(struct cone_event_fd *set, int fd) {
    struct cone_event_fd_sub **b = cone_event_fd_bucket(set, fd);
    if (!*b && (*b = (struct cone_event_fd_sub *) calloc(1, sizeof(struct cone_event_fd_sub)))) {
        (*b)->fd = fd;
    #if CONE_EPOLL
        struct epoll_event params = {EPOLLRDHUP|EPOLLHUP|EPOLLET|EPOLLIN|EPOLLOUT, {.ptr = *b}};
        if (epoll_ctl(set->epoll, EPOLL_CTL_ADD, fd, &params))
            free(*b), *b = NULL;
    #endif
    }
    return *b;
}

static inline void
cone_event_fd_close(struct cone_event_fd *set, struct cone_event_fd_sub *ev) {
#if CONE_EPOLL
    epoll_ctl(set->epoll, EPOLL_CTL_DEL, ev->fd, NULL);
#endif
    *cone_event_fd_bucket(set, ev->fd) = ev->link;
    free(ev);
}

static inline int
cone_event_fd_connect(struct cone_event_fd *set, int fd, int write, struct cone_closure cb) {
    struct cone_event_fd_sub *ev = cone_event_fd_open(set, fd);
    if (ev == NULL || ev->cbs[write].code)
        return -1;
    ev->cbs[write] = cb;
    return 0;
}

static inline int
cone_event_fd_emit(struct cone_event_fd *set, struct cone_nsec timeout) {
    if (cone_u128_eq(timeout, CONE_U128(0)))
        return -1;
    if (cone_u128_gt(timeout, CONE_U128(CONE_IO_TIMEOUT)))
        timeout = CONE_U128(CONE_IO_TIMEOUT);
#if CONE_EPOLL
    struct epoll_event evs[32];
    int got = epoll_wait(set->epoll, evs, 32, cone_u128_div(timeout, 1000000ul).L);
    if (got < 0)
        return errno == EINTR ? 0 : -1;
    for (size_t i = 0; i < (size_t)got; i++) {
        struct cone_event_fd_sub *c = (struct cone_event_fd_sub*)evs[i].data.ptr;
        if ((evs[i].events & (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP)) && cone_event_emit(&c->cbs[0]))
            return -1;  // TODO not fail
        if ((evs[i].events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) && cone_event_emit(&c->cbs[1]))
            return -1;  // TODO not fail
        if (c->cbs[0].code == NULL && c->cbs[1].code == NULL)
            cone_event_fd_close(set, c);
    }
#else
    fd_set fds[2] = {};
    int max_fd = 0;
    for (size_t i = 0; i < sizeof(set->fds) / sizeof(set->fds[0]); i++) {
        for (struct cone_event_fd_sub *c = set->fds[i], *next = NULL; c; c = next) {
            next = c->link;
            if (c->cbs[0].code == NULL && c->cbs[1].code == NULL) {
                cone_event_fd_close(set, c);
                continue;
            }
            if (max_fd <= c->fd)
                max_fd = c->fd + 1;
            for (int i = 0; i < 2; i++)
                if (c->cbs[i].code)
                    FD_SET(c->fd, &fds[i]);
        }
    }
    struct timeval us = {cone_u128_div(timeout, 1000000000ull).L, timeout.L % 1000000000ull / 1000};
    if (select(max_fd, &fds[0], &fds[1], NULL, &us) < 0)
        return errno == EINTR ? 0 : -1;
    for (size_t i = 0; i < sizeof(set->fds) / sizeof(set->fds[0]); i++)
        for (struct cone_event_fd_sub *c = set->fds[i]; c; c = c->link)
            for (int i = 0; i < 2; i++)
                if (FD_ISSET(c->fd, &fds[i]) && cone_event_emit(&c->cbs[i]))
                    return -1;  // TODO not fail
#endif
    return 0;
}
