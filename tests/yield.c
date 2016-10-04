#include "../cone.h"

#include <sched.h>
#include <stdio.h>

static const int YIELDS = 1000000;

int comain() {
    veil_nsec a = veil_nsec_monotonic();
    for (int i = 0; i < YIELDS; i++)
        sched_yield();
    veil_nsec b = veil_nsec_monotonic();
    printf("%f ns/switch\n", veil_u128_to_double(veil_u128_sub(b, a)) / YIELDS);
    return 0;
}
