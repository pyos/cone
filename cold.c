#include "cone.h"

#include <dlfcn.h>
#include <unistd.h>
#include <sys/socket.h>

#define cold_call(name, ...) cold_##name(__VA_ARGS__)
#define cold_defn(ret, name, ...) \
    static ret (*cold_##name)(__VA_ARGS__); \
    extern ret name(__VA_ARGS__)

#define cold_io(fd, write, rettype, expr) { rettype r; cold_io_impl(r, fd, write, expr); return r; }
#define cold_io_impl(r, fd, write, expr) \
    do r = expr; while (cone && r < 0 && (errno == EWOULDBLOCK || errno == EAGAIN) && !cone_iowait(fd, write));

cold_defn(int, listen, int fd, int backlog) {
    return cone && cone_unblock(fd) < 0 ? -1 : cold_call(listen, fd, backlog);
}

cold_defn(int, accept, int fd, struct sockaddr *addr, socklen_t *addrlen) {
    int client;
    cold_io_impl(client, fd, 0, cold_call(accept, fd, addr, addrlen));
    return client < 0 ? -1 : cone_unblock(client) ? (close(client), -1) : client;
}

cold_defn(int, accept4, int fd, struct sockaddr *addr, socklen_t *addrlen, int flags)
    cold_io(fd, 0, int, cold_call(accept4, fd, addr, addrlen, cone ? flags | SOCK_NONBLOCK : flags))

cold_defn(ssize_t, read, int fd, void *buf, size_t count)
    cold_io(fd, 0, ssize_t, cold_call(read, fd, buf, count))

cold_defn(ssize_t, recv, int fd, void *buf, size_t len, int flags)
    cold_io(fd, 0, ssize_t, cold_call(recv, fd, buf, len, flags))

cold_defn(ssize_t, recvfrom, int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
    cold_io(fd, 0, ssize_t, cold_call(recvfrom, fd, buf, len, flags, src_addr, addrlen))

cold_defn(ssize_t, recvmsg, int fd, struct msghdr *msg, int flags)
    cold_io(fd, 0, ssize_t, cold_call(recvmsg, fd, msg, flags))

cold_defn(ssize_t, write, int fd, const void *buf, size_t count)
    cold_io(fd, 1, ssize_t, cold_call(write, fd, buf, count))

cold_defn(ssize_t, send, int fd, const void *buf, size_t len, int flags)
    cold_io(fd, 1, ssize_t, cold_call(send, fd, buf, len, flags))

cold_defn(ssize_t, sendto, int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
    cold_io(fd, 1, ssize_t, cold_call(sendto, fd, buf, len, flags, dest_addr, addrlen))

cold_defn(ssize_t, sendmsg, int fd, const struct msghdr *msg, int flags)
    cold_io(fd, 1, ssize_t, cold_call(sendmsg, fd, msg, flags))

cold_defn(unsigned, sleep, unsigned sec) {
    return !cone ? cold_call(sleep, sec) : cone_sleep(veil_u128_mul((veil_nsec){0, sec}, 1000000000ul)) ? sec : 0;
}

cold_defn(int, nanosleep, const struct timespec *req, struct timespec *rem) {
    return !cone ? cold_call(nanosleep, req, rem) : cone_sleep(veil_nsec_from_timespec(*req));
}

cold_defn(int, sched_yield, void) {
    return !cone ? cold_call(sched_yield) : cone_yield();
}

static __attribute__((constructor)) void cold_init(void) {
    cold_listen      = dlsym(RTLD_NEXT, "listen");
    cold_accept      = dlsym(RTLD_NEXT, "accept");
    cold_accept4     = dlsym(RTLD_NEXT, "accept4");
    cold_read        = dlsym(RTLD_NEXT, "read");
    cold_recv        = dlsym(RTLD_NEXT, "recv");
    cold_recvfrom    = dlsym(RTLD_NEXT, "recvfrom");
    cold_recvmsg     = dlsym(RTLD_NEXT, "recvmsg");
    cold_write       = dlsym(RTLD_NEXT, "write");
    cold_send        = dlsym(RTLD_NEXT, "send");
    cold_sendto      = dlsym(RTLD_NEXT, "sendto");
    cold_sendmsg     = dlsym(RTLD_NEXT, "sendmsg");
    cold_sleep       = dlsym(RTLD_NEXT, "sleep");
    cold_nanosleep   = dlsym(RTLD_NEXT, "nanosleep");
#ifdef _POSIX_PRIORITY_SCHEDULING
    cold_sched_yield = dlsym(RTLD_NEXT, "sched_yield");
#endif
}
