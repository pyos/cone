#pragma once
#include "base.h"

#include <unistd.h>
#include <sys/epoll.h>

struct co_fd_event;

struct co_fd_callback
{
    struct co_event as_event;
    struct co_callback cb;
    struct co_fd_event *parent;
};

struct co_fd_event
{
    int fd;
    int epoll;
    struct co_fd_callback read;
    struct co_fd_callback write;
    struct epoll_event params;
    struct co_fd_event *link;
};

struct co_fd_set
{
    int epoll;
    struct co_fd_event *fds[COROUTINE_FD_BUCKETS];
};

static inline int
co_fd_callback_connect(struct co_fd_callback *ev, struct co_callback cb) {
    if (ev->cb.function)
        return -1;
    ev->cb = cb;
    ev->parent->params.events |= ev == &ev->parent->read ? EPOLLIN : EPOLLOUT;
    return epoll_ctl(ev->parent->epoll, EPOLL_CTL_MOD, ev->parent->fd, &ev->parent->params);
}

static inline int
co_fd_callback_disconnect(struct co_fd_callback *ev, struct co_callback cb) {
    if (ev->cb.function != cb.function || ev->cb.data != cb.data)
        return -1;
    ev->cb = (struct co_callback){};
    ev->parent->params.events &= ~(ev == &ev->parent->read ? EPOLLIN : EPOLLOUT);
    return epoll_ctl(ev->parent->epoll, EPOLL_CTL_MOD, ev->parent->fd, &ev->parent->params);
}

static inline int
co_fd_callback_emit(struct co_fd_callback *ev) {
    struct co_callback cb = ev->cb;
    ev->cb = (struct co_callback){};  // fd already unregistered due to EPOLLONESHOT
    return cb.function && cb.function(cb.data);
}

static inline struct co_fd_event *
co_fd_event_alloc(struct co_fd_set *set, int fd) {
    struct co_fd_event *x = (struct co_fd_event *)malloc(sizeof(struct co_fd_event));
    if (x != NULL) {
        struct co_fd_callback half = {.parent = x};
        half.as_event = co_event_impl(&co_fd_callback_connect, &co_fd_callback_disconnect);
        x->fd     = fd;
        x->epoll  = set->epoll;
        x->read   = half;
        x->write  = half;
        x->params = (struct epoll_event){EPOLLRDHUP | EPOLLHUP | EPOLLET | EPOLLONESHOT, {.ptr = x}};
        x->link   = set->fds[fd % COROUTINE_FD_BUCKETS];
        epoll_ctl(set->epoll, EPOLL_CTL_ADD, fd, &x->params);
        set->fds[fd % COROUTINE_FD_BUCKETS] = x;
    }
    return x;
}

static inline void
co_fd_event_dealloc(struct co_fd_set *set, struct co_fd_event *ev) {
    if (ev->read.cb.function != NULL || ev->write.cb.function != NULL)
        return;
    epoll_ctl(ev->epoll, EPOLL_CTL_DEL, ev->fd, NULL);
    struct co_fd_event **b = &set->fds[ev->fd % COROUTINE_FD_BUCKETS];
    while (*b != ev) b = &(*b)->link;
    *b = ev->link;
    free(ev);
}

static inline void
co_fd_event_force_dealloc(struct co_fd_set *set, struct co_fd_event *ev) {
    ev->read.cb = ev->write.cb = (struct co_callback){};
    co_fd_event_dealloc(set, ev);
}

static inline struct co_fd_event *
co_fd_event_find(const struct co_fd_set *set, int fd){
    struct co_fd_event *ev = set->fds[fd % COROUTINE_FD_BUCKETS];
    while (ev && ev->fd != fd) ev = ev->link;
    return ev;
}

static inline struct co_fd_event *
co_fd_event(struct co_fd_set *set, int fd) {
    struct co_fd_event *ev = co_fd_event_find(set, fd);
    return ev != NULL ? ev : co_fd_event_alloc(set, fd);
}

static inline int
co_fd_set_emit(struct co_fd_set *set, struct co_nsec_offset timeout) {
    struct epoll_event evs[32];
    int ms = (int)co_u128_div(timeout, 1000000ul).L;
    int got = epoll_wait(set->epoll, evs, sizeof(evs) / sizeof(evs[0]), ms == 0 ? -1 : ms);
    if (got > 0) {
        for (struct epoll_event *ev = evs; ev != evs + got; ev++) {
            struct co_fd_event *c = (struct co_fd_event*)ev->data.ptr;
            if (ev->events & (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP))
                co_fd_callback_emit(&c->read);
            if (ev->events & (EPOLLOUT | EPOLLERR | EPOLLHUP))
                co_fd_callback_emit(&c->write);
            co_fd_event_dealloc(set, c);
        }
    }
    return got < 0 ? -1 : 0;
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
            co_fd_event_force_dealloc(set, set->fds[i]);
    close(set->epoll);
}
