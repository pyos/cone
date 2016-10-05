#include <sched.h>
#include <unistd.h>

struct args { unsigned N; int fd; const char *data; size_t size; };

static int writer(struct args *aptr) {
    struct args args = *aptr;
    for (ssize_t wr, mod = 0; args.N--; mod = (mod + wr) % args.size)
        if ((wr = write(args.fd, args.data + mod, args.size - mod)) < 0)
            return mun_error_os();
    return mun_ok;
}

static int reader(struct args *aptr) {
    struct args args = *aptr;
    char buf[args.size];
    for (ssize_t rd, mod = 0; args.N--; mod = (mod + rd) % args.size) {
        if ((rd = read(args.fd, buf, args.size - mod)) < 0)
            return mun_error_os();
        if (memcmp(args.data + mod, buf, rd))
            return mun_error(assert, "recvd bad data");
    }
    return mun_ok;
}

static int rdwr() {
    int fds[2];
    if (pipe(fds))
        return mun_error_os();
    if (cone_unblock(fds[0]) || cone_unblock(fds[1]))
        return close(fds[0]), close(fds[1]), mun_error_up();
    struct args ar = { 100000, fds[0], "Hello, World! Hello, World! Hello, World! Hello, World!", 55 };
    struct args aw = { 100000, fds[1], "Hello, World! Hello, World! Hello, World! Hello, World!", 55 };
    struct cone *r = cone(reader, &ar);
    struct cone *w = cone(writer, &aw);
    if (cone_join(r) || cone_join(w))
        return close(fds[0]), close(fds[1]), mun_error_up();
    return close(fds[0]), close(fds[1]), mun_ok;
}

static int yield(char *msg) {
    const unsigned N = 1000000;
    mun_nsec a = mun_nsec_monotonic();
    for (unsigned i = 0; i < N; i++)
        sched_yield();
    mun_nsec b = mun_nsec_monotonic();
    sprintf(msg, "%f ns", mun_u128_to_double(mun_u128_sub(b, a)) / N);
    return mun_ok;
}

export { "cone:reader+writer", &rdwr }
     , { "cone:sched_yield", &yield }
