#pragma once
// C++ bindings for cone. Some notes:
//
//   * Make sure to link with libcxxcone, not libcone (or use -DCONE_CXX=1 when building)
//     for coroutine-local exception state. Otherwise, yielding while in a destructor
//     or a catch block will lead to Really Bad Stuff. (I'd say "undefined behavior",
//     but this whole thing is extremely undefined as it is.)
//
//   * Where cone and mun use the libc convention of returning an int {-1, 0} for signalling
//     {error, success}, C++ bindings instead return a C++ style boolean {false, true}.
//
//   * Some functionality from the C API, most notably `mun_error` and valued `cone_wake`,
//     is not wrapped. On the other hand, there is some stuff that is C++-only, e.g guards.
//
#include "cone.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

extern "C" char *__cxa_demangle(const char *, char *, size_t *, int *);

// `cone` is actually an opaque type defined in cone.c, but who cares? That's a different unit!
struct cone {
    // mun uses the monotonic clock, so here it is:
    using time = std::chrono::steady_clock::time_point;
    using timedelta = std::chrono::steady_clock::duration;

    // Sleep until this coroutine finishes, optionally returning its error. See `cone_cowait`.
    // Requires that an owning reference (see `ref` below) to this coroutine exists for the
    // duration of the sleep, else the behavior on return will be extra undefined.
    bool wait(bool rethrow = true) noexcept {
        return !cone_cowait(this, !rethrow);
    }

    // Make the next (or current, if any) call to `wait`, `iowait`, `sleep_until`, `sleep`,
    // or `yield` from this coroutine fail with ECANCELED. See `cone_cancel`. Only requires
    // an owning reference if called on a coroutine in a different thread; always memory-
    // safe otherwise.
    void cancel() noexcept {
        cone_cancel(this);
    }

    // `cone_deadline` and `cone_complete`, but in RAII form. Passing `time::max()`
    // as an argument makes this method a no-op, i.e. it returns some empty object.
    auto deadline(time t) noexcept {
        struct deleter {
            mun_usec t_;

            void operator()(cone *c) noexcept {
                cone_complete(c, t_);
            }
        };
        if (t == time::max())
            return std::unique_ptr<cone, deleter>{nullptr, deleter{0}};
        mun_cant_fail(cone_deadline(this, mun_usec_chrono(t)) MUN_RETHROW);
        return std::unique_ptr<cone, deleter>{this, deleter{mun_usec_chrono(t)}};
    }

    // Same as above, but relative to now. `timedelta::max()` is a no-op.
    auto timeout(timedelta t) noexcept {
        return deadline(t == timedelta::max() ? time::max() : time::clock::now() + t);
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

    // Enable or disable interruptions while calling the provided function. While
    // interruptions are disabled, blocking operations cannot time out or be cancelled.
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

    // Get the current scheduling delay, nullptr if not in an event loop.
    static const std::atomic<mun_usec>* delay() noexcept {
        return cone_delay();
    }

    struct dropper {
        void operator()(cone *c) const noexcept {
            cone_drop(c);
        }
    };

    // An owning reference to a coroutine.
    struct ref : std::unique_ptr<cone, dropper> {
        ref() = default;

        // Spawn a new coroutine that will call the given function without arguments.
        template <typename F /* = bool() */,
                  typename G = std::enable_if_t<std::is_invocable_r<bool, F>::value, std::remove_reference_t<F>>>
        ref(F&& f, size_t stack = 100UL * 1024) noexcept {
            // XXX if F is trivially copyable and fits into one `void*`, we can pass it by value.
            reset(cone_spawn(stack, cone_bind(&invoke<G>, new G(std::forward<F>(f)))));
            mun_cant_fail(!*this MUN_RETHROW);
        }
    };

    // An owning reference to a coroutine in a separate thread. (Upcasting is OK.)
    struct thread : ref {
        thread() = default;

        // Same as `ref`'s constructor, but also create a new thread for the coroutine.
        // Any coroutine it spawns itself through `ref` will also be on that thread.
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

    // An owning reference that cancels and uninterruptibly waits for a coroutine when
    // going out of scope. This ensures that when other locally owned resources are
    // destroyed, the coroutine has already finished executing.
    struct guard : std::unique_ptr<cone, aborter> {
        template <typename... Args>
        guard(Args&&... args)
            : std::unique_ptr<cone, aborter>::unique_ptr(cone::ref{std::forward<Args>(args)...}.release())
        {
        }
    };

    // A list of coroutines from which they remove themselves after terminating. Destroying
    // the list also cancels and uninterruptibly waits for all still-active coroutines.
    struct mguard {
        mguard() = default;
        mguard(mguard&& other) : mguard() { std::swap(fake_, other.fake_); }
        mguard& operator=(mguard&& other) { std::swap(fake_, other.fake_); return *this; }

        ~mguard() {
            intr<false>([this] {
                cancel();
                while (fake_->next_ != fake_.get())
                    ref{std::move(fake_->next_->r_)}->wait(/*rethrow=*/false);
            });
        }

        // Spawn a new coroutine and add it to the list. The returned reference is
        // non-owning; see `wait` above for the implications.
        template <typename F>
        cone* add(F&& f) {
            std::unique_ptr<node> n{new node};
            n->next_ = fake_->next_;
            n->prev_ = fake_.get();
            fake_->next_ = fake_->next_->prev_ = n.get();
            fake_->next_->r_ = [n = std::move(n), f = std::forward<F>(f)]() mutable { return f(); };
            return fake_->next_->r_.get();
        }

        // The number of spawned coroutines that have not yet terminated.
        size_t active() const noexcept {
            size_t i = 0;
            for (node* n = fake_->next_; n != fake_.get(); n = n->next_)
                i++;
            return i;
        }

        // `active() != 0`, but faster.
        bool empty() const noexcept {
            return fake_->next_ == fake_.get();
        }

        // Cancel all still running tasks.
        void cancel() noexcept {
            for (node* n = fake_->next_; n != fake_.get(); n = n->next_)
                if (n->r_)
                    n->r_->cancel();
        }

    private:
        struct node {
            node* next_ = this;
            node* prev_ = this;
            ref r_;

            ~node() {
                next_->prev_ = prev_;
                prev_->next_ = next_;
            }
        };

        std::unique_ptr<node> fake_{new node};
    };

    // Something that can be waited for.
    struct event : cone_event {
        event() noexcept : cone_event{} {}
        event(const event&) = delete;
        event& operator=(const event&) = delete;

        // Sleep until the event happens.
        bool wait() noexcept {
            return !cone_wait(this, 1);
        }

        // If the provided function returns `true`, sleep until the event happens,
        // else successfully return immediately. This operation is atomic.
        template <typename F /* = bool() noexcept */>
        bool wait_if(F&& f) noexcept {
            return !cone_wait(this, f());
        }

        // Wake at most `n` coroutines currently waiting for this event.
        size_t wake(size_t n = std::numeric_limits<size_t>::max()) noexcept {
            // TODO valued `wake`.
            return cone_wake(this, n, 0);
        }
    };

    // A coroutine-blocking mutex.
    struct mutex : cone_mutex {
        mutex() noexcept : cone_mutex{} {}
        mutex(const mutex&) = delete;
        mutex& operator=(const mutex&) = delete;

        // Try to acquire the lock, else fail with EAGAIN.
        bool try_lock() noexcept {
            return !cone_try_lock(this);
        }

        // Uninterruptibly acquire the lock. Mimics `std::mutex::lock`.
        void lock() noexcept {
            intr<false>([this]() { lock_cancellable(); });
        }

        // Interruptibly acquire the lock (i.e. this method can be cancelled or time out).
        bool lock_cancellable() noexcept {
            return !cone_lock(this);
        }

        // Release the lock. If `fair` is true and there are coroutines waiting to acquire
        // it, make sure the one that called `lock`/`lock_cancellable` first gets it.
        template <bool fair = false>
        bool unlock() noexcept {
            return cone_unlock(this, fair);
        }

        // Acquire the lock and return an object that releases it when destroyed.
        template <bool fair = false>
        auto guard(bool cancellable = true) noexcept {
            struct deleter {
                void operator()(mutex *m) const {
                    m->unlock<fair>();
                }
            };
            return std::unique_ptr<mutex, deleter>{!cancellable ? (lock(), this) : lock_cancellable() ? this : nullptr};
        }
    };

    // An object that allows coroutines to pass when the required number of them are ready.
    struct barrier {
        barrier(size_t n) noexcept : v_(n) {}

        // Wait until the rest of the `n` coroutines call this method. If more than `n`
        // do, behavior is undefined. (TODO make it so that the rest pass unhindered.)
        bool join() noexcept {
            if (!--v_)
                e_.wake();
            else while (v_.load(std::memory_order_acquire))
                if (!e_.wait_if([&]{ return v_.load(std::memory_order_acquire) != 0; }))
                    return false;
            return true;
        }

    private:
        event e_;
        std::atomic<size_t> v_;
    };

    // Evaluate the provided function, which should return a boolean indicating success,
    // and convert any exceptions thrown into mun errors (and return `false`).
    template <typename F /* = bool() */>
    static bool try_mun(F&& f) noexcept try {
        return f();
    } catch (const std::exception& e) {
        std::unique_ptr<const char, void(*)(const void*)> name{typeid(e).name(), [](const void*) noexcept {}};
        if (char *c = __cxa_demangle(name.get(), nullptr, nullptr, nullptr))
            name = {c, [](const void *c) noexcept { free((void*)c); }};
        // XXX perhaps mun_error should contain the whole name instead of a pointer?..
        return !mun_error(EEXCEPTION, "[%s] %s", name.get(), e.what());
    } catch (...) {
        return !mun_error(EEXCEPTION, "unknown");
    }

private:
    static inline mun_usec mun_usec_chrono(time t) noexcept {
        static const auto d = std::chrono::microseconds(mun_usec_monotonic()) - time::clock::now().time_since_epoch()
                            + std::chrono::nanoseconds(999);
        return std::chrono::duration_cast<std::chrono::microseconds>((t + d).time_since_epoch()).count();
    }

    template <typename F>
    static int invoke(void *ptr) noexcept {
        return try_mun([&] { return (*std::unique_ptr<F>(reinterpret_cast<F*>(ptr)))(); }) ? 0 : -1;
    }
};
