#pragma once
#ifndef COIL_INTERCEPT_DYNAMIC_LIBC
#define COIL_INTERCEPT_DYNAMIC_LIBC 0
#endif

#include "cone.h"

#include <sys/socket.h>

#if COIL_INTERCEPT_DYNAMIC_LIBC
#include <dlfcn.h>

#define coil_libc_call(name, ...) coil_libc_##name(__VA_ARGS__)
#define coil_libc_defn(ret, name, ...) \
    static ret (*coil_libc_##name)(__VA_ARGS__); \
    extern ret name(__VA_ARGS__)
#else
#define coil_libc_call(name, ...) name(__VA_ARGS__)
#define coil_libc_defn(ret, name, ...) extern ret coil_##name(__VA_ARGS__)
#endif

#define coil_libc_io(fd, write, rettype, expr) { rettype r; coil_libc_io_impl(r, fd, write, expr); return r; }
#define coil_libc_io_impl(r, fd, write, expr) \
    do r = expr; while (r < 0 && (errno == EWOULDBLOCK || errno == EAGAIN) && !cone_iowait(fd, write));

coil_libc_defn(int, listen, int fd, int backlog) {
    return cone && setnonblocking(fd) < 0 ? -1 : coil_libc_call(listen, fd, backlog);
}

coil_libc_defn(int, accept, int fd, struct sockaddr *addr, socklen_t *addrlen) {
    int client;
    coil_libc_io_impl(client, fd, 0, coil_libc_call(accept, fd, addr, addrlen));
    return client < 0 ? -1 : setnonblocking(client) ? (close(client), -1) : client;
}

#ifdef _GNU_SOURCE
coil_libc_defn(int, accept4, int fd, struct sockaddr *addr, socklen_t *addrlen, int flags)
    coil_libc_io(fd, 0, int, coil_libc_call(accept4, fd, addr, addrlen, cone ? flags | SOCK_NONBLOCK : flags))
#endif

coil_libc_defn(ssize_t, read, int fd, void *buf, size_t count)
    coil_libc_io(fd, 0, ssize_t, coil_libc_call(read, fd, buf, count))

coil_libc_defn(ssize_t, recv, int fd, void *buf, size_t len, int flags)
    coil_libc_io(fd, 0, ssize_t, coil_libc_call(recv, fd, buf, len, flags))

coil_libc_defn(ssize_t, recvfrom, int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
    coil_libc_io(fd, 0, ssize_t, coil_libc_call(recvfrom, fd, buf, len, flags, src_addr, addrlen))

coil_libc_defn(ssize_t, recvmsg, int fd, struct msghdr *msg, int flags)
    coil_libc_io(fd, 0, ssize_t, coil_libc_call(recvmsg, fd, msg, flags))

coil_libc_defn(ssize_t, write, int fd, const void *buf, size_t count)
    coil_libc_io(fd, 1, ssize_t, coil_libc_call(write, fd, buf, count))

coil_libc_defn(ssize_t, send, int fd, const void *buf, size_t len, int flags)
    coil_libc_io(fd, 1, ssize_t, coil_libc_call(send, fd, buf, len, flags))

coil_libc_defn(ssize_t, sendto, int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
    coil_libc_io(fd, 1, ssize_t, coil_libc_call(sendto, fd, buf, len, flags, dest_addr, addrlen))

coil_libc_defn(ssize_t, sendmsg, int fd, const struct msghdr *msg, int flags)
    coil_libc_io(fd, 1, ssize_t, coil_libc_call(sendmsg, fd, msg, flags))

coil_libc_defn(unsigned, sleep, unsigned sec) {
    return !cone ? coil_libc_call(sleep, sec) : cone_sleep(CONE_U128((uint64_t)sec * 1000000000ull)) ? sec : 0;
}

coil_libc_defn(int, nanosleep, const struct timespec *req, struct timespec *rem) {
    return !cone ? coil_libc_call(nanosleep, req, rem) : cone_sleep(cone_nsec_from_timespec(*req));
}

#ifdef _POSIX_PRIORITY_SCHEDULING
#include <sched.h>

coil_libc_defn(int, sched_yield, void) {
    return !cone ? coil_libc_call(sched_yield) : cone_loop_ping(cone->loop) ? -1 : cone_wait(&cone->loop->on_ping);
}
#endif

#if COIL_INTERCEPT_DYNAMIC_LIBC
static void __attribute__((constructor))
coil_libc_init(void) {
    coil_libc_listen      = dlsym(RTLD_NEXT, "listen");
    coil_libc_accept      = dlsym(RTLD_NEXT, "accept");
    coil_libc_accept4     = dlsym(RTLD_NEXT, "accept4");
    coil_libc_read        = dlsym(RTLD_NEXT, "read");
    coil_libc_recv        = dlsym(RTLD_NEXT, "recv");
    coil_libc_recvfrom    = dlsym(RTLD_NEXT, "recvfrom");
    coil_libc_recvmsg     = dlsym(RTLD_NEXT, "recvmsg");
    coil_libc_write       = dlsym(RTLD_NEXT, "write");
    coil_libc_send        = dlsym(RTLD_NEXT, "send");
    coil_libc_sendto      = dlsym(RTLD_NEXT, "sendto");
    coil_libc_sendmsg     = dlsym(RTLD_NEXT, "sendmsg");
    coil_libc_sleep       = dlsym(RTLD_NEXT, "sleep");
    coil_libc_nanosleep   = dlsym(RTLD_NEXT, "nanosleep");
#ifdef _POSIX_PRIORITY_SCHEDULING
    coil_libc_sched_yield = dlsym(RTLD_NEXT, "sched_yield");
#endif
}
#endif
