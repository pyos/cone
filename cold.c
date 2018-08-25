// Optional unit that shadows some standard library symbols. Originals
// are loaded with `dlsym`; dynamic linking is required for this to work.
#include "cone.h"
#include <time.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>

#ifndef RTLD_NEXT
#define RTLD_NEXT (void*)(uintptr_t)-1  // avoid requiring _GNU_SOURCE; unclear if RTLD_NEXT is POSIX-standard
#endif

#define cold_def(f) \
    static _Thread_local __typeof__(f) *libc_##f = NULL; \
    if (!libc_##f) libc_##f = dlsym(RTLD_NEXT, #f);

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

// `fd` is automatically switched to non-blocking mode.
int listen(int fd, int backlog) {
    cold_def(listen);
    return cone && cone_unblock(fd) ? -1 : libc_listen(fd, backlog);
}

#ifdef __linux__
// SOCK_NONBLOCK is always on.
int accept4(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags)
    cold_iocall(fd, 0, accept4, fd, addr, addrlen, cone ? flags | SOCK_NONBLOCK : flags)

int accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    return accept4(fd, addr, addrlen, 0);
}
#else
static int cold_accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
    cold_iocall(fd, 0, accept, fd, addr, addrlen)

// Returned socket is non-blocking. Error codes of `fcntl` apply.
int accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    int client = cold_accept(fd, addr, addrlen);
    if (client >= 0 && cone_unblock(client))
        return close(client), -1;
    return client;
}
#endif

// Coroutine-blocking version. The socket is automatically switched into non-blocking mode.
int connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    if (cone && cone_unblock(fd))
        return -1;
    cold_def(connect);
    int result = libc_connect(fd, addr, addrlen);
    if (result >= 0 || errno != EINPROGRESS || !cone)
        return result;
    socklen_t optlen = sizeof(int);
    if (cone_iowait(fd, 1) || getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &optlen) < 0)
        return -1;
    if (result == 0)
        return 0;
    errno = result;
    return -1;
}

// Coroutine-blocking versions.
ssize_t read(int fd, void *buf, size_t count)
    cold_iocall(fd, 0, read, fd, buf, count)

ssize_t pread(int fd, void *buf, size_t count, off_t offset)
    cold_iocall(fd, 0, pread, fd, buf, count, offset)

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
    cold_iocall(fd, 0, readv, fd, iov, iovcnt)

ssize_t recv(int fd, void *buf, size_t len, int flags)
    cold_iocall(fd, 0, recv, fd, buf, len, flags)

ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
    cold_iocall(fd, 0, recvfrom, fd, buf, len, flags, src_addr, addrlen)

ssize_t recvmsg(int fd, struct msghdr *msg, int flags)
    cold_iocall(fd, 0, recvmsg, fd, msg, flags)

ssize_t write(int fd, const void *buf, size_t count)
    cold_iocall(fd, 1, write, fd, buf, count)

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
    cold_iocall(fd, 1, pwrite, fd, buf, count, offset)

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
    cold_iocall(fd, 1, writev, fd, iov, iovcnt)

ssize_t send(int fd, const void *buf, size_t len, int flags)
    cold_iocall(fd, 1, send, fd, buf, len, flags)

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
    cold_iocall(fd, 1, sendto, fd, buf, len, flags, dest_addr, addrlen)

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
    cold_iocall(fd, 1, sendmsg, fd, msg, flags)

// Coroutine-blocking; does not interact with signals (but can be interrupted by
// `cone_cancel`); always returns the argument on error, even if slept for some time.
unsigned sleep(unsigned sec) {
    cold_def(sleep);
    return cone ? cone_sleep((mun_usec)sec * 1000000ul) ? sec : 0 : libc_sleep(sec);
}

// Coroutine-blocking; does not interact with signals; does not fill `rem`.
int nanosleep(const struct timespec *req, struct timespec *rem) {
    cold_def(nanosleep);
    return cone ? cone_sleep((mun_usec)req->tv_sec*1000000ull + req->tv_nsec/1000) : libc_nanosleep(req, rem);
}
