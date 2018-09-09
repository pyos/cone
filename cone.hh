#pragma once

#include "cone.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

extern "C" char *__cxa_demangle(const char *, char *, size_t *, int *);

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
        cone_cancel(this);
    }

    // `cone_deadline` and `cone_complete`, but in RAII form.
    auto deadline(time t) noexcept {
        struct deleter {
            mun_usec t_;

            void operator()(cone *c) noexcept {
                cone_complete(c, t_);
            }
        } d{mun_usec_chrono(t)};
        mun_cant_fail(cone_deadline(this, d.t_) MUN_RETHROW);
        return std::unique_ptr<cone, deleter>{this, d};
    }

    // Same as above, but relative to now.
    auto deadline(timedelta t) noexcept {
        return deadline(time::clock::now() + t);
    }

    // Delay cancellation and deadlines until the end of the function.
    template <bool state, typename F>
    static auto intr(F&& f) {
        if (cone_intr(state) == state)
            return f();
        auto d = [](const char *) noexcept { cone_intr(!state); };
        auto g = std::unique_ptr<const char, void(*)(const char*) noexcept>{"-", d};
        return f();
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
    static bool sleep_for(timedelta t) noexcept {
        return sleep(time::clock::now() + t);
    }

    // Get the number of currently running coroutines, nullptr if not in an event loop.
    static const std::atomic<unsigned>* count() noexcept {
        return cone_count();
    }

    struct dropper {
        void operator()(cone *c) const noexcept {
            cone_drop(c);
        }
    };

    struct ref : std::unique_ptr<cone, dropper> {
        using std::unique_ptr<cone, dropper>::unique_ptr;

        template <typename F /* = bool() */, typename G = std::remove_reference_t<F>>
        ref(F&& f, size_t stack = 100UL * 1024) noexcept {
            // XXX if F is trivially copyable and fits into one `void*`, we can pass it by value.
            reset(cone_spawn(stack, cone_bind(&invoke<G>, new G(std::forward<F>(f)))));
            mun_cant_fail(!*this MUN_RETHROW);
        }
    };

    struct thread : ref {
        using ref::ref;

        template <typename F /* = bool() */, typename G = std::remove_reference_t<F>>
        thread(F&& f, size_t stack = 100UL * 1024) noexcept {
            reset(cone_loop(stack, cone_bind(&invoke<G>, new G(std::forward<F>(f))), [](cone_closure c) {
                return std::thread{[=](){ mun_cant_fail(c.code(c.data) MUN_RETHROW); }}.detach(), 0;
            }));
            mun_cant_fail(!*this MUN_RETHROW);
        }
    };

    struct aborter {
        void operator()(cone *c) const noexcept {
            intr<false>([c]() {
                cone_cancel(c);
                cone_join(c, 1);
            });
        }
    };

    struct guard : std::unique_ptr<cone, aborter> {
        guard(ref r = {})
            : std::unique_ptr<cone, aborter>::unique_ptr(r.release())
        {
        }
    };

    struct event {
        event() noexcept {}

        event(event&& other) noexcept {
            std::swap(e_, other.e_);
        }

        event& operator=(event&& other) noexcept {
            return std::swap(e_, other.e_), *this;
        }

        bool wait() noexcept {
            return !cone_wait_if(&e_, 1);
        }

        template <bool b = true, typename F /* = bool() noexcept */>
        bool wait_if(F&& f) noexcept {
            return b ? !cone_wait_if(&e_, f()) : !cone_wait_if_not(&e_, f());
        }

        void wake(size_t n = std::numeric_limits<size_t>::max()) noexcept {
            cone_wake(&e_, n);
        }

    private:
        cone_event e_ = {};
    };

    struct mutex {
        bool try_lock() noexcept {
            return !v_.exchange(true);
        }

        bool lock() noexcept {
            if (!try_lock()) while (!e_.wait_if<false>([this]() { return try_lock(); })) {
                if (mun_errno != EAGAIN) // could still be us who got woken by unlock() though
                    return try_lock() && (unlock(), false);
                for (unsigned n = n_; n > 1 && !n_.compare_exchange_weak(n, n / 2);) {}
            }
            return true;
        }

        void unlock() noexcept {
            v_.store(false, std::memory_order_release);
            // `wake(1)` works really bad if the critical section rarely yields: if many
            // coroutines are waiting on the lock, a simple `wake()` would make
            // the loop run them one after another. `wake(1)`, meanwhile, forces the
            // next cone to wait until the run queue is drained. On the other hand,
            // if the critical section yields, `wake()` creates a thundering herd.
            unsigned n = n_;
            while (n < 2048 && !n_.compare_exchange_weak(n, n * 2)) {}
            e_.wake(n);
        }

        auto guard() noexcept {
            struct deleter {
                void operator()(mutex *m) const {
                    m->unlock();
                }
            };
            return std::unique_ptr<mutex, deleter>{lock() ? this : nullptr};
        }

    private:
        event e_;
        std::atomic<bool> v_{false};
        std::atomic<unsigned> n_{32};
    };

private:
    static inline mun_usec mun_usec_chrono(time t) noexcept {
        static const auto d = std::chrono::microseconds(mun_usec_monotonic()) - time::clock::now().time_since_epoch()
                            + std::chrono::nanoseconds(999);
        return std::chrono::duration_cast<std::chrono::microseconds>((t + d).time_since_epoch()).count();
    }

    template <typename F>
    static int invoke(void *ptr) noexcept try {
        return (*std::unique_ptr<F>(reinterpret_cast<F*>(ptr)))() ? 0 : -1;
    } catch (const std::exception& e) {
        std::unique_ptr<const char, void(*)(const void*)> name{typeid(e).name(), [](const void*) noexcept {}};
        if (char *c = __cxa_demangle(name.get(), nullptr, nullptr, nullptr))
            name = {c, [](const void *c) noexcept { free((void*)c); }};
        // XXX perhaps mun_error should contain the whole name instead of a pointer?..
        return mun_error_at(mun_errno_custom + 18293, "exception", MUN_CURRENT_FRAME, "[%s] %s", name.get(), e.what());
    } catch (...) {
        return mun_error_at(mun_errno_custom + 18293, "exception", MUN_CURRENT_FRAME, "unknown");
    }
};
