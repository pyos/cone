#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "../libco/coro.h"

#include <sched.h>

//static const int COROS = 1000000;
static const int YIELDS_PER_CORO = 1000000;

int amain() {
    struct co_nsec a = co_nsec_monotonic();
    for (int i = 0; i < YIELDS_PER_CORO; i++)
        sched_yield();
    struct co_nsec b = co_nsec_monotonic();
    double r = co_u128_to_double(co_u128_sub(b, a)) / YIELDS_PER_CORO;
    printf("%f ns/switch\n", r);
    return 0;
}
