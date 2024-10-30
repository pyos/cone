#include "base.cc"
#include <mutex>

#include "../cold.h"

static const char *suffixes[] = {"s", "ms", "us", "ns"};

template <typename F /*= bool(size_t iterations) */>
static bool measure(F&& f) {
    double total = 0;
    size_t n = 0;
    auto deadline = cone->deadline(cone::time::clock::now() + std::chrono::seconds(10));
    for (size_t r = 1;; ) {
        auto a = cone::time::clock::now();
        if (!f(r)) return !INFO("x%zu + x%zu", n, r);
        auto b = cone::time::clock::now();
        auto p = std::chrono::duration_cast<std::chrono::duration<double>>(b - a).count();
        if (p > 0.1) {
            total += p;
            n += r;
        }
        if (total > 2.5) break;
        if (p < 1) r *= 2;
    }
    double t = total / n;
    unsigned i = 0;
    for (; i + 1 < sizeof(suffixes) / sizeof(suffixes[0]) && t < 0.1; i++)
        t *= 1000;
    return INFO(n == 1 ? "%f %s" : "%f %s/iter (x%zu)", t, suffixes[i], n);
}

template <size_t ratio /* a to b */, typename F /*= bool(size_t a, size_t b) */>
static bool measure2(F&& f) {
    return measure([&](size_t i) {
        size_t b = i / (ratio + 1);
        size_t a = b * ratio;
        return f(a, b);
    });
}

static bool test_yield() {
    return measure([](size_t yields) {
        for (size_t i = 0; i < yields; i++)
            if (!cone::yield() MUN_RETHROW)
                return false;
        return true;
    });
}

static bool test_spawn() {
    return measure([](size_t cones) {
        for (size_t i = 0; i < cones; i++)
            if (!cone::ref{[](){ return true; }}->wait(cone::rethrow) MUN_RETHROW)
                return false;
        return true;
    });
}

static bool test_spawn_many() {
    return measure([](size_t cones) { return spawn_and_wait(cones, []() { return true; }); });
}

template <size_t ratio>
static bool test_spawn_many_yielding() {
    return measure2<ratio>([](size_t cones, size_t yields_per_cone) {
        return spawn_and_wait(cones, [=]() {
            for (size_t n = yields_per_cone; n--;)
                if (!cone::yield() MUN_RETHROW)
                    return false;
            return true;
        });
    });
}

template <size_t ratio>
static bool test_mutex() {
    return measure2<ratio>([&](size_t cones, size_t yields_per_cone) {
        size_t r = 0;
        cone::mutex m;
        return spawn_and_wait(cones, [&]() {
            for (size_t n = yields_per_cone; n--;) {
                std::unique_lock<cone::mutex> g(m);
                size_t c = r;
                if (!cone::yield() MUN_RETHROW)
                    return false;
                r = c + 1;
            }
            return true;
        }) && !mun_assert(r == cones * yields_per_cone, "%zu != %zu", r, cones * yields_per_cone);
    });
}

template <size_t threads, size_t ratio, typename M>
static bool test_mt_mutex() {
    return measure2<ratio>([&](size_t iters, size_t cones) {
        M m;
        size_t r = 0;
        size_t e = threads * cones * iters;
        return spawn_and_wait<cone::thread>(threads, [&]() {
            return spawn_and_wait(cones, [&]() {
                for (size_t j = 0; j < iters; j++) {
                    std::unique_lock<M> g(m);
                    r++;
                }
                return true;
            });
        }) && ASSERT(r == e, "%zu != %zu", r, e);
    });
}

template <size_t n>
static bool test_io() {
    return measure([&](size_t m) {
        std::vector<cone::guard> cs(n*2);
        for (size_t i = 0; i < n; i++) {
            int fds[2];
            if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) MUN_RETHROW_OS)
                return false;
            if (cold_unblock(fds[0]) || cold_unblock(fds[1]) MUN_RETHROW_OS)
                return close(fds[0]), close(fds[1]), false;
            cs[i*2+0] = [&, fd = fds[0]]() {
                char buf[1024];
                while (true) {
                    int ret = cold_read(fd, buf, sizeof(buf));
                    if (ret < 0 MUN_RETHROW_OS) return close(fd), false;
                    if (ret == 0) return close(fd), true;
                }
            };
            cs[i*2+1] = [&, fd = fds[1]]() {
                char data[] = "Hello, World!\n";
                size_t size = sizeof(data) - 1;
                for (size_t i = 0; i < m; i++) {
                    for (size_t o = 0; o < size; ) {
                        int ret = cold_write(fd, data + o, size - o);
                        if (ret < 0 MUN_RETHROW_OS) return close(fd), false;
                        o += ret;
                    }
                }
                return close(fd), true;
            };
        }
        for (cone::guard &c : cs)
            if (!c->wait(cone::rethrow) MUN_RETHROW)
                return false;
        return true;
    });
}

export {
    { "perf:yield/N", &test_yield },
    { "perf:(spawn(nop), wait, drop)/N", &test_spawn },
    { "perf:spawn(nop)/N, wait/N, drop/N", &test_spawn_many },
    { "perf:spawn(yield/N)/1kN, wait/1kN, drop/1kN", &test_spawn_many_yielding<1000> },
    { "perf:spawn((lock, yield, unlock)/N)/200N, wait/200N, drop/200N", &test_mutex<200> },
    { "perf:8 threads:spawn((lock, inc, unlock)/10N)/N (cone::mutex)", &test_mt_mutex<8, 10, cone::mutex> },
    { "perf:8 threads:spawn((lock, inc, unlock)/10N)/N (std::mutex)", &test_mt_mutex<8, 10, std::mutex> },
    { "perf:8 threads:spawn((lock, inc, unlock)/100kN)/N (cone::mutex)", &test_mt_mutex<8, 100000, cone::mutex> },
    { "perf:8 threads:spawn((lock, inc, unlock)/100kN)/N (std::mutex)", &test_mt_mutex<8, 100000, std::mutex> },
    { "perf:spawn(read/*)/100, spawn(write/N)/100, wait/200", &test_io<100> },
};
