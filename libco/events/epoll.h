#pragma once
#include "../generic/callback.h"

#include <unistd.h>
#include <sys/epoll.h>

struct co_fd_duplex;

struct co_event_fd
{
    struct co_callback cb;
    struct co_fd_duplex *parent;
};

struct co_fd_duplex
{
    int fd;
    int epoll;
    struct co_event_fd read;
    struct co_event_fd write;
    struct epoll_event params;
    struct co_fd_duplex *link;
};

struct co_fd_set
{
    int epoll;
    struct co_fd_duplex *fds[COROUTINE_FD_BUCKETS];
};

static inline int
co_event_fd_connect(struct co_event_fd *ev, struct co_callback cb) {
    if (ev->cb.function)
        return -1;
    ev->cb = cb;
    ev->parent->params.events |= ev == &ev->parent->read ? EPOLLIN : EPOLLOUT;
    return epoll_ctl(ev->parent->epoll, EPOLL_CTL_MOD, ev->parent->fd, &ev->parent->params);
}

static inline int
co_event_fd_disconnect(struct co_event_fd *ev, struct co_callback cb) {
    if (ev->cb.function != cb.function || ev->cb.data != cb.data)
        return -1;
    ev->cb = (struct co_callback){};
    ev->parent->params.events &= ~(ev == &ev->parent->read ? EPOLLIN : EPOLLOUT);
    return epoll_ctl(ev->parent->epoll, EPOLL_CTL_MOD, ev->parent->fd, &ev->parent->params);
}

static inline int
co_event_fd_emit(struct co_event_fd *ev) {
    struct co_callback cb = ev->cb;
    ev->cb = (struct co_callback){};  // fd already unregistered due to EPOLLONESHOT
    return cb.function && cb.function(cb.data);
}

static inline struct co_fd_duplex *
co_fd_duplex_alloc(struct co_fd_set *set, int fd) {
    struct co_fd_duplex *x = (struct co_fd_duplex *)malloc(sizeof(struct co_fd_duplex));
    if (x != NULL) {
        x->fd     = fd;
        x->epoll  = set->epoll;
        x->read   = (struct co_event_fd){.parent = x};
        x->write  = (struct co_event_fd){.parent = x};
        x->params = (struct epoll_event){EPOLLRDHUP | EPOLLHUP | EPOLLET | EPOLLONESHOT, {.ptr = x}};
        x->link   = set->fds[fd % COROUTINE_FD_BUCKETS];
        epoll_ctl(set->epoll, EPOLL_CTL_ADD, fd, &x->params);
        set->fds[fd % COROUTINE_FD_BUCKETS] = x;
    }
    return x;
}

static inline void
co_fd_duplex_dealloc(struct co_fd_set *set, struct co_fd_duplex *ev) {
    epoll_ctl(ev->epoll, EPOLL_CTL_DEL, ev->fd, NULL);
    struct co_fd_duplex **b = &set->fds[ev->fd % COROUTINE_FD_BUCKETS];
    while (*b != ev) b = &(*b)->link;
    *b = ev->link;
    free(ev);
}

static inline struct co_fd_duplex *
co_fd_duplex_find(const struct co_fd_set *set, int fd){
    struct co_fd_duplex *ev = set->fds[fd % COROUTINE_FD_BUCKETS];
    while (ev && ev->fd != fd) ev = ev->link;
    return ev;
}

static inline struct co_fd_duplex *
co_fd_duplex(struct co_fd_set *set, int fd) {
    struct co_fd_duplex *ev = co_fd_duplex_find(set, fd);
    return ev != NULL ? ev : co_fd_duplex_alloc(set, fd);
}

static inline void
co_fd_set_init(struct co_fd_set *set) {
    *set = (struct co_fd_set){epoll_create1(0), {}};
}

static inline void
co_fd_set_fini(struct co_fd_set *set) {
    if (set->epoll < 0)
        return;
    for (int i = 0; i < COROUTINE_FD_BUCKETS; i++)
        while (set->fds[i])
            co_fd_duplex_dealloc(set, set->fds[i]);
    close(set->epoll);
}

static inline int
co_fd_set_emit(struct co_fd_set *set, struct co_nsec_offset timeout) {
    struct epoll_event evs[32];
    int ms = (int)co_u128_div(timeout, 1000000ul).L;
    int got = epoll_wait(set->epoll, evs, sizeof(evs) / sizeof(evs[0]), ms == 0 ? -1 : ms);
    if (got > 0) {
        for (struct epoll_event *ev = evs; ev != evs + got; ev++) {
            struct co_fd_duplex *c = (struct co_fd_duplex*)ev->data.ptr;
            if (ev->events & (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP))
                co_event_fd_emit(&c->read);
            if (ev->events & (EPOLLOUT | EPOLLERR | EPOLLHUP))
                co_event_fd_emit(&c->write);
            if (c->read.cb.function == NULL && c->write.cb.function == NULL)
                co_fd_duplex_dealloc(set, c);
        }
    }
    return got < 0 ? -1 : 0;
}
