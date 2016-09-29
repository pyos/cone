#include "coro.h"

#include <sched.h>
#include <string.h>

extern "C" int amain() noexcept {
    aio::unblock(0);
    aio::unblock(1);
    coro::spawn([]() {
        char data[1024];
        auto size = read(0, data, sizeof(data));
        sched_yield();
        write(1, data, size);
    });
    coro::spawn([]() {
        for (int i = 0; i < 3; i++) {
            sleep(i);
            const char data[] = "Hello, World!\n";
            write(1, data, strlen(data));
        }
    });
    return 0;
}
