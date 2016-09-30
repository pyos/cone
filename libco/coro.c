#include "coro.h"

#include <time.h>
#include <errno.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

int amain(int argc, const char **argv);

struct co_amain_ctx
{
    int ret;
    int argc;
    const char **argv;
};

_Thread_local struct coro * volatile coro_current;

static int      (*co_libc_listen)      (int, int);
static int      (*co_libc_accept)      (int, struct sockaddr *, socklen_t *);
static int      (*co_libc_accept4)     (int, struct sockaddr *, socklen_t *, int);
static ssize_t  (*co_libc_read)        (int, void *, size_t);
static ssize_t  (*co_libc_write)       (int, const void *, size_t);
static ssize_t  (*co_libc_recv)        (int, void *, size_t, int);
static ssize_t  (*co_libc_recvfrom)    (int, void *, size_t, int, struct sockaddr *, socklen_t *);
static ssize_t  (*co_libc_recvmsg)     (int, struct msghdr *, int);
static ssize_t  (*co_libc_send)        (int, const void *, size_t, int);
static ssize_t  (*co_libc_sendto)      (int, const void *, size_t, int, const struct sockaddr *, socklen_t);
static ssize_t  (*co_libc_sendmsg)     (int, const struct msghdr *, int);
static unsigned (*co_libc_sleep)       (unsigned);
static int      (*co_libc_usleep)      (useconds_t);
static int      (*co_libc_nanosleep)   (const struct timespec *req, struct timespec *rem);
#ifdef _POSIX_PRIORITY_SCHEDULING
static int      (*co_libc_sched_yield) ();
#endif

static void
co_libc_init(void) {
    co_libc_listen      = dlsym(RTLD_NEXT, "listen");
    co_libc_accept      = dlsym(RTLD_NEXT, "accept");
    co_libc_accept4     = dlsym(RTLD_NEXT, "accept4");
    co_libc_read        = dlsym(RTLD_NEXT, "read");
    co_libc_write       = dlsym(RTLD_NEXT, "write");
    co_libc_recv        = dlsym(RTLD_NEXT, "recv");
    co_libc_recvfrom    = dlsym(RTLD_NEXT, "recvfrom");
    co_libc_recvmsg     = dlsym(RTLD_NEXT, "recvmsg");
    co_libc_send        = dlsym(RTLD_NEXT, "send");
    co_libc_sendto      = dlsym(RTLD_NEXT, "sendto");
    co_libc_sendmsg     = dlsym(RTLD_NEXT, "sendmsg");
    co_libc_sleep       = dlsym(RTLD_NEXT, "sleep");
    co_libc_usleep      = dlsym(RTLD_NEXT, "usleep");
    co_libc_nanosleep   = dlsym(RTLD_NEXT, "nanosleep");
    #ifdef _POSIX_PRIORITY_SCHEDULING
    co_libc_sched_yield = dlsym(RTLD_NEXT, "sched_yield");
    #endif
}

static int
co_run_amain(struct co_amain_ctx *c) {
    c->ret = amain(c->argc, c->argv);
    return 0;
}

int
main(int argc, const char **argv) {
    co_libc_init();
    struct co_amain_ctx c = {1, argc, argv};
    if (coro_main(co_bind(&co_run_amain, &c)))
        return 1;
    return c.ret;
}

#define LOOP(mode, f, fd, ...)                                             \
    do {                                                                   \
        struct coro *c = coro_current;                                     \
        if (!c)                                                            \
            return f(fd, ##__VA_ARGS__);                                   \
        __typeof__(f(fd, ##__VA_ARGS__)) r;                                \
        r = (__typeof__(r))-1;                                             \
        while ((r = f(fd, ##__VA_ARGS__)) == (__typeof__(r))-1 &&          \
               (errno == EWOULDBLOCK || errno == EAGAIN)) {                \
            struct co_fd_duplex *ev = co_fd_duplex(&c->loop->base.io, fd); \
            if (ev == NULL || coro_pause_fd(c, &ev->mode))                 \
                return r;                                                  \
        }                                                                  \
        return r;                                                          \
    } while (0)

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    LOOP(read, co_libc_accept4, sockfd, addr, addrlen, SOCK_NONBLOCK);
}

int listen(int sockfd, int backlog) {
    return setnonblocking(sockfd) ? -1 : co_libc_listen(sockfd, backlog);
}

int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
    LOOP(read, co_libc_accept4, sockfd, addr, addrlen, flags | SOCK_NONBLOCK);
}

ssize_t read(int fd, void *buf, size_t count) {
    LOOP(read, co_libc_read, fd, buf, count);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    LOOP(read, co_libc_recv, sockfd, buf, len, flags);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen) {
    LOOP(read, co_libc_recvfrom, sockfd, buf, len, flags, src_addr, addrlen);
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
    LOOP(read, co_libc_recvmsg, sockfd, msg, flags);
}

ssize_t write(int fd, const void *buf, size_t count) {
    LOOP(write, co_libc_write, fd, buf, count);
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    LOOP(write, co_libc_send, sockfd, buf, len, flags);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen) {
    LOOP(write, co_libc_sendto, sockfd, buf, len, flags, dest_addr, addrlen);
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    LOOP(write, co_libc_sendmsg, sockfd, msg, flags);
}

#define SLEEP(f, total, ...)                                                                 \
    do {                                                                                     \
        struct coro *c = coro_current;                                                       \
        if (c == NULL)                                                                       \
            return f(__VA_ARGS__);                                                           \
        struct co_event_scheduler sc = co_event_schedule_after(&c->loop->base.sched, total); \
        return coro_pause_scheduler(c, &sc);                                                 \
    } while (0)

unsigned sleep(unsigned seconds) {
    SLEEP(co_libc_sleep, CO_U128((uint64_t)seconds * 1000000000ull), seconds);
}

int usleep(useconds_t usec) {
    SLEEP(co_libc_usleep, CO_U128((uint64_t)usec * 1000u), usec);
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    SLEEP(co_libc_nanosleep, co_nsec_from_timespec(*req), req, rem);
}

int sched_yield(void) {
    struct coro *c = coro_current;
    if (c) {
        if (co_loop_ping(&c->loop->base))
            return -1;
        return coro_pause_vec(c, &c->loop->base.on_ping);
    }
#ifdef _POSIX_PRIORITY_SCHEDULING
    return co_libc_sched_yield();
#else
    return EINVAL;
#endif
}
