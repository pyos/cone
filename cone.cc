#include "cone.hh"

#if __GNUC__
#include <cxxabi.h>
#endif

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
