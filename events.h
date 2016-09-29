#pragma once
#include <assert.h>

#include <queue>
#include <chrono>
#include <vector>
#include <stdexcept>
#include <functional>
#include <unordered_set>

struct uncopyable {
    uncopyable() = default;
    uncopyable(const uncopyable&) = delete;
    uncopyable& operator=(const uncopyable&) = delete;
};

#define HASHABLE_BY_FIELD(T, field)                                                        \
    struct eq {                                                                            \
        bool operator()(const T& a, const T& b) const noexcept {                           \
            return a.field == b.field;                                                     \
        }                                                                                  \
    };                                                                                     \
    struct hash {                                                                          \
        size_t operator()(const T& a) const noexcept {                                     \
            return std::hash<typename std::remove_cv<decltype(a.field)>::type>()(a.field); \
        }                                                                                  \
    };

namespace aio {
    struct event : uncopyable {
        using callback = std::function<void() noexcept>;

        void operator+=(const callback *c) {
            cs.push_back(c);
        }

        void operator-=(const callback *c) noexcept {
            for (auto it = cs.begin(); it != cs.end();)
                it = *it == c ? cs.erase(it) : it + 1;
        }

        void operator()() noexcept {
            while (!cs.empty())
                for (auto c : std::vector<const callback *>(std::move(cs)))
                    (*c)();
        }

        template <typename evt>
        void move_to(evt&& ev) {
            for (auto c : std::vector<const callback *>(std::move(cs)))
                ev += c;
        }

        struct io;
        struct time;

    private:
        std::vector<const callback *> cs;
    };

    struct event::time : uncopyable {
        using clock = std::chrono::steady_clock;
        using point = std::pair<clock::time_point, const callback*>;

        struct emplacer {
            time* t;
            clock::duration d;
            void operator+=(const callback *c) {
                t->events.emplace(clock::now() + d, c);
            }
        };

        clock::duration operator()() noexcept {
            clock::time_point now = clock::now();
            while (!events.empty()) {
                point next = events.top();
                if (next.first > now && next.first > (now = clock::now()))
                    return next.first - now;
                events.pop();
                (*next.second)();
            }
            return clock::duration::zero();
        }

        emplacer operator[](clock::duration d) {
            return emplacer{this, d};
        }

    private:
        std::priority_queue<point, std::vector<point>, std::greater<point>> events;
    };
};

#if COROUTINE_EPOLL || (!defined(COROUTINE_EPOLL) && defined(__linux__))
#include "events_io_epoll.h"
#else
#include "events_io_select.h"
#endif
