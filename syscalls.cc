#include "coro.h"

#include <time.h>
#include <errno.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

namespace libc {
    #define FUNC(name, R, Args) static auto name = (R(*)Args) dlsym(RTLD_NEXT, #name);
    FUNC(listen,    int,      (int, int));
    FUNC(accept,    int,      (int, struct sockaddr *, socklen_t *));
    FUNC(accept4,   int,      (int, struct sockaddr *, socklen_t *, int));
    FUNC(read,      ssize_t,  (int, void *, size_t));
    FUNC(write,     ssize_t,  (int, const void *, size_t));
    FUNC(recv,      ssize_t,  (int, void *, size_t, int));
    FUNC(recvfrom,  ssize_t,  (int, void *, size_t, int, struct sockaddr *, socklen_t *));
    FUNC(recvmsg,   ssize_t,  (int, struct msghdr *, int));
    FUNC(send,      ssize_t,  (int, const void *, size_t, int));
    FUNC(sendto,    ssize_t,  (int, const void *, size_t, int, const struct sockaddr *, socklen_t));
    FUNC(sendmsg,   ssize_t,  (int, const struct msghdr *, int));
    FUNC(sleep,     unsigned, (unsigned));
    FUNC(usleep,    int,      (useconds_t));
    FUNC(nanosleep, int,      (const struct timespec *req, struct timespec *rem));

    #ifdef _POSIX_PRIORITY_SCHEDULING
        FUNC(sched_yield, int, ());
    #endif
    #undef FUNC
};

template <bool write, typename R, typename... Args>
static R loop(R (*f)(int, Args...), int fd, Args... args) {
    if (auto c = coro::current) {
        R r = R(-1);
        while ((r = f(fd, args...)) == R(-1) && (errno == EWOULDBLOCK || errno == EAGAIN))
            c->pause(write ? c->loop->io[fd].write : c->loop->io[fd].read);
        return r;
    }
    return f(fd, args...);
}

template <typename Rep, typename Period, typename R, typename... Args>
static R pause(R (*f)(Args...), std::chrono::duration<Rep, Period> total, Args... args) {
    if (auto c = coro::current) {
        c->pause<false, true>(c->loop->after[total]);
        return R();
    }
    return f(args...);
}

extern "C" int listen(int sockfd, int backlog) {
    if (aio::unblock(sockfd))
        return -1;
    return libc::listen(sockfd, backlog);
}

extern "C" int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    return loop<false>(libc::accept4, sockfd, addr, addrlen, (int)SOCK_NONBLOCK);
}

extern "C" int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags) {
    return loop<false>(libc::accept4, sockfd, addr, addrlen, flags | SOCK_NONBLOCK);
}

extern "C" ssize_t read(int fd, void *buf, size_t count) {
    return loop<false>(libc::read, fd, buf, count);
}

extern "C" ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    return loop<false>(libc::recv, sockfd, buf, len, flags);
}

extern "C" ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen) {
    return loop<false>(libc::recvfrom, sockfd, buf, len, flags, src_addr, addrlen);
}

extern "C" ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags) {
    return loop<false>(libc::recvmsg, sockfd, msg, flags);
}

extern "C" ssize_t write(int fd, const void *buf, size_t count) {
    return loop<true>(libc::write, fd, buf, count);
}

extern "C" ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    return loop<true>(libc::send, sockfd, buf, len, flags);
}

extern "C" ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen) {
    return loop<true>(libc::sendto, sockfd, buf, len, flags, dest_addr, addrlen);
}

extern "C" ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags) {
    return loop<true>(libc::sendmsg, sockfd, msg, flags);
}

extern "C" unsigned sleep(unsigned seconds) {
    return pause(libc::sleep, std::chrono::seconds(seconds), seconds);
}

extern "C" int usleep(useconds_t usec) {
    return pause(libc::usleep, std::chrono::microseconds(usec), usec);
}

extern "C" int nanosleep(const struct timespec *req, struct timespec *rem) {
    return pause(libc::nanosleep, std::chrono::seconds(req->tv_sec) + std::chrono::nanoseconds(req->tv_nsec), req, rem);
}

extern "C" int sched_yield(void) {
    if (auto c = coro::current) {
        c->loop->ping();
        c->pause(c->loop->on_ping);
    }
    #ifdef _POSIX_PRIORITY_SCHEDULING
    else return libc::sched_yield();
    #endif
    return 0;
}
