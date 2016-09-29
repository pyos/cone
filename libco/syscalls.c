#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "coro.h"

#include <time.h>
#include <errno.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

static int __co_libc_initialized = 0;
static int      (*__co_libc_listen)      (int, int);
static int      (*__co_libc_accept)      (int, struct sockaddr *, socklen_t *);
static int      (*__co_libc_accept4)     (int, struct sockaddr *, socklen_t *, int);
static ssize_t  (*__co_libc_read)        (int, void *, size_t);
static ssize_t  (*__co_libc_write)       (int, const void *, size_t);
static ssize_t  (*__co_libc_recv)        (int, void *, size_t, int);
static ssize_t  (*__co_libc_recvfrom)    (int, void *, size_t, int, struct sockaddr *, socklen_t *);
static ssize_t  (*__co_libc_recvmsg)     (int, struct msghdr *, int);
static ssize_t  (*__co_libc_send)        (int, const void *, size_t, int);
static ssize_t  (*__co_libc_sendto)      (int, const void *, size_t, int, const struct sockaddr *, socklen_t);
static ssize_t  (*__co_libc_sendmsg)     (int, const struct msghdr *, int);
static unsigned (*__co_libc_sleep)       (unsigned);
static int      (*__co_libc_usleep)      (useconds_t);
static int      (*__co_libc_nanosleep)   (const struct timespec *req, struct timespec *rem);
#ifdef _POSIX_PRIORITY_SCHEDULING
static int      (*__co_libc_sched_yield) ();
#endif

static void
__co_libc_init(void) {
    if (__co_libc_initialized)
        return;
    __co_libc_initialized = 1;
    __co_libc_listen      = dlsym(RTLD_NEXT, "listen");
    __co_libc_accept      = dlsym(RTLD_NEXT, "accept");
    __co_libc_accept4     = dlsym(RTLD_NEXT, "accept4");
    __co_libc_read        = dlsym(RTLD_NEXT, "read");
    __co_libc_write       = dlsym(RTLD_NEXT, "write");
    __co_libc_recv        = dlsym(RTLD_NEXT, "recv");
    __co_libc_recvfrom    = dlsym(RTLD_NEXT, "recvfrom");
    __co_libc_recvmsg     = dlsym(RTLD_NEXT, "recvmsg");
    __co_libc_send        = dlsym(RTLD_NEXT, "send");
    __co_libc_sendto      = dlsym(RTLD_NEXT, "sendto");
    __co_libc_sendmsg     = dlsym(RTLD_NEXT, "sendmsg");
    __co_libc_sleep       = dlsym(RTLD_NEXT, "sleep");
    __co_libc_usleep      = dlsym(RTLD_NEXT, "usleep");
    __co_libc_nanosleep   = dlsym(RTLD_NEXT, "nanosleep");
    #ifdef _POSIX_PRIORITY_SCHEDULING
    __co_libc_sched_yield = dlsym(RTLD_NEXT, "sched_yield");
    #endif
}


#define LOOP(mode, f, fd, ...)                                           \
    do {                                                                 \
        __co_libc_init();                                                \
        struct coro *c = coro_current;                                   \
        if (!c)                                                          \
            return f(fd, ##__VA_ARGS__);                                 \
        __typeof__(f(fd, ##__VA_ARGS__)) r;                              \
        r = (__typeof__(r))-1;                                           \
        while ((r = f(fd, ##__VA_ARGS__)) == (__typeof__(r))-1 &&        \
               (errno == EWOULDBLOCK || errno == EAGAIN)) {              \
            struct co_fd_event *ev = co_fd_event(&c->loop->base.io, fd); \
            if (ev == NULL || coro_pause(c, &ev->mode.as_event))         \
                return r;                                                \
        }                                                                \
        return r;                                                        \
    } while (0)


int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    LOOP(read, __co_libc_accept4, sockfd, addr, addrlen, SOCK_NONBLOCK);
}

int listen(int sockfd, int backlog) {
    if (setnonblocking(sockfd))
        return -1;
    __co_libc_init();
    return __co_libc_listen(sockfd, backlog);
}

int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
    LOOP(read, __co_libc_accept4, sockfd, addr, addrlen, flags | SOCK_NONBLOCK);
}

ssize_t read(int fd, void *buf, size_t count) {
    LOOP(read, __co_libc_read, fd, buf, count);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    LOOP(read, __co_libc_recv, sockfd, buf, len, flags);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen) {
    LOOP(read, __co_libc_recvfrom, sockfd, buf, len, flags, src_addr, addrlen);
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
    LOOP(read, __co_libc_recvmsg, sockfd, msg, flags);
}

ssize_t write(int fd, const void *buf, size_t count) {
    LOOP(write, __co_libc_write, fd, buf, count);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    LOOP(write, __co_libc_send, sockfd, buf, len, flags);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen) {
    LOOP(write, __co_libc_sendto, sockfd, buf, len, flags, dest_addr, addrlen);
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    LOOP(write, __co_libc_sendmsg, sockfd, msg, flags);
}

#define SLEEP(f, total, ...)                                                                 \
    do {                                                                                     \
        __co_libc_init();                                                                    \
        struct coro *c = coro_current;                                                       \
        if (c == NULL)                                                                       \
            return f(__VA_ARGS__);                                                           \
        struct co_event_scheduler sc = co_event_schedule_after(&c->loop->base.sched, total); \
        return coro_pause(c, &sc.as_event);                                                  \
    } while (0)

unsigned sleep(unsigned seconds) {
    SLEEP(__co_libc_sleep, co_u128_value((uint64_t)seconds * 1000000000ull), seconds);
}

int usleep(useconds_t usec) {
    SLEEP(__co_libc_usleep, co_u128_value((uint64_t)usec * 1000u), usec);
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    SLEEP(__co_libc_nanosleep, co_nsec_from_timespec(*req), req, rem);
}

int sched_yield(void) {
    struct coro *c = coro_current;
    if (c) {
        if (co_loop_ping(&c->loop->base))
            return -1;
        return coro_pause(c, &c->loop->base.on_ping.as_event);
    }
#ifdef _POSIX_PRIORITY_SCHEDULING
    __co_libc_init();
    return __co_libc_sched_yield();
#else
    return EINVAL;
#endif
}
