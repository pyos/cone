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

    struct deadline {
        // `cone_deadline` and `cone_complete`, but in RAII form. This object must not
        // outlive the `cone`. (Don't tell me to rewrite this in Rust.)
        deadline(cone *, time);

        // Same as above, but relative to now.
        deadline(cone *c, timedelta t)
            : deadline(c, time::clock::now() + t)
        {
        }

        ~deadline();

        deadline(const deadline&) = delete;
        deadline& operator=(const deadline&) = delete;

    private:
        cone *c_;
        time t_;
    };

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

        operator cone*() const {
            return r_.get();
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

    struct event {
        event() noexcept;
        ~event() noexcept;

        event(event&& other) noexcept
            : event()
        {
            std::swap(r_, other.r_);
        }

        cone::event& operator=(event&& other) noexcept {
            std::swap(r_, other.r_);
            return *this;
        }

        bool wait() noexcept {
            return wait(std::atomic<unsigned>{0}, 0);
        }

        bool wait(const std::atomic<unsigned>&, unsigned expect) noexcept;
        void wake(size_t n = std::numeric_limits<size_t>::max()) noexcept;

    private:
        // XXX maybe don't bother and simply include mun.h?
        std::aligned_storage_t<sizeof(void*) + sizeof(size_t) * 3, alignof(void*)> r_;
    };

    struct mutex {
        bool try_lock() noexcept {
            return v_.exchange(1) == 0;
        }

        bool lock() noexcept {
            while (!try_lock())
                if (!e_.wait(v_, 1))
                    return false;
            return true;
        }

        void unlock() noexcept {
            v_ = 0;
            e_.wake(); // XXX should only wake one (but handle cancellation)
        }

    private:
        std::atomic<unsigned> v_{0};
        event e_;
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
