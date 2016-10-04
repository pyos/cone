/*
 * collld / cone linked with linux libc dynamically
 *          --   -           -     -    -
 */
#include "cone.h"

#include <dlfcn.h>
#include <sys/socket.h>

#define collld_call(name, ...) collld_##name(__VA_ARGS__)
#define collld_defn(ret, name, ...) \
    static ret (*collld_##name)(__VA_ARGS__); \
    extern ret name(__VA_ARGS__)

#define collld_io(fd, write, rettype, expr) { rettype r; collld_io_impl(r, fd, write, expr); return r; }
#define collld_io_impl(r, fd, write, expr) \
    do r = expr; while (cone && r < 0 && (errno == EWOULDBLOCK || errno == EAGAIN) && !cone_iowait(fd, write));

collld_defn(int, listen, int fd, int backlog) {
    return cone && cone_unblock(fd) < 0 ? -1 : collld_call(listen, fd, backlog);
}

collld_defn(int, accept, int fd, struct sockaddr *addr, socklen_t *addrlen) {
    int client;
    collld_io_impl(client, fd, 0, collld_call(accept, fd, addr, addrlen));
    return client < 0 ? -1 : cone_unblock(client) ? (close(client), -1) : client;
}

collld_defn(int, accept4, int fd, struct sockaddr *addr, socklen_t *addrlen, int flags)
    collld_io(fd, 0, int, collld_call(accept4, fd, addr, addrlen, cone ? flags | SOCK_NONBLOCK : flags))

collld_defn(ssize_t, read, int fd, void *buf, size_t count)
    collld_io(fd, 0, ssize_t, collld_call(read, fd, buf, count))

collld_defn(ssize_t, recv, int fd, void *buf, size_t len, int flags)
    collld_io(fd, 0, ssize_t, collld_call(recv, fd, buf, len, flags))

collld_defn(ssize_t, recvfrom, int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
    collld_io(fd, 0, ssize_t, collld_call(recvfrom, fd, buf, len, flags, src_addr, addrlen))

collld_defn(ssize_t, recvmsg, int fd, struct msghdr *msg, int flags)
    collld_io(fd, 0, ssize_t, collld_call(recvmsg, fd, msg, flags))

collld_defn(ssize_t, write, int fd, const void *buf, size_t count)
    collld_io(fd, 1, ssize_t, collld_call(write, fd, buf, count))

collld_defn(ssize_t, send, int fd, const void *buf, size_t len, int flags)
    collld_io(fd, 1, ssize_t, collld_call(send, fd, buf, len, flags))

collld_defn(ssize_t, sendto, int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
    collld_io(fd, 1, ssize_t, collld_call(sendto, fd, buf, len, flags, dest_addr, addrlen))

collld_defn(ssize_t, sendmsg, int fd, const struct msghdr *msg, int flags)
    collld_io(fd, 1, ssize_t, collld_call(sendmsg, fd, msg, flags))

collld_defn(unsigned, sleep, unsigned sec) {
    return !cone ? collld_call(sleep, sec) : cone_sleep(cot_u128_mul((cot_nsec){0, sec}, 1000000000ul)) ? sec : 0;
}

collld_defn(int, nanosleep, const struct timespec *req, struct timespec *rem) {
    return !cone ? collld_call(nanosleep, req, rem) : cone_sleep(cot_nsec_from_timespec(*req));
}

collld_defn(int, sched_yield, void) {
    return !cone ? collld_call(sched_yield) : cone_loop_ping(cone->loop) ? -1 : cone_wait(&cone->loop->on_ping);
}

static __attribute__((constructor)) void collld_init(void) {
    collld_listen      = dlsym(RTLD_NEXT, "listen");
    collld_accept      = dlsym(RTLD_NEXT, "accept");
    collld_accept4     = dlsym(RTLD_NEXT, "accept4");
    collld_read        = dlsym(RTLD_NEXT, "read");
    collld_recv        = dlsym(RTLD_NEXT, "recv");
    collld_recvfrom    = dlsym(RTLD_NEXT, "recvfrom");
    collld_recvmsg     = dlsym(RTLD_NEXT, "recvmsg");
    collld_write       = dlsym(RTLD_NEXT, "write");
    collld_send        = dlsym(RTLD_NEXT, "send");
    collld_sendto      = dlsym(RTLD_NEXT, "sendto");
    collld_sendmsg     = dlsym(RTLD_NEXT, "sendmsg");
    collld_sleep       = dlsym(RTLD_NEXT, "sleep");
    collld_nanosleep   = dlsym(RTLD_NEXT, "nanosleep");
#ifdef _POSIX_PRIORITY_SCHEDULING
    collld_sched_yield = dlsym(RTLD_NEXT, "sched_yield");
#endif
}
