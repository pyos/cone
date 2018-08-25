#pragma once

#include "cone.h"

#include <atomic>
#include <chrono>
#include <memory>

// `cone` is actually an opaque type defined in cone.c, but who cares? That's a different unit!
struct cone {
    using time = std::chrono::steady_clock::time_point;
    using timedelta = std::chrono::steady_clock::duration;

    // Sleep until this coroutine finishes, optionally returning any error it finishes with.
    bool wait(bool rethrow = true) noexcept {
        return !cone_cowait(this, !rethrow);
    }

    // Make the next (or current, if any) call to `wait`, `iowait`, `sleep_until`, `sleep`,
    // or `yield` from this coroutine fail with ECANCELED. No-op if the coroutine has finished.
    void cancel() noexcept {
        mun_cant_fail(cone_cancel(this) MUN_RETHROW);
    }

    struct deadline {
        // `cone_deadline` and `cone_complete`, but in RAII form. This object must not
        // outlive the `cone`. (Don't tell me to rewrite this in Rust.)
        deadline(cone *c, time t)
            : c_(c)
            , t_(t)
        {
            mun_cant_fail(cone_deadline(c_, mun_usec_chrono(t_)) MUN_RETHROW);
        }

        // Same as above, but relative to now.
        deadline(cone *c, timedelta t)
            : deadline(c, time::clock::now() + t)
        {
        }

        ~deadline() {
            cone_complete(c_, mun_usec_chrono(t_));
        }

        deadline(const deadline&) = delete;
        deadline& operator=(const deadline&) = delete;

    private:
        cone *c_;
        time t_;
    };

    // Wait until the next iteration of the event loop.
    static bool yield() noexcept {
        return !cone_yield();
    }

    // Wait until a specified point in time.
    static bool sleep(time t) noexcept {
        return !cone_sleep_until(mun_usec_chrono(t));
    }

    // Wait for some time.
    static bool sleep(timedelta t) noexcept {
        return sleep(time::clock::now() + t);
    }

    // Get a reference to the number of currently running coroutines.
    static const std::atomic<unsigned>& count() noexcept {
        mun_assert(::cone, "not running in a coroutine");
        return *cone_count();
    }

    // Convert a C++ exception that is being handled into a mun error.
    static void exception_to_error() noexcept;

    struct ref {
        ref(cone *c = nullptr) noexcept
            : r_(c, [](cone *) noexcept {})
        {
        }

        template <typename F /* = bool() */, typename G = std::remove_reference_t<F>>
        ref(F&& f, size_t stack = 100UL * 1024)
            // XXX if F is trivially copyable and fits into one `void*`, we can pass it by value.
            : r_(cone_spawn(stack, cone_bind(&invoke<G>, new G(std::forward<F>(f)))), cone_drop)
        {
            mun_cant_fail(!r_.get() MUN_RETHROW);
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
        template <typename F>
        static int invoke(void *ptr) noexcept {
            try {
                return (*std::unique_ptr<F>(reinterpret_cast<F*>(ptr)))() ? 0 : -1;
            } catch (...) {
                return exception_to_error(), -1;
            }
        }

        std::unique_ptr<cone, void (*)(cone*) noexcept> r_;
    };

    struct event {
        event() noexcept = default;

        ~event() noexcept {
            mun_vec_fini(&e_);
        }

        event(event&& other) noexcept {
            std::swap(e_, other.e_);
        }

        event& operator=(event&& other) noexcept {
            return std::swap(e_, other.e_), *this;
        }

        bool wait() noexcept {
            return wait(std::atomic<unsigned>{0}, 0);
        }

        bool wait(const std::atomic<unsigned>& atom, unsigned expect) noexcept {
            return !cone_wait(&e_, &atom, expect);
        }

        void wake(size_t n = std::numeric_limits<size_t>::max()) noexcept {
            mun_cant_fail(cone_wake(&e_, n) MUN_RETHROW);
        }

    private:
        cone_event e_ = {};
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
    static inline mun_usec mun_usec_chrono(time t) noexcept {
        static const auto d = std::chrono::microseconds(mun_usec_monotonic()) - time::clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::microseconds>((t + d).time_since_epoch()).count();
    }
};
