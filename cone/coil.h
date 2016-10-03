#pragma once
#include "cone.h"

#include <dlfcn.h>
#include <sys/socket.h>
#ifdef _POSIX_PRIORITY_SCHEDULING
#include <sched.h>
#else
#define sched_yield() -1
#endif

#define coil_libc_io(fd, write, rettype, expr) { \
    rettype r;                                   \
    do r = expr; while (r < 0 && (errno == EWOULDBLOCK || errno == EAGAIN) && !cone_iowait(fd, write)); \
    return r;                                    \
}

int coil_listen(int fd, int backlog) {
    return cone && setnonblocking(fd) < 0 ? -1 : listen(fd, backlog);
}

int coil_accept3(int fd, struct sockaddr *addr, socklen_t *addrlen)
    coil_libc_io(fd, 0, int, accept(fd, addr, addrlen))

int coil_accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    return (fd = coil_accept3(fd, addr, addrlen)) < 0 ? -1 : setnonblocking(fd) ? (close(fd), -1) : fd;
}

#ifdef _GNU_SOURCE
int coil_accept4(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags)
    coil_libc_io(fd, 0, int, accept4(fd, addr, addrlen, cone ? flags | SOCK_NONBLOCK : flags))
#endif

ssize_t coil_read(int fd, void *buf, size_t count)
    coil_libc_io(fd, 0, ssize_t, read(fd, buf, count))

ssize_t coil_recv(int fd, void *buf, size_t len, int flags)
    coil_libc_io(fd, 0, ssize_t, recv(fd, buf, len, flags))

ssize_t coil_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
    coil_libc_io(fd, 0, ssize_t, recvfrom(fd, buf, len, flags, src_addr, addrlen))

ssize_t coil_recvmsg(int fd, struct msghdr *msg, int flags)
    coil_libc_io(fd, 0, ssize_t, recvmsg(fd, msg, flags))

ssize_t coil_write(int fd, const void *buf, size_t count)
    coil_libc_io(fd, 1, ssize_t, write(fd, buf, count))

ssize_t coil_send(int fd, const void *buf, size_t len, int flags)
    coil_libc_io(fd, 1, ssize_t, send(fd, buf, len, flags))

ssize_t coil_sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
    coil_libc_io(fd, 1, ssize_t, sendto(fd, buf, len, flags, dest_addr, addrlen))

ssize_t coil_sendmsg(int fd, const struct msghdr *msg, int flags)
    coil_libc_io(fd, 1, ssize_t, sendmsg(fd, msg, flags))

unsigned coil_sleep(unsigned sec) {
    return !cone ? sleep(sec) : cone_sleep(CONE_U128((uint64_t)sec * 1000000000ull)) ? sec : 0;
}

int coil_nanosleep(const struct timespec *req, struct timespec *rem) {
    return !cone ? nanosleep(req, rem) : cone_sleep(cone_nsec_from_timespec(*req));
}

int coil_sched_yield(void) {
    return !cone ? sched_yield() : cone_loop_ping(cone->loop) ? -1 : cone_wait(&cone->loop->on_ping);
}
