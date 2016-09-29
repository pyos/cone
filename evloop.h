#pragma once
#include "events.h"

#include <fcntl.h>
#include <assert.h>
#include <unistd.h>

#include <atomic>

namespace aio {
    static inline int unblock(int fd) noexcept {
        int flags = fcntl(fd, F_GETFL);
        return flags == -1 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    struct evloop : uncopyable {
        event on_ping;
        event on_exit;
        event::io io;
        event::time after;

        evloop() {
            int fd[2];
            if (::pipe2(fd, O_NONBLOCK))
                assert("pipe2 must not fail" && 0), abort();
            ping_r = fd[0];
            ping_w = fd[1];
            io[ping_r].read += &consume_ping;
        }

        ~evloop() {
            on_exit();
            ::close(ping_r);
            ::close(ping_w);
        }

        int run() noexcept {
            assert("aio::evloop::run must not call itself" && !running);
            for (running.store(true, std::memory_order_release); running.load(std::memory_order_acquire);)
                if (io(after()) && errno != EINTR)
                    return -1;
            return 0;
        }

        const event::callback stop = [this]() noexcept {
            running.store(false, std::memory_order_release);
            ping();
        };

        const event::callback ping = [this]() noexcept {
            bool expect = false;
            if (pinged.compare_exchange_strong(expect, true, std::memory_order_acquire))
                ::write(ping_w, "", 1);  // never yields
        };

    private:
        const event::callback consume_ping = [this]() noexcept {
            ssize_t rd = ::read(ping_r, &rd, sizeof(rd));  // never yields
            io[ping_r].read += &consume_ping;
            pinged.store(false, std::memory_order_release);
            on_ping();
        };

        int ping_r, ping_w;
        std::atomic_bool running{false}, pinged{false};
    };
};
