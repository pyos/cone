#include "coro.h"

#include <time.h>
#include <errno.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/socket.h>

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

struct co_amain_ctx
{
    int ret;
    int argc;
    const char **argv;
};

int amain(int argc, const char **argv);

static int
co_run_amain(struct co_amain_ctx *c) {
    c->ret = amain(c->argc, c->argv);
    return 0;
}

int main(int argc, const char **argv) {
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
    struct co_amain_ctx c = {1, argc, argv};
    return coro_main(co_bind(&co_run_amain, &c)) ? 1 : c.ret;
}

#define FDLOOP(fd, write, rettype, call) do {                                                       \
    rettype r = (rettype)-1;                                                                        \
    while ((r = call) < 0 && (errno == EWOULDBLOCK || errno == EAGAIN) && !coro_iowait(fd, write)); \
    return r;                                                                                       \
} while (0)

int listen(int fd, int backlog) {
    return setnonblocking(fd) ? -1 : co_libc_listen(fd, backlog);
}

int accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    FDLOOP(fd, 0, int, co_libc_accept4(fd, addr, addrlen, SOCK_NONBLOCK));
}

int accept4(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
    FDLOOP(fd, 0, int, co_libc_accept4(fd, addr, addrlen, flags | SOCK_NONBLOCK));
}

ssize_t read(int fd, void *buf, size_t count) {
    FDLOOP(fd, 0, ssize_t, co_libc_read(fd, buf, count));
}

ssize_t recv(int fd, void *buf, size_t len, int flags) {
    FDLOOP(fd, 0, ssize_t, co_libc_recv(fd, buf, len, flags));
}

ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen) {
    FDLOOP(fd, 0, ssize_t, co_libc_recvfrom(fd, buf, len, flags, src_addr, addrlen));
}

ssize_t recvmsg(int fd, struct msghdr *msg, int flags) {
    FDLOOP(fd, 0, ssize_t, co_libc_recvmsg(fd, msg, flags));
}

ssize_t write(int fd, const void *buf, size_t count) {
    FDLOOP(fd, 1, ssize_t, co_libc_write(fd, buf, count));
}

ssize_t send(int fd, const void *buf, size_t len, int flags) {
    FDLOOP(fd, 1, ssize_t, co_libc_send(fd, buf, len, flags));
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen) {
    FDLOOP(fd, 1, ssize_t, co_libc_sendto(fd, buf, len, flags, dest_addr, addrlen));
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags) {
    FDLOOP(fd, 1, ssize_t, co_libc_sendmsg(fd, msg, flags));
}

unsigned sleep(unsigned sec) {
    return coro_current ? coro_sleep(CO_U128((uint64_t)sec * 1000000000ull)) ? sec : 0 : co_libc_sleep(sec);
}

int usleep(useconds_t usec) {
    return coro_current ? coro_sleep(CO_U128((uint64_t)usec * 1000u)) : co_libc_usleep(usec);
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    return coro_current ? coro_sleep(co_nsec_from_timespec(*req)) : co_libc_nanosleep(req, rem);
}

int sched_yield(void) {
    struct coro *c = coro_current;
    return c ? (co_loop_ping(c->loop) ? -1 : coro_wait(&c->loop->on_ping))
#ifdef _POSIX_PRIORITY_SCHEDULING
             : co_libc_sched_yield();
#else
             : EINVAL;
#endif
}
