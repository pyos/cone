//
// cold // cone libc definitions
//
// Optional unit that shadows some standard library symbols. Originals
// are loaded with `dlsym`; dynamic linking is required for this to work.
//
#include "cone.h"
#include <time.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/socket.h>

#define cold_def(ret, name, ...) \
    static ret (*libc_##name)(__VA_ARGS__) = NULL; \
    extern ret name(__VA_ARGS__)

// Yield to the event loop until `expr` stops failing with EWOULDBLOCK/EAGAIN, assuming
// it attempts to read from/write to `fd`; store the result in `r`. Same as `expr` if
// `fd` is blocking.
#define cold_iowait(r, fd, write, expr) \
    do r = expr; while (cone && r < 0 && (errno == EWOULDBLOCK || errno == EAGAIN) && !cone_iowait(fd, write));

#define cold_iocall(fd, write, rettype, expr) \
    { rettype r; cold_iowait(r, fd, write, expr); return r; }

// man listen 2
// `fd` is automatically switched to non-blocking mode.
cold_def(int, listen, int fd, int backlog) {
    return !cone || !cone_unblock(fd) ? libc_listen(fd, backlog) : -1;
}

// man accept 2
// Returned socket is non-blocking. Error codes of `fcntl` apply.
cold_def(int, accept, int fd, struct sockaddr *addr, socklen_t *addrlen) {
    int client; cold_iowait(client, fd, 0, libc_accept(fd, addr, addrlen));
    if (client >= 0 && cone && cone_unblock(client))
        return close(client), -1;
    return client;
}

#ifdef __linux__
// man accept 2
// SOCK_NONBLOCK is always on.
cold_def(int, accept4, int fd, struct sockaddr *addr, socklen_t *addrlen, int flags)
    cold_iocall(fd, 0, int, libc_accept4(fd, addr, addrlen, cone ? flags | SOCK_NONBLOCK : flags))
#endif

// man {read,recv,recvfrom,recvmsg,write,send,sendto,sendmsg} 2
// Coroutine-blocking versions.
cold_def(ssize_t, read, int fd, void *buf, size_t count)
    cold_iocall(fd, 0, ssize_t, libc_read(fd, buf, count))

cold_def(ssize_t, recv, int fd, void *buf, size_t len, int flags)
    cold_iocall(fd, 0, ssize_t, libc_recv(fd, buf, len, flags))

cold_def(ssize_t, recvfrom, int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
    cold_iocall(fd, 0, ssize_t, libc_recvfrom(fd, buf, len, flags, src_addr, addrlen))

cold_def(ssize_t, recvmsg, int fd, struct msghdr *msg, int flags)
    cold_iocall(fd, 0, ssize_t, libc_recvmsg(fd, msg, flags))

cold_def(ssize_t, write, int fd, const void *buf, size_t count)
    cold_iocall(fd, 1, ssize_t, libc_write(fd, buf, count))

cold_def(ssize_t, send, int fd, const void *buf, size_t len, int flags)
    cold_iocall(fd, 1, ssize_t, libc_send(fd, buf, len, flags))

cold_def(ssize_t, sendto, int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
    cold_iocall(fd, 1, ssize_t, libc_sendto(fd, buf, len, flags, dest_addr, addrlen))

cold_def(ssize_t, sendmsg, int fd, const struct msghdr *msg, int flags)
    cold_iocall(fd, 1, ssize_t, libc_sendmsg(fd, msg, flags))

// man sleep 3
// Coroutine-blocking; does not interact with signals (but can be interrupted by
// `cone_cancel`); always returns the argument on error, even if slept for some time.
cold_def(unsigned, sleep, unsigned sec) {
    return !cone ? libc_sleep(sec) : cone_sleep((mun_usec)sec * 1000000ul) ? sec : 0;
}

// man nanosleep 2
// Coroutine-blocking; does not interact with signals; does not fill `rem`.
cold_def(int, nanosleep, const struct timespec *req, struct timespec *rem) {
    return !cone ? libc_nanosleep(req, rem) : cone_sleep((mun_usec)req->tv_sec*1000000ull + req->tv_nsec/1000);
}

static __attribute__((constructor)) void cold_init(void) {
#ifndef RTLD_NEXT
#define RTLD_NEXT (void*)(uintptr_t)-1  // avoid requiring _GNU_SOURCE; unclear if RTLD_NEXT is POSIX-standard
#endif
    libc_listen      = dlsym(RTLD_NEXT, "listen");
    libc_accept      = dlsym(RTLD_NEXT, "accept");
#ifdef __linux__
    libc_accept4     = dlsym(RTLD_NEXT, "accept4");
#endif
    libc_read        = dlsym(RTLD_NEXT, "read");
    libc_recv        = dlsym(RTLD_NEXT, "recv");
    libc_recvfrom    = dlsym(RTLD_NEXT, "recvfrom");
    libc_recvmsg     = dlsym(RTLD_NEXT, "recvmsg");
    libc_write       = dlsym(RTLD_NEXT, "write");
    libc_send        = dlsym(RTLD_NEXT, "send");
    libc_sendto      = dlsym(RTLD_NEXT, "sendto");
    libc_sendmsg     = dlsym(RTLD_NEXT, "sendmsg");
    libc_sleep       = dlsym(RTLD_NEXT, "sleep");
    libc_nanosleep   = dlsym(RTLD_NEXT, "nanosleep");
}
