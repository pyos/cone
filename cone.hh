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
    auto timeout(timedelta t) noexcept {
        return deadline(time::clock::now() + t);
    }

    // Same as above, but limited to a single function call.
    template <typename F>
    auto deadline(time t, F&& f) noexcept {
        auto d = deadline(t);
        return f();
    }

    template <typename F>
    auto timeout(timedelta t, F&& f) noexcept {
        auto d = timeout(t);
        return f();
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
        ref() = default;

        template <typename F /* = bool() */, typename G = std::remove_reference_t<F>,
                  typename = std::enable_if_t<!std::is_base_of<ref, G>::value>>
        ref(F&& f, size_t stack = 100UL * 1024) noexcept {
            // XXX if F is trivially copyable and fits into one `void*`, we can pass it by value.
            reset(cone_spawn(stack, cone_bind(&invoke<G>, new G(std::forward<F>(f)))));
            mun_cant_fail(!*this MUN_RETHROW);
        }
    };

    struct thread : ref {
        thread() = default;

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
        template <typename... Args>
        guard(Args&&... args)
            : std::unique_ptr<cone, aborter>::unique_ptr(cone::ref{std::forward<Args>(args)...}.release())
        {
        }
    };

    struct event : cone_event {
        event() noexcept : cone_event{} {}
        event(const event&) = delete;
        event& operator=(const event&) = delete;

        bool wait() noexcept {
            return !cone_wait(this, 1);
        }

        template <typename F /* = bool() noexcept */>
        bool wait_if(F&& f) noexcept {
            return !cone_wait(this, f());
        }

        // TODO valued `wake`.

        size_t wake(size_t n = std::numeric_limits<size_t>::max()) noexcept {
            return cone_wake(this, n, 0);
        }
    };

    struct mutex : cone_mutex {
        mutex() noexcept : cone_mutex{} {}
        mutex(const mutex&) = delete;
        mutex& operator=(const mutex&) = delete;

        bool try_lock() noexcept {
            return !cone_try_lock(this);
        }

        void lock() noexcept {
            intr<false>([this]() { lock_cancellable(); });
        }

        bool lock_cancellable() noexcept {
            return !cone_lock(this);
        }

        bool unlock(bool fair = false) noexcept {
            return cone_unlock(this, fair);
        }

        auto guard(bool cancellable = true) noexcept {
            struct deleter {
                void operator()(mutex *m) const {
                    m->unlock();
                }
            };
            return std::unique_ptr<mutex, deleter>{!cancellable ? (lock(), this) : lock_cancellable() ? this : nullptr};
        }
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
