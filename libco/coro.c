#include "coro.h"

#include <dlfcn.h>
#include <sys/socket.h>

_Thread_local struct coro * volatile coro_current;

struct co_amain_ctx
{
    int ret, argc;
    const char **argv;
};

extern int amain(int argc, const char **argv);

static int co_run_amain(struct co_amain_ctx *c) {
    c->ret = amain(c->argc, c->argv);
    return 0;
}

extern int main(int argc, const char **argv) {
    struct co_amain_ctx c = {1, argc, argv};
    return coro_main(co_bind(&co_run_amain, &c)) ? 1 : c.ret;
}

#define co_libc(ret, name, ...)                \
    static ret (*co_libc_##name)(__VA_ARGS__); \
    extern ret name(__VA_ARGS__)

#define co_libc_io(fd, write, rettype, expr) { \
    rettype r;                                 \
    do r = expr; while (r < 0 && (errno == EWOULDBLOCK || errno == EAGAIN) && !coro_iowait(fd, write)); \
    return r;                                  \
}

co_libc(int, listen, int fd, int backlog) {
    return coro_current && setnonblocking(fd) < 0 ? -1 : co_libc_listen(fd, backlog);
}

co_libc(int, accept3, int fd, struct sockaddr *addr, socklen_t *addrlen)
    co_libc_io(fd, 0, int, co_libc_accept3(fd, addr, addrlen))

#ifdef _GNU_SOURCE
co_libc(int, accept4, int fd, struct sockaddr *addr, socklen_t *addrlen, int flags)
    co_libc_io(fd, 0, int, co_libc_accept4(fd, addr, addrlen, coro_current ? flags | SOCK_NONBLOCK : flags))
#endif

int accept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
#ifdef _GNU_SOURCE
    return accept4(fd, addr, addrlen, 0);
#else
    return (fd = accept3(fd, addr, addrlen)) < 0 ? -1 : setnonblocking(fd) ? (close(fd), -1) : fd;
#endif
}

co_libc(ssize_t, read, int fd, void *buf, size_t count)
    co_libc_io(fd, 0, ssize_t, co_libc_read(fd, buf, count))

co_libc(ssize_t, recv, int fd, void *buf, size_t len, int flags)
    co_libc_io(fd, 0, ssize_t, co_libc_recv(fd, buf, len, flags))

co_libc(ssize_t, recvfrom, int fd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
    co_libc_io(fd, 0, ssize_t, co_libc_recvfrom(fd, buf, len, flags, src_addr, addrlen))

co_libc(ssize_t, recvmsg, int fd, struct msghdr *msg, int flags)
    co_libc_io(fd, 0, ssize_t, co_libc_recvmsg(fd, msg, flags))

co_libc(ssize_t, write, int fd, const void *buf, size_t count)
    co_libc_io(fd, 1, ssize_t, co_libc_write(fd, buf, count))

co_libc(ssize_t, send, int fd, const void *buf, size_t len, int flags)
    co_libc_io(fd, 1, ssize_t, co_libc_send(fd, buf, len, flags))

co_libc(ssize_t, sendto, int fd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
    co_libc_io(fd, 1, ssize_t, co_libc_sendto(fd, buf, len, flags, dest_addr, addrlen))

co_libc(ssize_t, sendmsg, int fd, const struct msghdr *msg, int flags)
    co_libc_io(fd, 1, ssize_t, co_libc_sendmsg(fd, msg, flags))

co_libc(unsigned, sleep, unsigned sec) {
    return !coro_current ? co_libc_sleep(sec) : coro_sleep(CO_U128((uint64_t)sec * 1000000000ull)) ? sec : 0;
}

co_libc(int, nanosleep, const struct timespec *req, struct timespec *rem) {
    return !coro_current ? co_libc_nanosleep(req, rem) : coro_sleep(co_nsec_from_timespec(*req));
}

co_libc(int, sched_yield, void) {
    return !coro_current ? co_libc_sched_yield == NULL ? -1 : co_libc_sched_yield()
                         : co_loop_ping(coro_current->loop) ? -1 : coro_wait(&coro_current->loop->on_ping);
}

static void __attribute__((constructor))
co_libc_init(void) {
#if __cplusplus
#define co_libc_load(name, sym) name = (decltype(name)) dlsym(RTLD_NEXT, sym)
#else
#define co_libc_load(name, sym) name = dlsym(RTLD_NEXT, sym)
#endif
    co_libc_load(co_libc_listen,      "listen");
    co_libc_load(co_libc_accept3,     "accept");
#ifdef _GNU_SOURCE
    co_libc_load(co_libc_accept4,     "accept4");
#endif
    co_libc_load(co_libc_read,        "read");
    co_libc_load(co_libc_recv,        "recv");
    co_libc_load(co_libc_recvfrom,    "recvfrom");
    co_libc_load(co_libc_recvmsg,     "recvmsg");
    co_libc_load(co_libc_write,       "write");
    co_libc_load(co_libc_send,        "send");
    co_libc_load(co_libc_sendto,      "sendto");
    co_libc_load(co_libc_sendmsg,     "sendmsg");
    co_libc_load(co_libc_sleep,       "sleep");
    co_libc_load(co_libc_nanosleep,   "nanosleep");
    co_libc_load(co_libc_sched_yield, "sched_yield");
}
