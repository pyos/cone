#pragma once
#include "evloop.h"

#include <stdio.h>
#include <string.h>

#if !defined(COROUTINE_X86_64_SYSV_CTX) && defined(__linux__) && defined(__x86_64__)
#define COROUTINE_X86_64_SYSV_CTX 1
#endif

#if !COROUTINE_X86_64_SYSV_CTX
#include <ucontext.h>
#endif

#include <thread>
#include <memory>
#include <unordered_map>

namespace coro {
    extern thread_local struct cobase* current;

    struct context {
        aio::event::callback body;

        template <typename F>
        context(char *stack, size_t size, F&& body) : body(std::forward<F>(body)) {
            #if COROUTINE_X86_64_SYSV_CTX
                memset(regs[1] = stack + size - 8, 0, 8);  // point the return address at 0
            #else
                getcontext(&inner);
                inner.uc_stack.ss_sp = stack;
                inner.uc_stack.ss_size = size;
                makecontext(&inner, (void(*)())&__run, 1, this);
            #endif
        }

    #if COROUTINE_X86_64_SYSV_CTX
        void *regs[4] = {0, 0, (void*)&__run, this};
        void enter() noexcept { leave(); }
        void leave() noexcept {
            __asm__ volatile (
                "lea LJMPRET%=(%%rip), %%rcx\n"
                #define XCHG(a, b, tmp) "mov " a ", " tmp " \n mov " b ", " a " \n mov " tmp ", " b "\n"
                XCHG("%%rbp",  "0(%0)", "%%r8")  // gcc complains about clobbering %rbp with -fno-omit-frame-pointer
                XCHG("%%rsp",  "8(%0)", "%%r9")
                XCHG("%%rcx", "16(%0)", "%%r10")  // `xchg` is implicitly `lock`ed => slower than 3 `mov`s
                XCHG("%%rdi", "24(%0)", "%%r11")
                #undef XCHG
                "jmp *%%rcx\n"
                "LJMPRET%=:"
              :
              : "a"(regs)
              : "rbx", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
                "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9",
                "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "cc" // not "memory" (so don't read `regs`)
            );
        }
    #else
        ucontext_t inner, outer;
        void enter() noexcept { swapcontext(&outer, &inner); }
        void leave() noexcept { swapcontext(&inner, &outer); }
    #endif

        [[noreturn]] static void __run(context* t) noexcept {
            t->body();
            t->leave();
            abort();
        }
    };

    struct coloop : aio::evloop {
        std::unordered_map<std::shared_ptr<cobase>, aio::event::callback> coros;
    };

    struct cobase : std::enable_shared_from_this<cobase>, uncopyable {
        coloop* loop;
        aio::event done;

        template <typename F>
        cobase(coloop *loop, size_t size, F&& body) : loop(loop), state((char*)this, size, std::forward<F>(body)) {
            schedule();
        }

        virtual ~cobase() {};

        template <bool interruptible = true, typename evt>
        void pause(evt&& until) {
            until += &schedule;
            state.leave();  // TODO cancellation
        }

        void join() {
            if (loop) current->pause(done);
        }

    protected:
        context state;

        const aio::event::callback schedule = [this]() noexcept {
            loop->after[std::chrono::seconds(0)] += &run;
        };

        const aio::event::callback run = [this]() noexcept {
            cobase* preempted = current;
            current = this;
            state.enter();
            current = preempted;
        };
    };

    template <typename Ret> struct f : cobase {
        Ret result;
        template <typename F>
        f(coloop *loop, size_t size, F&& body) : cobase(loop, size, [this, body]() {
            this->result = std::move(body());
            this->done.move_to(this->loop->after[std::chrono::seconds(0)]);
            this->loop = nullptr;
        }) {}
    };

    template <> struct f<void> : cobase {
        template <typename F>
        f(coloop *loop, size_t size, F&& body) : cobase(loop, size, [this, body]() {
            body();
            this->done.move_to(this->loop->after[std::chrono::seconds(0)]);
            this->loop = nullptr;
        }) {}
    };

    template <typename F, typename C = f<typename std::result_of<F()>::type>>
    static inline std::shared_ptr<C> spawn(F&& body, coloop *loop = current->loop, size_t stack_size = 65536) {
        stack_size &= ~(size_t)15;
        std::shared_ptr<C> a{new (::operator new(stack_size)) C(loop, stack_size, std::forward<F>(body))};
        a->done += &loop->coros.emplace(std::static_pointer_cast<cobase>(a), [loop, a](){ loop->coros.erase(a); }).first->second;
        return a;
    }

    template <typename F>
    static inline auto __run(F&& main) -> std::shared_ptr<f<decltype(main())>> {
        coloop loop;
        auto root = spawn(std::forward<F>(main), &loop);
        root->done += &loop.stop;
        while (!loop.run()) {
            if (loop.coros.empty())
                return root;
            loop.coros.begin()->first->done += &loop.stop;
        }
        fprintf(stderr, "[coro::coloop] FATAL: %s (%d)\n", strerror(errno), errno);
        abort();  // there are threads stuck in indeterminate state, might as well crash
    }

    template <typename F>
    static inline auto run(F&& main) -> decltype(f<decltype(main())>::result) {
        return std::move(__run(std::forward<F>(main))->result);
    }

    template <typename F>
    static inline auto run(F&& main) -> typename std::enable_if<std::is_same<decltype(main()), void>::value>::type {
        __run(std::forward<F>(main));
    }

    template <typename F, typename... Args>
    static inline std::thread thread(F&& f) {
        return std::thread(run<F>, std::forward<F>(f));
    }
};
