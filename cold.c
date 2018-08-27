#include "cone.h"

#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>

#if COLD_NO_OVERRIDE
#define cold_fcn(f) cold_##f
#define cold_def(f) static __typeof__(f) *const libc_##f = &f
#else
#include <dlfcn.h>

#ifndef RTLD_NEXT
#define RTLD_NEXT (void*)(uintptr_t)-1  // avoid requiring _GNU_SOURCE; unclear if RTLD_NEXT is POSIX-standard
#endif

#define cold_fcn(f) f
#define cold_def(f) \
    static _Thread_local __typeof__(f) *libc_##f = NULL; \
    mun_assert(libc_##f || (libc_##f = dlsym(RTLD_NEXT, #f)), #f " not found")
#endif

#if __APPLE__
#define cold_retryable(e, w) ((e) == EWOULDBLOCK || (e) == EAGAIN || ((w) && errno == EPROTOTYPE))
#else
#define cold_retryable(e, w) ((e) == EWOULDBLOCK || (e) == EAGAIN)
#endif

#define cold_iocall(fd, write, f, ...) {     \
    cold_def(f);                             \
    __typeof__(f(__VA_ARGS__)) __r;          \
    while ((__r = libc_##f(__VA_ARGS__)) < 0 \
     && cold_retryable(errno, write)         \
     && cone && !cone_iowait(fd, write)) {}  \
    return __r;                              \
}

int cold_fcn(listen)(int fd, int backlog) {
    cold_def(listen);
    return cone && cone_unblock(fd) ? -1 : libc_listen(fd, backlog);
}

#ifdef __linux__
int cold_fcn(accept4)(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags)
    cold_iocall(fd, 0, accept4, fd, addr, addrlen, cone ? flags | SOCK_NONBLOCK : flags)

int cold_fcn(accept)(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    return accept4(fd, addr, addrlen, 0);
}
#else
static int accept_impl(int fd, struct sockaddr *addr, socklen_t *addrlen)
    cold_iocall(fd, 0, accept, fd, addr, addrlen)

int cold_fcn(accept)(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    int client = accept_impl(fd, addr, addrlen);
    if (client >= 0 && cone_unblock(client))
        return close(client), -1;
    return client;
}
#endif

int cold_fcn(connect)(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    if (cone && cone_unblock(fd))
        return -1;
    cold_def(connect);
    int result = libc_connect(fd, addr, addrlen);
    if (result >= 0 || errno != EINPROGRESS || !cone)
        return result;
    if (cone_iowait(fd, 1) || getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &(socklen_t){sizeof(int)}) < 0)
        return -1;
    if (result == 0)
        return 0;
    errno = result;
    return -1;
}

ssize_t cold_fcn(read)(int fd, void *buf, size_t count)
    cold_iocall(fd, 0, read, fd, buf, count)

ssize_t cold_fcn(pread)(int fd, void *buf, size_t count, off_t offset)
    cold_iocall(fd, 0, pread, fd, buf, count, offset)

#if __linux__ && !COLD_NO_OVERRIDE
ssize_t __read_chk(int fd, void *buf, size_t count, size_t buflen)
    cold_iocall(fd, 0, __read_chk, fd, buf, count, buflen)

ssize_t __pread_chk(int fd, void *buf, size_t count, off_t offset, size_t buflen)
    cold_iocall(fd, 0, __pread_chk, fd, buf, count, offset, buflen)
#endif

ssize_t cold_fcn(readv)(int fd, const struct iovec *iov, int iovcnt)
    cold_iocall(fd, 0, readv, fd, iov, iovcnt)

ssize_t cold_fcn(recv)(int fd, void *buf, size_t len, int flags)
    cold_iocall(fd, 0, recv, fd, buf, len, flags)

ssize_t cold_fcn(recvfrom)(int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
    cold_iocall(fd, 0, recvfrom, fd, buf, len, flags, src_addr, addrlen)

ssize_t cold_fcn(recvmsg)(int fd, struct msghdr *msg, int flags)
    cold_iocall(fd, 0, recvmsg, fd, msg, flags)

ssize_t cold_fcn(write)(int fd, const void *buf, size_t count)
    cold_iocall(fd, 1, write, fd, buf, count)

ssize_t cold_fcn(pwrite)(int fd, const void *buf, size_t count, off_t offset)
    cold_iocall(fd, 1, pwrite, fd, buf, count, offset)

ssize_t cold_fcn(writev)(int fd, const struct iovec *iov, int iovcnt)
    cold_iocall(fd, 1, writev, fd, iov, iovcnt)

ssize_t cold_fcn(send)(int fd, const void *buf, size_t len, int flags)
    cold_iocall(fd, 1, send, fd, buf, len, flags)

ssize_t cold_fcn(sendto)(int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
    cold_iocall(fd, 1, sendto, fd, buf, len, flags, dest_addr, addrlen)

ssize_t cold_fcn(sendmsg)(int fd, const struct msghdr *msg, int flags)
    cold_iocall(fd, 1, sendmsg, fd, msg, flags)

#if !COLD_NO_OVERRIDE
unsigned sleep(unsigned sec) {
    cold_def(sleep);
    // FIXME should return the difference if woken due to error
    return cone ? cone_sleep((mun_usec)sec * 1000000ul) ? sec : 0 : libc_sleep(sec);
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    cold_def(nanosleep);
    // FIXME should fill `rem` if woken due to error
    return cone ? cone_sleep((mun_usec)req->tv_sec*1000000ull + req->tv_nsec/1000) : libc_nanosleep(req, rem);
}
#endif
