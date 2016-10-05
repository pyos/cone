#include "../cone.h"

#include <sched.h>
#include <stdio.h>

static const int YIELDS = 1000000;

int comain() {
    mun_nsec a = mun_nsec_monotonic();
    for (int i = 0; i < YIELDS; i++)
        sched_yield();
    mun_nsec b = mun_nsec_monotonic();
    printf("%f ns/switch\n", mun_u128_to_double(mun_u128_sub(b, a)) / YIELDS);
    return 0;
}
