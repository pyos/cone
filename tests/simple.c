#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "coro.h"

#include <sched.h>
#include <string.h>

static int c1() {
    char data[1024];
    ssize_t size = read(0, data, sizeof(data));
    sched_yield();
    write(1, data, size);
    return 0;
}

static int c2() {
    for (int i = 0; i < 3; i++) {
        sleep(i);
        const char data[] = "Hello, World!\n";
        write(1, data, strlen(data));
    }
    return 0;
}

int amain() {
    setnonblocking(0);
    setnonblocking(1);
    coro_decref(coro_spawn(co_callback_bind(&c1, NULL), 0));
    coro_decref(coro_spawn(co_callback_bind(&c2, NULL), 0));
    return 0;
}
