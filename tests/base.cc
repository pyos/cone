#include "../mun.h"
#include "../cold.h"
#include "../cone.h"
#include "../cone.hh"
#include <stdio.h>
#include <stdlib.h>
#include <regex>
#include <vector>

using namespace std::literals::chrono_literals;

struct __test { const char *name; bool (*impl)(); };
extern std::vector<struct __test> __tests;
static char __msg[2048];

#define export std::vector<struct __test> __tests =
#define ASSERT(x, ...) ((x) || !mun_error(EINVAL, __VA_ARGS__))
#define INFO(...) (snprintf(__msg, sizeof(__msg), __VA_ARGS__), true)

template <typename T = cone::ref, typename F>
static inline bool spawn_and_wait(size_t n, F&& f) {
    std::vector<T> cs;
    cs.reserve(n);
    for (size_t i = 0; i < n; i++)
        cs.emplace_back(f);
    for (auto& c : cs)
        if (!c->wait(cone::rethrow) MUN_RETHROW)
            return false;
    return true;
}

int main(int argc, const char **argv) {
    std::vector<std::regex> match(argv + !!argc, argv + argc);
    int ret = 0;
    int ran = 0;
    for (const auto& t : __tests) {
        if (!match.empty() && !std::any_of(match.begin(), match.end(), [&](auto& re) { return std::regex_match(t.name, re); }))
            continue;
        __msg[0] = 0;
        printf("\033[33;1m * \033[0m\033[1m%s\033[0m\n", t.name);
        bool fail = !t.impl();
        printf("\033[A\033[3%d;1m * \033[0m\033[1m%s\033[0m\033[K%s%s\n", 1 + !fail, t.name, __msg[0] ? ": " : "", __msg);
        if (fail) {
            mun_error_show("test failed with", NULL);
            ret = 1;
        }
        unsigned left = *cone_count() - 1;
        mun_assert(!left, "test left %u coroutine(s) behind", left);
        ran++;
    }
    if (ASSERT(ran == 0, "no tests matched the provided regexes")) {
        mun_error_show("input", NULL);
        return 2;
    }
    return ret;
}
