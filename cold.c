#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE // accept4, recvmmsg, sendmmsg
#endif

#include "cone.h"
#include "cold.h"

#include <fcntl.h>

#if __APPLE__
#define cold_retryable(e, w) ((e) == EWOULDBLOCK || (e) == EAGAIN || ((w) && errno == EPROTOTYPE))
#else
#define cold_retryable(e, w) ((e) == EWOULDBLOCK || (e) == EAGAIN)
#endif

#define cold_iocall(fd, write, f, ...) {     \
    __typeof__(f(__VA_ARGS__)) __r;          \
    while ((__r = f(__VA_ARGS__)) < 0        \
     && cold_retryable(errno, write)         \
     && cone && !cone_iowait(fd, write)) {}  \
    return __r;                              \
}

int cold_unblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    return flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) MUN_RETHROW_OS;
}

ssize_t cold_read(int fd, void *buf, size_t count)
    cold_iocall(fd, 0, read, fd, buf, count)

ssize_t cold_pread(int fd, void *buf, size_t count, off_t offset)
    cold_iocall(fd, 0, pread, fd, buf, count, offset)

ssize_t cold_readv(int fd, const struct iovec *iov, int iovcnt)
    cold_iocall(fd, 0, readv, fd, iov, iovcnt)

ssize_t cold_recv(int fd, void *buf, size_t len, int flags)
    cold_iocall(fd, 0, recv, fd, buf, len, flags)

ssize_t cold_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
    cold_iocall(fd, 0, recvfrom, fd, buf, len, flags, src_addr, addrlen)

ssize_t cold_recvmsg(int fd, struct msghdr *msg, int flags)
    cold_iocall(fd, 0, recvmsg, fd, msg, flags)

ssize_t cold_write(int fd, const void *buf, size_t count)
    cold_iocall(fd, 1, write, fd, buf, count)

ssize_t cold_pwrite(int fd, const void *buf, size_t count, off_t offset)
    cold_iocall(fd, 1, pwrite, fd, buf, count, offset)

ssize_t cold_writev(int fd, const struct iovec *iov, int iovcnt)
    cold_iocall(fd, 1, writev, fd, iov, iovcnt)

ssize_t cold_send(int fd, const void *buf, size_t len, int flags)
    cold_iocall(fd, 1, send, fd, buf, len, flags)

ssize_t cold_sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
    cold_iocall(fd, 1, sendto, fd, buf, len, flags, dest_addr, addrlen)

ssize_t cold_sendmsg(int fd, const struct msghdr *msg, int flags)
    cold_iocall(fd, 1, sendmsg, fd, msg, flags)

int cold_listen(int fd, int backlog) {
    return cone && cold_unblock(fd) ? -1 : listen(fd, backlog);
}

int cold_connect(int fd, const struct sockaddr *addr, socklen_t addrlen) {
    if (cone && cold_unblock(fd))
        return -1;
    int result = connect(fd, addr, addrlen);
    if (result >= 0 || errno != EINPROGRESS || !cone)
        return result;
    if (cone_iowait(fd, 1) || getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &(socklen_t){sizeof(int)}) < 0)
        return -1;
    if (result == 0)
        return 0;
    errno = result;
    return -1;
}

#ifdef __linux__
int cold_accept4(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags)
    cold_iocall(fd, 0, accept4, fd, addr, addrlen, (cone ? SOCK_NONBLOCK : 0) | flags)

int cold_accept(int fd, struct sockaddr *addr, socklen_t *addrlen)
    cold_iocall(fd, 0, accept4, fd, addr, addrlen, (cone ? SOCK_NONBLOCK : 0))

int cold_recvmmsg(int fd, struct mmsghdr *msgvec, unsigned vlen, int flags, struct timespec *timeout)
    cold_iocall(fd, 0, recvmmsg, fd, msgvec, vlen, flags, timeout)

int cold_sendmmsg(int fd, struct mmsghdr *msgvec, unsigned vlen, int flags)
    cold_iocall(fd, 1, sendmmsg, fd, msgvec, vlen, flags)
#else
static int accept_impl(int fd, struct sockaddr *addr, socklen_t *addrlen)
    cold_iocall(fd, 0, accept, fd, addr, addrlen)

int cold_accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    int client = accept_impl(fd, addr, addrlen);
    if (client >= 0 && cold_unblock(client))
        return close(client), -1;
    return client;
}
#endif
