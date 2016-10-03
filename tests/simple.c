#include "../cone/cone.h"
#include "../cone/coil.h"

#include <sched.h>

static int c1() {
    char data[1024];
    ssize_t size = coil_read(0, data, sizeof(data));
    coil_sched_yield();
    coil_write(1, data, size);
    return 0;
}

static int c2() {
    for (int i = 0; i < 3; i++) {
        coil_sleep(i);
        coil_write(1, "Hello, World!\n", 14);
    }
    return 0;
}

int comain() {
    setnonblocking(0);
    setnonblocking(1);
    cone_decref(cone(&c1, NULL));
    cone_decref(cone(&c2, NULL));
    return 0;
}
