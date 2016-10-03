#include "cone.h"

#include <dlfcn.h>
#include <sys/socket.h>

_Thread_local struct cone * volatile cone;

struct cone_amain_ctx
{
    int ret, argc;
    const char **argv;
};

extern int amain(int argc, const char **argv);

static int cone_run_amain(struct cone_amain_ctx *c) {
    c->ret = amain(c->argc, c->argv);
    return 0;
}

extern int main(int argc, const char **argv) {
    struct cone_amain_ctx c = {1, argc, argv};
    return cone_main(cone_bind(&cone_run_amain, &c)) ? 1 : c.ret;
}

#define cone_libc(ret, name, ...)                \
    static ret (*cone_libc_##name)(__VA_ARGS__); \
    extern ret name(__VA_ARGS__)

#define cone_libc_io(fd, write, rettype, expr) { \
    rettype r;                                   \
    do r = expr; while (r < 0 && (errno == EWOULDBLOCK || errno == EAGAIN) && !cone_iowait(fd, write)); \
    return r;                                    \
}

cone_libc(int, listen, int fd, int backlog) {
    return cone && setnonblocking(fd) < 0 ? -1 : cone_libc_listen(fd, backlog);
}

cone_libc(int, accept3, int fd, struct sockaddr *addr, socklen_t *addrlen)
    cone_libc_io(fd, 0, int, cone_libc_accept3(fd, addr, addrlen))

#ifdef _GNU_SOURCE
cone_libc(int, accept4, int fd, struct sockaddr *addr, socklen_t *addrlen, int flags)
    cone_libc_io(fd, 0, int, cone_libc_accept4(fd, addr, addrlen, cone ? flags | SOCK_NONBLOCK : flags))
#endif

int accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
#ifdef _GNU_SOURCE
    return accept4(fd, addr, addrlen, 0);
#else
    return (fd = accept3(fd, addr, addrlen)) < 0 ? -1 : setnonblocking(fd) ? (close(fd), -1) : fd;
#endif
}

cone_libc(ssize_t, read, int fd, void *buf, size_t count)
    cone_libc_io(fd, 0, ssize_t, cone_libc_read(fd, buf, count))

cone_libc(ssize_t, recv, int fd, void *buf, size_t len, int flags)
    cone_libc_io(fd, 0, ssize_t, cone_libc_recv(fd, buf, len, flags))

cone_libc(ssize_t, recvfrom, int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
    cone_libc_io(fd, 0, ssize_t, cone_libc_recvfrom(fd, buf, len, flags, src_addr, addrlen))

cone_libc(ssize_t, recvmsg, int fd, struct msghdr *msg, int flags)
    cone_libc_io(fd, 0, ssize_t, cone_libc_recvmsg(fd, msg, flags))

cone_libc(ssize_t, write, int fd, const void *buf, size_t count)
    cone_libc_io(fd, 1, ssize_t, cone_libc_write(fd, buf, count))

cone_libc(ssize_t, send, int fd, const void *buf, size_t len, int flags)
    cone_libc_io(fd, 1, ssize_t, cone_libc_send(fd, buf, len, flags))

cone_libc(ssize_t, sendto, int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
    cone_libc_io(fd, 1, ssize_t, cone_libc_sendto(fd, buf, len, flags, dest_addr, addrlen))

cone_libc(ssize_t, sendmsg, int fd, const struct msghdr *msg, int flags)
    cone_libc_io(fd, 1, ssize_t, cone_libc_sendmsg(fd, msg, flags))

cone_libc(unsigned, sleep, unsigned sec) {
    return !cone ? cone_libc_sleep(sec) : cone_sleep(CONE_U128((uint64_t)sec * 1000000000ull)) ? sec : 0;
}

cone_libc(int, nanosleep, const struct timespec *req, struct timespec *rem) {
    return !cone ? cone_libc_nanosleep(req, rem) : cone_sleep(cone_nsec_from_timespec(*req));
}

cone_libc(int, sched_yield, void) {
    return !cone ? cone_libc_sched_yield == NULL ? -1 : cone_libc_sched_yield()
                 : cone_loop_ping(cone->loop) ? -1 : cone_wait(&cone->loop->on_ping);
}

static void __attribute__((constructor))
cone_libc_init(void) {
#if __cplusplus
#define cone_libc_load(name, sym) name = (decltype(name)) dlsym(RTLD_NEXT, sym)
#else
#define cone_libc_load(name, sym) name = dlsym(RTLD_NEXT, sym)
#endif
    cone_libc_load(cone_libc_listen,      "listen");
    cone_libc_load(cone_libc_accept3,     "accept");
#ifdef _GNU_SOURCE
    cone_libc_load(cone_libc_accept4,     "accept4");
#endif
    cone_libc_load(cone_libc_read,        "read");
    cone_libc_load(cone_libc_recv,        "recv");
    cone_libc_load(cone_libc_recvfrom,    "recvfrom");
    cone_libc_load(cone_libc_recvmsg,     "recvmsg");
    cone_libc_load(cone_libc_write,       "write");
    cone_libc_load(cone_libc_send,        "send");
    cone_libc_load(cone_libc_sendto,      "sendto");
    cone_libc_load(cone_libc_sendmsg,     "sendmsg");
    cone_libc_load(cone_libc_sleep,       "sleep");
    cone_libc_load(cone_libc_nanosleep,   "nanosleep");
    cone_libc_load(cone_libc_sched_yield, "sched_yield");
}
