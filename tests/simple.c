#include "../cone.h"

#include <sched.h>
#include <unistd.h>

static int c1() {
    char data[1024];
    ssize_t size = read(0, data, sizeof(data));
    if (size < 0 || sched_yield() || write(1, data, size) < 0)
        return veil_error_os();
    return veil_ok;
}

static int c2() {
    for (int i = 0; i < 3; i++) {
        sleep(i);
        if (write(1, "Hello, World!\n", 14) < 0)
            return veil_error_os();
    }
    return veil_ok;
}

int comain() {
    cone_unblock(0);
    cone_unblock(1);
    if (cone_decref(cone(&c1, NULL)) || cone_decref(cone(&c2, NULL)))
        return veil_error_up();
    return 0;
}
