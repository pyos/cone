#include "../mun.h"
#include "../cold.h"
#include "../cone.h"
#include "../cone.hh"
#include <stdio.h>
#include <stdlib.h>
#include <vector>

using namespace std::literals::chrono_literals;

#define ASSERT(x, ...) ((x) || !mun_error(assert, __VA_ARGS__))

template <size_t N, typename T = cone::ref, typename F>
static inline bool spawn_and_wait(F&& f) {
    std::vector<T> cs;
    cs.reserve(N);
    for (size_t i = 0; i < N; i++)
        cs.emplace_back(f);
    for (auto& c : cs)
        if (!c->wait())
            return false;
    return true;
}

#define export struct { const char *name; bool (*impl)(char *); } __tests[] = {
#define __s1(x) #x
#define __s2(x) __s1(x)
#include __s2(SRC)
};

int main() {
    int ret = 0;
    char buf[2048];
    for (unsigned i = 0; i < sizeof(__tests) / sizeof(*__tests); i++) {
        buf[0] = 0;
        printf("\033[33;1m * \033[0m\033[1m%s\033[0m\n", __tests[i].name);
        bool fail = !__tests[i].impl(buf);
        printf("\033[A\033[3%d;1m * \033[0m\033[1m%s\033[0m\033[K%s%s\n", 1 + !fail, __tests[i].name, buf[0] ? ": " : "", buf);
        if (fail) {
            mun_error_show("test failed with", NULL);
            ret = 1;
        }
        unsigned left = *cone_count() - 1;
        mun_assert(!left, "test left %u coroutine(s) behind", left);
    }
    return ret;
}
