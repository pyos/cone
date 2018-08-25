#pragma once

#include <atomic>
#include <chrono>
#include <memory>

// `cone` is actually an opaque type defined in cone.c, but who cares? That's a different unit!
struct cone {
    using time = std::chrono::steady_clock::time_point;
    using timedelta = std::chrono::steady_clock::duration;

    // Note that not all of this is committed to memory if you don't use the whole stack.
    static constexpr size_t default_stack = 100L * 1024;

    // Sleep until this coroutine finishes, optionally returning any error it finishes with.
    bool wait(bool rethrow = true) noexcept;

    // Make the next (or current, if any) call to `wait`, `iowait`, `sleep_until`, `sleep`,
    // or `yield` from this coroutine fail with ECANCELED. No-op if the coroutine has finished.
    void cancel() noexcept;

    // Wait until the next iteration of the event loop.
    static bool yield() noexcept;

    // Wait until a specified point in time.
    static bool sleep(time) noexcept;

    // Wait for some time.
    static bool sleep(timedelta t) noexcept {
        return sleep(time::clock::now() + t);
    }

    // Get a reference to the number of currently running coroutines.
    static const std::atomic<unsigned>& count() noexcept;

    // Convert a C++ exception that is being handled into a mun error.
    static void exception_to_error() noexcept;

    struct ref {
        ref(cone *c = nullptr) noexcept
            : r_(c, [](cone *) noexcept {})
        {
        }

        template <typename F /* = bool() */>
        ref(F&& f, size_t stack = default_stack)
            : ref(std::make_unique<std::remove_reference_t<F>>(std::forward<F>(f)), stack)
        {
        }

        cone& operator*() const {
            return *r_;
        }

        cone* operator->() const {
            return r_.get();
        }

    private:
        ref(int (*)(void*), void *, size_t stack) noexcept;

        template <typename F, typename D>
        ref(std::unique_ptr<F, D> f, size_t stack = default_stack)
            : ref(&invoke<F, D>, f.get(), stack)
        {
            (void)f.release();
        }

    private:
        std::unique_ptr<cone, void (*)(cone*) noexcept> r_;
    };

private:
    template <typename F, typename D>
    static int invoke(void *ptr) noexcept {
        try {
            return (*std::unique_ptr<F, D>(reinterpret_cast<F*>(ptr)))() ? 0 : -1;
        } catch (...) {
            return exception_to_error(), -1;
        }
    }
};
