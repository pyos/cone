#include "../cone.h"

#include <sched.h>

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
        write(1, "Hello, World!\n", 14);
    }
    return 0;
}

int comain() {
    cone_unblock(0);
    cone_unblock(1);
    cone_decref(cone(&c1, NULL));
    cone_decref(cone(&c2, NULL));
    return 0;
}
