#include "../cone.h"

#include <sched.h>
#include <stdio.h>

static const int YIELDS = 1000000;

int comain() {
    struct cone_nsec a = cone_nsec_monotonic();
    for (int i = 0; i < YIELDS; i++)
        sched_yield();
    struct cone_nsec b = cone_nsec_monotonic();
    printf("%f ns/switch\n", cone_u128_to_double(cone_u128_sub(b, a)) / YIELDS);
    return 0;
}
