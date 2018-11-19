#pragma once

#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>

#if __cplusplus
extern "C" {
#endif

struct iovec;
struct msghdr;

#if COLD_NO_OVERRIDE
#define cold_fwd(ret, f, args) ret cold_##f args
#else
#define cold_fwd(ret, f, args) ret f args; static ret (*const cold_##f) args = &f
#endif

// Switch a file descriptor into non-blocking mode. See fcntl(2) for error codes.
int cold_unblock(int fd);

// `fd` is automatically switched to non-blocking mode:
cold_fwd(int, listen, (int, int));
// Coroutine-blocking; `fd` is automatically switched to non-blocking mode:
cold_fwd(int, connect, (int, const struct sockaddr *, socklen_t));
// Returned socket is non-blocking:
cold_fwd(int, accept, (int, struct sockaddr *, socklen_t *));
#if __linux__
cold_fwd(int, accept4, (int, struct sockaddr *, socklen_t *, int));
#endif
// Coroutine-blocking versions:
cold_fwd(ssize_t, read,     (int, void *, size_t));
cold_fwd(ssize_t, pread,    (int, void *, size_t, off_t));
cold_fwd(ssize_t, readv,    (int, const struct iovec *, int));
cold_fwd(ssize_t, recv,     (int, void *, size_t, int));
cold_fwd(ssize_t, recvfrom, (int, void *, size_t, int, struct sockaddr *, socklen_t *));
cold_fwd(ssize_t, recvmsg,  (int, struct msghdr *, int));
cold_fwd(ssize_t, write,    (int, const void *, size_t));
cold_fwd(ssize_t, pwrite,   (int, const void *, size_t, off_t));
cold_fwd(ssize_t, writev,   (int, const struct iovec *, int));
cold_fwd(ssize_t, send,     (int, const void *, size_t, int));
cold_fwd(ssize_t, sendto,   (int, const void *, size_t, int, const struct sockaddr *, socklen_t));
cold_fwd(ssize_t, sendmsg,  (int, const struct msghdr *, int));

#if __linux__
cold_fwd(int, recvmmsg, (int, struct mmsghdr *, unsigned, int, struct timespec *));
cold_fwd(int, sendmmsg, (int, struct mmsghdr *, unsigned, int));
#endif

#undef cold_fwd

#if __cplusplus
}
#endif
