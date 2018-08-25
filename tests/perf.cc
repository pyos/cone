#include <vector>

template <unsigned N = 1000000, typename F>
static bool run(char *msg, F&& f) {
    auto a = cone::time::clock::now();
    for (unsigned i = 0; i < N; i++)
        if (!f())
            return false;
    auto b = cone::time::clock::now();
    sprintf(msg, "%f us/iter", (double)std::chrono::duration_cast<std::chrono::microseconds>(b - a).count() / N);
    return true;
}

static bool test_yield(char *msg) {
    return run(msg, cone::yield);
}

static bool test_spawn(char *msg) {
    return run(msg, []() { return cone::ref{[](){ return true; }}->wait(); });
}

static bool test_spawn_many(char *msg) {
    std::vector<cone::ref> spawned;
    spawned.reserve(1000000);
    bool ok = run(msg, [&]() { spawned.emplace_back([]() { return true; }); return true; });
    spawned.back()->wait();
    return ok;
}

export { "perf:yield", &test_yield }
     , { "perf:spawn", &test_spawn }
     , { "perf:spawn many", &test_spawn_many }
