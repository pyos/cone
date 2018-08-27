#pragma once

#include <sys/socket.h> // for socklen_t

#if __cplusplus
extern "C" {
#endif

struct iovec;
struct msghdr;

#if COLD_NO_OVERRIDE
#define cold_fwd(ret, f, typed, untyped) ret cold_##f typed;
#else
#define cold_fwd(ret, f, typed, untyped) ret f typed; static inline ret cold_##f typed { return f untyped; }
#endif

// `fd` is automatically switched to non-blocking mode:
cold_fwd(int, listen, (int fd, int backlog), (fd, backlog))
// Coroutine-blocking; `fd` is automatically switched to non-blocking mode:
cold_fwd(int, connect, (int fd, const struct sockaddr *addr, socklen_t addrlen), (fd, addr, addrlen))
// Returned socket is non-blocking:
cold_fwd(int, accept, (int fd, struct sockaddr *addr, socklen_t *addrlen), (fd, addr, addrlen))
#ifdef __linux__
cold_fwd(int, accept4, (int fd, struct sockaddr *addr, socklen_t *addrlen, int flags), (fd, addr, addrlen, flags))
#endif
// Coroutine-blocking versions:
cold_fwd(ssize_t, read, (int fd, void *buf, size_t count), (fd, buf, count))
cold_fwd(ssize_t, pread, (int fd, void *buf, size_t count, off_t offset), (fd, buf, count, offset))
cold_fwd(ssize_t, readv, (int fd, const struct iovec *iov, int iovcnt), (fd, iov, iovcnt))
cold_fwd(ssize_t, recv, (int fd, void *buf, size_t len, int flags), (fd, buf, len, flags))
cold_fwd(ssize_t, recvfrom, (int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen), (fd, buf, len, flags, src_addr, addrlen))
cold_fwd(ssize_t, recvmsg, (int fd, struct msghdr *msg, int flags), (fd, msg, flags))
cold_fwd(ssize_t, write, (int fd, const void *buf, size_t count), (fd, buf, count))
cold_fwd(ssize_t, pwrite, (int fd, const void *buf, size_t count, off_t offset), (fd, buf, count, offset))
cold_fwd(ssize_t, writev, (int fd, const struct iovec *iov, int iovcnt), (fd, iov, iovcnt))
cold_fwd(ssize_t, send, (int fd, const void *buf, size_t len, int flags), (fd, buf, len, flags))
cold_fwd(ssize_t, sendto, (int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen), (fd, buf, len, flags, dest_addr, addrlen))
cold_fwd(ssize_t, sendmsg, (int fd, const struct msghdr *msg, int flags), (fd, msg, flags))

#undef cold_fwd

#if __cplusplus
}
#endif
