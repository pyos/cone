// Optional unit that shadows some standard library symbols. Originals
// are loaded with `dlsym`; dynamic linking is required for this to work.
#include "cone.h"
#include <time.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/socket.h>

#ifndef RTLD_NEXT
#define RTLD_NEXT (void*)(uintptr_t)-1  // avoid requiring _GNU_SOURCE; unclear if RTLD_NEXT is POSIX-standard
#endif

#define cold_def(ret, name, ...) \
    typedef ret (*typeof_##name)(__VA_ARGS__);    \
    static typeof_##name libc_##name() {          \
        static _Thread_local typeof_##name v = 0; \
        if (!v) v = dlsym(RTLD_NEXT, #name);      \
        return v;                                 \
    };                                            \
    ret name(__VA_ARGS__)

#define cold_iowait(fd, write, expr) \
    do if ((expr) >= 0 || (errno != EWOULDBLOCK && errno != EAGAIN)) break; while (cone && !cone_iowait(fd, write));

#define cold_iocall(fd, write, expr) \
    { __typeof__(expr) __r; cold_iowait(fd, write, __r = expr); return __r; }

// `fd` is automatically switched to non-blocking mode.
cold_def(int, listen, int fd, int backlog) {
    return cone && cone_unblock(fd) ? -1 : libc_listen()(fd, backlog);
}

#ifdef __linux__
// SOCK_NONBLOCK is always on.
cold_def(int, accept4, int fd, struct sockaddr *addr, socklen_t *addrlen, int flags)
    cold_iocall(fd, 0, libc_accept4()(fd, addr, addrlen, cone ? flags | SOCK_NONBLOCK : flags))
#endif

// Returned socket is non-blocking. Error codes of `fcntl` apply.
#ifdef __linux__
int accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    return accept4(fd, addr, addrlen, 0);
}
#else
cold_def(int, accept, int fd, struct sockaddr *addr, socklen_t *addrlen) {
    int client; cold_iowait(fd, 0, client = libc_accept()(fd, addr, addrlen));
    if (client >= 0 && cone_unblock(client))
        return close(client), -1;
    return client;
}
#endif

// Coroutine-blocking version. The socket is automatically switched into non-blocking mode.
cold_def(int, connect, int fd, const struct sockaddr *addr, socklen_t addrlen) {
    if (cone && cone_unblock(fd))
        return -1;
    int result = libc_connect()(fd, addr, addrlen);
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
cold_def(ssize_t, read, int fd, void *buf, size_t count)
    cold_iocall(fd, 0, libc_read()(fd, buf, count))

cold_def(ssize_t, recv, int fd, void *buf, size_t len, int flags)
    cold_iocall(fd, 0, libc_recv()(fd, buf, len, flags))

cold_def(ssize_t, recvfrom, int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
    cold_iocall(fd, 0, libc_recvfrom()(fd, buf, len, flags, src_addr, addrlen))

cold_def(ssize_t, recvmsg, int fd, struct msghdr *msg, int flags)
    cold_iocall(fd, 0, libc_recvmsg()(fd, msg, flags))

cold_def(ssize_t, write, int fd, const void *buf, size_t count)
    cold_iocall(fd, 1, libc_write()(fd, buf, count))

cold_def(ssize_t, send, int fd, const void *buf, size_t len, int flags)
    cold_iocall(fd, 1, libc_send()(fd, buf, len, flags))

cold_def(ssize_t, sendto, int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
    cold_iocall(fd, 1, libc_sendto()(fd, buf, len, flags, dest_addr, addrlen))

cold_def(ssize_t, sendmsg, int fd, const struct msghdr *msg, int flags)
    cold_iocall(fd, 1, libc_sendmsg()(fd, msg, flags))

// Coroutine-blocking; does not interact with signals (but can be interrupted by
// `cone_cancel`); always returns the argument on error, even if slept for some time.
cold_def(unsigned, sleep, unsigned sec) {
    return cone ? cone_sleep((mun_usec)sec * 1000000ul) ? sec : 0 : libc_sleep()(sec);
}

// Coroutine-blocking; does not interact with signals; does not fill `rem`.
cold_def(int, nanosleep, const struct timespec *req, struct timespec *rem) {
    return cone ? cone_sleep((mun_usec)req->tv_sec*1000000ull + req->tv_nsec/1000) : libc_nanosleep()(req, rem);
}
