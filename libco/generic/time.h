#pragma once
#include "u128.h"

#include <time.h>
#include <sys/types.h>

#define co_nsec        co_u128
#define co_nsec_offset co_u128

// maximum number of whole seconds that fits into a 64-bit nanosecond counter
#define CO_U64_SEC_MAX (0xFFFFFFFFFFFFFFFFull / 1000000000ull)

static inline struct co_nsec
co_nsec_from_timespec(struct timespec val) {
    struct co_nsec now = {};
    uint64_t sec = val.tv_sec;
    while (sec >= CO_U64_SEC_MAX) {
        now = co_u128_add(now, CO_U128(CO_U64_SEC_MAX * 1000000000ull));
        sec -= CO_U64_SEC_MAX;
    }
    return co_u128_add(now, CO_U128(sec * 1000000000ull + (uint64_t)val.tv_nsec));
}

static inline struct co_nsec
co_nsec_on(clockid_t clock) {
    struct timespec val;
    return clock_gettime(clock, &val) ? (struct co_nsec){} : co_nsec_from_timespec(val);
}

static inline struct co_nsec
co_nsec_now() {
    return co_nsec_on(CLOCK_REALTIME);
}

static inline struct co_nsec
co_nsec_monotonic() {
    return co_nsec_on(CLOCK_MONOTONIC);
}
