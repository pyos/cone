#include <vector>

template <unsigned N, typename... Fs>
static bool run(char *msg, Fs&&... fs) {
    auto a = cone::time::clock::now();
    bool results[] = {
        [](auto&& f){
            for (unsigned i = 0; i < N; i++)
                if (!f(i))
                    return false;
            return true;
        }(fs)...
    };
    for (bool c : results)
        if (!c)
            return false;
    auto b = cone::time::clock::now();
    sprintf(msg, "%f us/iter", (double)std::chrono::duration_cast<std::chrono::microseconds>(b - a).count() / N);
    return true;
}

template <size_t yields>
static bool test_yield(char *msg) {
    return run<yields>(msg, [](unsigned) { return cone::yield(); });
}

template <size_t cones>
static bool test_spawn(char *msg) {
    return run<cones>(msg, [](unsigned) { return cone::ref{[](){ return true; }}->wait(); });
}

template <size_t cones>
static bool test_spawn_many(char *msg) {
    std::vector<cone::ref> spawned(cones);
    return run<cones>(msg, [&](unsigned i) { spawned[i] = []() { return true; }; return true; },
                           [&](unsigned i) { return spawned[i]->wait(); },
                           [&](unsigned i) { spawned[i] = cone::ref(); return true; });
}

template <size_t cones, size_t yields_per_cone>
static bool test_spawn_many_yielding(char *msg) {
    std::vector<cone::ref> spawned(cones);
    return run<cones>(msg,
        [&](unsigned i) {
            spawned[i] = []() {
                for (size_t n = yields_per_cone; n--;)
                    if (!cone::yield())
                        return false;
                return true;
            };
            return true;
        },
        [&](unsigned i) { return spawned[i]->wait(); },
        [&](unsigned i) { spawned[i] = cone::ref(); return true; });
}

template <size_t cones, size_t yields_per_cone>
static bool test_mutex(char *msg) {
    size_t r = 0;
    cone::mutex m;
    std::vector<cone::ref> spawned(cones);
    return run<cones>(msg,
        [&](unsigned i) {
            spawned[i] = [&]() {
                for (size_t n = yields_per_cone; n--;) if (cone::mutex::guard g{m}) {
                    size_t c = r;
                    if (!cone::yield())
                        return false;
                    r = c + 1;
                } else {
                    return false;
                }
                return true;
            };
            return true;
        },
        [&](unsigned i) { return spawned[i]->wait(); },
        [&](unsigned i) { spawned[i] = cone::ref(); return true; })
     && !mun_assert(r == cones * yields_per_cone, "%zu != %zu", r, cones * yields_per_cone);
}

export { "perf:yield x 3m", &test_yield<3000000> }
     , { "perf:(spawn(nop), wait, drop) x 6m", &test_spawn<6000000> }
     , { "perf:spawn(nop) x 1m, wait x 1m, drop x 1m", &test_spawn_many<1000000> }
     , { "perf:spawn(yield x 100) x 100k, wait x 100k, drop x 100k", &test_spawn_many_yielding<100000, 100> }
     , { "perf:spawn((lock, yield, unlock) x 100) x 20k, wait x 20k, drop x 20k", &test_mutex<20000, 100> }
