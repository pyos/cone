#include <mutex>

#include "../cold.h"

static const char *suffixes[] = {"s", "ms", "us", "ns"};

template <typename F /*= bool(size_t iterations) */>
static bool measure(char *msg, F&& f) {
    double total = 0;
    size_t n = 0;
    auto deadline = cone->deadline(cone::time::clock::now() + std::chrono::seconds(10));
    for (size_t r = 1;; ) {
        auto a = cone::time::clock::now();
        if (!f(r)) {
            sprintf(msg, "x%zu + x%zu", n, r);
            return false;
        }
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
    sprintf(msg, n == 1 ? "%f %s" : "%f %s/iter (x%zu)", t, suffixes[i], n);
    return true;
}

template <size_t ratio /* a to b */, typename F /*= bool(size_t a, size_t b) */>
static bool measure2(char *msg, F&& f) {
    return measure(msg, [&](size_t i) {
        size_t b = i / (ratio + 1);
        size_t a = b * ratio;
        return f(a, b);
    });
}

static bool test_yield(char *msg) {
    return measure(msg, [](size_t yields) {
        for (size_t i = 0; i < yields; i++)
            if (!cone::yield() MUN_RETHROW)
                return false;
        return true;
    });
}

static bool test_spawn(char *msg) {
    return measure(msg, [](size_t cones) {
        for (size_t i = 0; i < cones; i++)
            if (!cone::ref{[](){ return true; }}->wait(cone::rethrow) MUN_RETHROW)
                return false;
        return true;
    });
}

static bool test_spawn_many(char *msg) {
    return measure(msg, [](size_t cones) { return spawn_and_wait(cones, []() { return true; }); });
}

template <size_t ratio>
static bool test_spawn_many_yielding(char *msg) {
    return measure2<ratio>(msg, [](size_t cones, size_t yields_per_cone) {
        return spawn_and_wait(cones, [=]() {
            for (size_t n = yields_per_cone; n--;)
                if (!cone::yield() MUN_RETHROW)
                    return false;
            return true;
        });
    });
}

template <size_t ratio>
static bool test_mutex(char *msg) {
    return measure2<ratio>(msg, [&](size_t cones, size_t yields_per_cone) {
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
static bool test_mt_mutex(char *msg) {
    return measure2<ratio>(msg, [&](size_t iters, size_t cones) {
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

export { "perf:yield/N", &test_yield }
     , { "perf:(spawn(nop), wait, drop)/N", &test_spawn }
     , { "perf:spawn(nop)/N, wait/N, drop/N", &test_spawn_many }
     , { "perf:spawn(yield/N)/1kN, wait/1kN, drop/1kN", &test_spawn_many_yielding<1000> }
     , { "perf:spawn((lock, yield, unlock)/N)/200N, wait/200N, drop/200N", &test_mutex<200> }
     , { "perf:8 threads:spawn((lock, inc, unlock)/10N)/N (cone::mutex)", &test_mt_mutex<8, 10, cone::mutex> }
     , { "perf:8 threads:spawn((lock, inc, unlock)/10N)/N (std::mutex)", &test_mt_mutex<8, 10, std::mutex> }
     , { "perf:8 threads:spawn((lock, inc, unlock)/100kN)/N (cone::mutex)", &test_mt_mutex<8, 100000, cone::mutex> }
     , { "perf:8 threads:spawn((lock, inc, unlock)/100kN)/N (std::mutex)", &test_mt_mutex<8, 100000, std::mutex> }
