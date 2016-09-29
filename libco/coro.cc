#include "coro.h"

thread_local coro::cobase* coro::current;

extern "C" int amain(int argc, const char **argv) noexcept;
extern "C" int main(int argc, const char **argv) noexcept {
    return coro::run(std::bind(amain, argc, argv));
}
