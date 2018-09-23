#include <mutex>

static const char *suffixes[] = {"s", "ms", "us", "ns"};

template <size_t n = 1, typename F>
static auto measure(char *msg, F&& f) {
    auto a = cone::time::clock::now();
    auto r = f();
    auto b = cone::time::clock::now();
    auto t = std::chrono::duration_cast<std::chrono::duration<double>>(b - a).count() / n;
    unsigned i = 0;
    for (; i + 1 < sizeof(suffixes) / sizeof(suffixes[0]) && t < 0.1; i++)
        t *= 1000;
    sprintf(msg, n == 1 ? "%f %s" : "%f %s/iter", t, suffixes[i]);
    return r;
}

template <size_t yields>
static bool test_yield(char *msg) {
    return measure<yields>(msg, [] {
        for (size_t i = 0; i < yields; i++)
            if (!cone::yield())
                return false;
        return true;
    });
}

template <size_t cones>
static bool test_spawn(char *msg) {
    return measure<cones>(msg, [] {
        for (size_t i = 0; i < cones; i++)
            if (!cone::ref{[](){ return true; }}->wait())
                return false;
        return true;
    });
}

template <size_t cones>
static bool test_spawn_many(char *msg) {
    return measure<cones>(msg, []() { return spawn_and_wait<cones>([]() { return true; }); });
}

template <size_t cones, size_t yields_per_cone>
static bool test_spawn_many_yielding(char *msg) {
    return measure<cones>(msg, []() {
        return spawn_and_wait<cones>([]() {
            for (size_t n = yields_per_cone; n--;)
                if (!cone::yield())
                    return false;
            return true;
        });
    });
}

template <size_t cones, size_t yields_per_cone>
static bool test_mutex(char *msg) {
    size_t r = 0;
    cone::mutex m;
    return measure<cones>(msg, [&]() {
        return spawn_and_wait<cones>([&]() {
            for (size_t n = yields_per_cone; n--;) {
                std::unique_lock<cone::mutex> g(m);
                size_t c = r;
                if (!cone::yield())
                    return false;
                r = c + 1;
            }
            return true;
        });
    }) && !mun_assert(r == cones * yields_per_cone, "%zu != %zu", r, cones * yields_per_cone);
}

template <size_t threads, size_t cones, size_t iters, typename M>
static bool test_mt_mutex(char *msg) {
    M m;
    size_t r = 0;
    size_t e = threads * cones * iters;
    return measure(msg, [&]() {
        return spawn_and_wait<threads, cone::thread>([&]() {
            return spawn_and_wait<cones>([&]() {
                for (size_t j = 0; j < iters; j++) {
                    std::unique_lock<M> g(m);
                    r++;
                }
                return true;
            });
        });
    }) && ASSERT(r == e, "%zu != %zu", r, e);
}

export { "perf:yield x 3m", &test_yield<3000000> }
     , { "perf:(spawn(nop), wait, drop) x 6m", &test_spawn<6000000> }
     , { "perf:spawn(nop) x 1m, wait x 1m, drop x 1m", &test_spawn_many<1000000> }
     , { "perf:spawn(yield x 100) x 100k, wait x 100k, drop x 100k", &test_spawn_many_yielding<100000, 100> }
     , { "perf:spawn((lock, yield, unlock) x 100) x 20k, wait x 20k, drop x 20k", &test_mutex<20000, 100> }
     , { "perf:8 threads * 100 cones * 1k locked increments (cone::mutex)", &test_mt_mutex<8, 100, 1000, cone::mutex> }
     , { "perf:8 threads * 100 cones * 1k locked increments (std::mutex)", &test_mt_mutex<8, 100, 1000, std::mutex> }
     , { "perf:8 threads * 1 cone * 100k locked increments (cone::mutex)", &test_mt_mutex<8, 1, 100000, cone::mutex> }
     , { "perf:8 threads * 1 cone * 100k locked increments (std::mutex)", &test_mt_mutex<8, 1, 100000, std::mutex> }
