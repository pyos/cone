#include "coro.h"

#include <chrono>
#include <sched.h>

//static const int COROS = 1000000;
static const int YIELDS_PER_CORO = 1000000;

extern "C" int amain() noexcept {
    using clock = std::chrono::steady_clock;
    auto a = clock::now();
    for (int i = 0; i < YIELDS_PER_CORO; i++)
        sched_yield();
    auto b = clock::now();
    double r = std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count() / (double)YIELDS_PER_CORO;
    printf("%f ns/switch\n", r);
    return 0;
}
