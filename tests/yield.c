#include "../cone.h"

#include <sched.h>
#include <stdio.h>

static const int YIELDS = 1000000;

int comain() {
    cot_nsec a = cot_nsec_monotonic();
    for (int i = 0; i < YIELDS; i++)
        sched_yield();
    cot_nsec b = cot_nsec_monotonic();
    printf("%f ns/switch\n", cot_u128_to_double(cot_u128_sub(b, a)) / YIELDS);
    return 0;
}
