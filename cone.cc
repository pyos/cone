#include "cone.h"
#include "cone.hh"

#if __GNUC__
#include <cxxabi.h>
#endif

#if !CONE_CXX
static_assert(0, "need -DCONE_CXX=1 (when building cone.c too!) for coroutine-local exceptions");
#endif

bool cone::wait(bool rethrow) noexcept {
    return !cone_cowait(this, !rethrow);
}

void cone::cancel() noexcept {
    mun_cant_fail(cone_cancel(this) MUN_RETHROW);
}

bool cone::yield() noexcept {
    return !cone_yield();
}

static uint64_t usec(cone::time t) {
    return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count(); // wut
}

bool cone::sleep(cone::time t) noexcept {
    static const uint64_t diff = mun_usec_monotonic() - usec(cone::time::clock::now());
    return !cone_sleep_until(usec(t) + diff);
}

const std::atomic<unsigned>& cone::count() noexcept {
    mun_assert(::cone, "not running in a coroutine");
    return *cone_count();
}

static const std::unique_ptr<const char, void(*)(const void*)> demangle(const char *name) {
    #if __GNUC__
        int status;
        if (char *c = __cxxabiv1::__cxa_demangle(name, nullptr, nullptr, &status))
            return {c, [](const void *c) noexcept { free((void*)c); }};
    #endif
    return {name, [](const void*) noexcept {}};
}

void cone::exception_to_error() noexcept {
    static constexpr int mun_errno_exception = mun_errno_custom + 18293;
    if (auto e = std::current_exception()) try {
        std::rethrow_exception(e);
    } catch (const std::exception& e) {
        mun_error(exception, "[%s] %s", demangle(typeid(e).name()).get(), e.what());
    } catch (...) {
        mun_error(exception, "[not std::exception]");
    } else {
        mun_error(assert, "no active exception");
    }
}

cone::ref::ref(int (*f)(void*), void *data, size_t stack) noexcept
    : r_(cone_spawn(stack, cone_bind(f, data)), [](cone *c) noexcept { cone_drop(c); })
{
    mun_cant_fail(!r_.get() MUN_RETHROW);
}
