#pragma once

#include <unistd.h>
#include <sys/socket.h>
#include <sys/uio.h>

#if __cplusplus
extern "C" {
#endif

// Switch a file descriptor into non-blocking mode. See fcntl(2) for error codes.
int cold_unblock(int fd);

// See the manual for your libc of choice. When called from inside a coroutine,
// * `listen` and `connect` switch their argument into non-blocking mode;
// * `accept` does the same with the returned file descriptor;
// * anything that would return EAGAIN is instead coroutine-blocking.
ssize_t cold_read     (int, void *, size_t);
ssize_t cold_pread    (int, void *, size_t, off_t);
ssize_t cold_readv    (int, const struct iovec *, int);
ssize_t cold_recv     (int, void *, size_t, int);
ssize_t cold_recvfrom (int, void *, size_t, int, struct sockaddr *, socklen_t *);
ssize_t cold_recvmsg  (int, struct msghdr *, int);
ssize_t cold_write    (int, const void *, size_t);
ssize_t cold_pwrite   (int, const void *, size_t, off_t);
ssize_t cold_writev   (int, const struct iovec *, int);
ssize_t cold_send     (int, const void *, size_t, int);
ssize_t cold_sendto   (int, const void *, size_t, int, const struct sockaddr *, socklen_t);
ssize_t cold_sendmsg  (int, const struct msghdr *, int);
int     cold_listen   (int, int);
int     cold_connect  (int, const struct sockaddr *, socklen_t);
int     cold_accept   (int, struct sockaddr *, socklen_t *);
#if defined(__linux__) && defined(_GNU_SOURCE)
int     cold_accept4  (int, struct sockaddr *, socklen_t *, int);
int     cold_recvmmsg (int, struct mmsghdr *, unsigned, int, struct timespec *);
int     cold_sendmmsg (int, struct mmsghdr *, unsigned, int);
#endif

#if __cplusplus
}
#endif
