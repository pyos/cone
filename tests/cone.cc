#include "base.cc"
#include <fenv.h>
#include <math.h>
#include <float.h>
#include <unistd.h>
#include <sys/socket.h>

#include <stdexcept>

static bool test_yield() {
    int v = 0;
    cone::ref _ = [&]() { return (v++, true); };
    return cone::yield() && ASSERT(v == 1, "%d != 1", v);
}

static bool test_detach() {
    int v = 0;
    cone::ref{[&]() { return (v++, cone::yield()) ? (v++, true) : (v++, false); }};
    return cone::yield() && ASSERT(v == 1, "%d != 1", v)
        && cone::yield() && ASSERT(v == 2, "%d != 2", v);
}

static bool test_wait() {
    int v = 0;
    cone::ref c = [&]() { return cone::yield() && cone::yield() && cone::yield() && (v++, true); };
    return c->wait(cone::rethrow) && ASSERT(v == 1, "%d != 1", v);
}

static bool test_cancel() {
    int v = 0;
    cone::ref c = [&]() { return cone::yield() && (v++, true); };
    c->cancel();
    return ASSERT(!c->wait(cone::rethrow), "cancelled coroutine succeeded")
        && ASSERT(mun_errno == ECANCELED, "unexpected error %d", mun_errno)
        && ASSERT(v == 0, "%d != 0", v);
}

static bool test_cancel_atomic() {
    int v = 0;
    // also run this under asan (must not use-after-free)
    cone::ref{[&]() { return ::cone->cancel(), v++, true; }};
    return cone::yield() && ASSERT(v == 1, "%d != 1", v);
}

static bool test_cancel_uninterruptible() {
    int v = 0;
    cone::ref c = [&]() { return cone::uninterruptible([&]() { return cone::yield() && (v++, true); })
                              && cone::yield() && (v++, true); };
    c->cancel();
    return ASSERT(!c->wait(cone::rethrow), "cancelled coroutine succeeded")
        && ASSERT(mun_errno == ECANCELED, "unexpected error %d", mun_errno)
        && ASSERT(v == 1, "%d != 1", v);
}

static bool test_cancel_sleeping() {
    cone::ref c = []() { return ASSERT(!cone::sleep_for(100ms) && mun_errno == ECANCELED, "not cancelled"); };
    auto a = cone::time::clock::now();
    return (c->cancel(), c->wait(cone::rethrow))
        && ASSERT(cone::time::clock::now() - a < 10ms, "shouldn't have slept");
}

static bool test_cancel_by_guard() {
    cone::guard{[&]() { return cone::sleep_for(100s); }};
    return true; // test framework will abort if the coroutine didn't finish
}

static bool test_wait_no_rethrow() {
    cone::ref c = [&]() { return cone::yield(); };
    c->cancel();
    return c->wait(cone::norethrow);
}

template <bool cancel>
static bool test_sleep() {
    auto start = cone::time::clock::now();
    cone::ref a = [=]() { return cone::sleep(start + 50ms); };
    cone::ref b = [=]() { return cone::sleep(start + 100ms); };
    if (!a->wait(cone::rethrow))
        return false;
    if (cancel)
        b->cancel();
    if (cancel ? !ASSERT(!b->wait(cone::rethrow) && mun_errno == ECANCELED, "b->wait failed")
               : !b->wait(cone::rethrow))
        return false;
    auto end = cone::time::clock::now();
    return INFO("%fs", std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count())
        && ASSERT((end - start) >= (cancel ? 50ms : 100ms), "did not sleep enough")
        && ASSERT(!cancel || (end - start) < 75ms, "slept too much");
}

static bool test_sleep_after_cancel() {
    ::cone->cancel();
    auto a = cone::time::clock::now();
    return ASSERT(!cone::yield() && mun_errno == ECANCELED, "did not cancel itself")
        && cone::sleep(a + 10ms) && ASSERT(cone::time::clock::now() - a >= 10ms, "slept for too little");
}

static bool test_deadline() {
    auto d = ::cone->timeout(0us);
    return ASSERT(!cone::sleep_for(1ms) && mun_errno == ETIMEDOUT, "deadline did not trigger");
}

static bool test_deadline_lifting() {
    ::cone->timeout(0us);
    return cone::sleep_for(1ms);
}

static bool test_deadline_nop() {
    return ::cone->timeout(cone::timedelta::max(), [] { return cone::sleep_for(1ms); });
}

static bool test_count() {
    auto& c = *cone::count();
    if (!ASSERT(c == 1u, "%u != 1", c.load())) return false; // i.e. this coroutine
    cone::ref _1 = []() { return true; };
    if (!ASSERT(c == 2u, "%u != 2", c.load())) return false;
    cone::ref _2 = []() { return true; };
    cone::ref _3 = []() { return true; };
    return ASSERT(c == 4u, "%u != 4", c.load()) && cone::yield()
        && ASSERT(c == 1u, "%u != 1", c.load());
}

static bool test_event() {
    cone::event ev;
    cone::ref _1 = [&]() { size_t w = ev.wake(); return ASSERT(w == 1u, "%zu != 1", w); };
    return ev.wait() && _1->wait(cone::rethrow);
}

static bool test_event_wake() {
    int v = 0;
    cone::event ev;
    cone::ref _1 = [&]() { return ev.wait() && (v++, true); };
    cone::ref _2 = [&]() { size_t w = ev.wake(1); return ASSERT(w == 1u, "%zu != 1", w); };
    // _1 has not started running yet, so this coroutine will be the first in queue.
    return ev.wait() && ASSERT(v == 0, "%d != 0", v)
        && (ev.wake(), cone::yield() && ASSERT(v == 1, "%d != 1", v)) && _2->wait(cone::rethrow);
}

static bool test_mutex() {
    int last = 0;
    cone::mutex m;
    cone::ref a = [&]() {
        auto g = m.guard();
        return cone::yield() && cone::yield() && cone::yield() && cone::yield() && (last = 1, true);
    };
    cone::ref b = [&]() {
        return m.lock(), last = 2, m.unlock(), true;
    };
    return a->wait(cone::rethrow) && b->wait(cone::rethrow) && ASSERT(last == 2, "%d != 2", last);
}

static bool test_exceptions_0() {
    cone::ref x = []() -> bool { throw std::runtime_error("<-- should preferably be demangled"); };
    return ASSERT(!x->wait(cone::rethrow), "x succeeded despite throwing") && INFO("%s", mun_last_error()->text);
}

static bool test_exceptions_1() {
    cone::ref x = []() -> bool {
        struct pause { ~pause() { cone::yield(); } } x;
        throw std::runtime_error("1");
    };
    cone::yield(); // `x` now paused while unwinding
    try {
        throw std::runtime_error("2");
    } catch (const std::exception& e) {
        cone::yield(); // `x` now done handling the error (and should fail)
        if (!ASSERT(!strcmp(e.what(), "2"), "%s != 2", e.what()))
            return false;
    }
    return ASSERT(!x->wait(cone::rethrow), "x succeeded despite throwing");
}

static bool test_exceptions_2() {
    auto check_error = [](const char *expect) {
        try {
            throw std::runtime_error(expect);
        } catch (const std::exception& e) {
            if (!cone::yield() || !ASSERT(!strcmp(e.what(), expect), "%s != %s", e.what(), expect))
                return false;
            try {
                std::rethrow_exception(std::current_exception());
            } catch (const std::exception& g) {
                return ASSERT(!strcmp(g.what(), expect), "%s != %s", g.what(), expect);
            }
        }
    };
    cone::ref x = [&]() -> bool { return check_error("1"); };
    bool ok = check_error("2");
    return x->wait(cone::rethrow) && ok;
}

namespace {
    struct fd {
        int i = -1;

        ~fd() {
            if (i >= 0)
                close(i);
        }
    };
}

static bool test_yield_to_io() {
    fd fds[2];
    if (pipe((int*)fds) || cold_unblock(fds[0].i) || cold_unblock(fds[1].i) MUN_RETHROW_OS)
        return false;
    int v = 0;
    cone::ref c = [&]() {
        char buf[1];
        return !(cold_read(fds[0].i, buf, 1) < 0 MUN_RETHROW_OS) && (v++, true);
    };
    return cone::yield() && !(write(fds[1].i, "k", 1) < 0 MUN_RETHROW_OS)
        // one to schedule, one to run
        && cone::yield() && cone::yield() && ASSERT(v == 1, "%d != 1", v);
}

static bool test_rdwr() {
    fd fds[2];
    if (pipe((int*)fds) || cold_unblock(fds[0].i) || cold_unblock(fds[1].i) MUN_RETHROW_OS)
        return false;
    const char data[] = "Hello, World! Hello, World! Hello, World! Hello, World!";
    cone::ref r = [&, fd = fds[0].i]() {
        char buf[sizeof(data)];
        for (ssize_t rd, mod = 0, N = 100000; N--; mod = (mod + rd) % sizeof(data)) {
            if ((rd = cold_read(fd, buf, sizeof(data) - mod)) < 0 MUN_RETHROW_OS)
                return false;
            if (!ASSERT(memcmp(data + mod, buf, rd) == 0, "recvd bad data"))
                return false;
        }
        return true;
    };
    cone::ref w = [&, fd = fds[1].i]() {
        for (ssize_t wr, mod = 0, N = 100000; N--; mod = (mod + wr) % sizeof(data))
            if ((wr = cold_write(fd, data + mod, sizeof(data) - mod)) < 0 MUN_RETHROW_OS)
                return false;
        return true;
    };
    return r->wait(cone::rethrow) && w->wait(cone::rethrow);
}

static bool test_concurrent_rw() {
    fd fds[2];
    // don't bother with non-blocking mode, we'll `cone_iowait` directly.
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, (int*)fds) MUN_RETHROW_OS)
        return false;
    int result = 0;
    cone::ref a = [&]() { return !cone_iowait(fds[0].i, 0) && (result |= 1, true); };
    cone::ref b = [&]() { return !cone_iowait(fds[0].i, 1) && (result |= 2, true); };
    cone::ref c = [&]() { return !cone_iowait(fds[0].i, 0) && (result |= 4, true); };
    return b->wait(cone::rethrow)
        && ASSERT(result == 2, "reader also finished, but data hasn't been written yet")
        && ASSERT(write(fds[1].i, "x", 1) == 1, "write() failed")
        && a->wait(cone::rethrow)
        && ASSERT(result == 7, "reader status: %s, %s", (result & 1 ? "awake" : "asleep"), (result & 4 ? "awake" : "asleep"))
        && c->wait(cone::rethrow);
}

template <size_t n>
static bool test_many_fds() {
    fd fds[n * 2];
    for (size_t i = 0; i < n; i++)
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, (int*)&fds[i*2]) MUN_RETHROW_OS)
            return false;
    size_t result = 0;
    cone::ref cs[n];
    for (size_t i = 0; i < n; i++)
        cs[i] = [&, i]() { return !cone_iowait(fds[i*2].i, 0); };
    if (!cone::yield() MUN_RETHROW) return false;
    if (!ASSERT(result == 0, "some readers already finished")) return false;
    for (size_t i = 0; i < n; i++)
        if (!ASSERT(write(fds[i*2+1].i, "x", 1) == 1, "write() failed"))
            return false;
    if (!cone::yield() MUN_RETHROW) return false;
    for (size_t i = 0; i < n; i++)
        if (!cs[i]->wait(cone::rethrow) MUN_RETHROW) return false;
    return true;
}

static bool test_io_starvation() {
    bool stop = false;
    cone::event a;
    cone::event b;
    auto wake_wait = [&](cone::event& wk, cone::event& wt) {
        while (!stop)
            if (wk.wake(), !wt.wait())
                return false;
        return true;
    };
    cone::ref ca = [&]() { return wake_wait(b, a); };
    cone::ref cb = [&]() { return wake_wait(a, b); };
    // yielding successfully implies completing an i/o peek (see test_yield_to_io)
    cone::ref cc = [&]() { return cone::yield() && (a.wake(), b.wake(), stop = true); };
    if (!ASSERT(cone::sleep_for(20ms), "sleep() failed")) return false;
    cc->cancel();
    ca->cancel();
    cb->cancel();
    return cc->wait(cone::rethrow) && ca->wait(cone::rethrow) && cb->wait(cone::rethrow);
}

static bool test_thread() {
    int v = 0;
    return cone::thread([&]() {
        return cone::sleep_for(100ms)
            && cone::ref{[&]() { return ++v; }}->wait(cone::rethrow); }
    )->wait(cone::rethrow) && ASSERT(v == 1, "%d != 1", v);
}

static bool test_mt_mutex() {
    size_t r = 0;
    cone::mutex m;
    return spawn_and_wait<cone::thread>(4, [&]() {
        return spawn_and_wait(100, [&]() {
            for (size_t j = 0; j < 10000; j++) if (auto g = m.guard(cone::mutex::interruptible))
                r++;
            else
                return false;
            return true;
        });
    }) && ASSERT(r == 4 * 100 * 10000, "%zu != %d", r, 4 * 100 * 10000);
}

static bool test_mguard() {
    cone::mguard g;
    if (!ASSERT(g.active() == 0, "@0"))
        return false;
    g.add([]() { return true; });
    g.add([]() { return true; });
    g.add([]() { return true; });
    if (!ASSERT(g.active() == 3, "@1") || !cone::yield() || !ASSERT(g.active() == 0, "@2"))
        return false;
    g.add([]() { return cone::sleep_for(1s); });
    g.add([]() { return cone::sleep_for(1s); });
    g.add([]() { return cone::sleep_for(1s); });
    return ASSERT(g.active() == 3, "@3") && cone::yield() && ASSERT(g.active() == 3, "@4");
}

static bool test_sse2_csr() {
    double x = 2.0;
    cone::ref _1 = [&x] {
        fesetround(FE_DOWNWARD);
        double y = sqrt(x);
        cone::yield();
        double z = sqrt(x);
        return ASSERT(y == z, "%.*e != %.*e", DECIMAL_DIG, y, DECIMAL_DIG, z);
    };
    cone::ref _2 = [] { fesetround(FE_UPWARD); return true; };
    return _1->wait(cone::rethrow);
}

export {
    { "cone:yield", &test_yield },
    { "cone:detach", &test_detach },
    { "cone:wait", &test_wait },
    { "cone:wait on cancelled", &test_cancel },
    { "cone:wait on cancelled, but atomic", &test_cancel_atomic },
    { "cone:wait on cancelled, but uninterruptible", &test_cancel_uninterruptible },
    { "cone:wait on cancelled before sleeping", &test_cancel_sleeping },
    { "cone:wait on cancelled by scope guard", &test_cancel_by_guard },
    { "cone:wait(rethrow=false)", &test_wait_no_rethrow },
    { "cone:sleep 50ms concurrent with 100ms", &test_sleep<false> },
    { "cone:sleep 50ms concurrent with cancelled 100ms", &test_sleep<true> },
    { "cone:sleep while handling cancellation", &test_sleep_after_cancel },
    { "cone:deadline", &test_deadline },
    { "cone:deadline lifting", &test_deadline_lifting },
    { "cone:deadline at never", &test_deadline_nop },
    { "cone:count", &test_count },
    { "cone:event", &test_event },
    { "cone:event.wake(1)", &test_event_wake },
    { "cone:mutex", &test_mutex },
    { "cone:throw", &test_exceptions_0 },
    { "cone:throw and unwind", &test_exceptions_1 },
    { "cone:throw and throw again", &test_exceptions_2 },
    { "cone:yield to reader", &test_yield_to_io },
    { "cone:reader + writer", &test_rdwr },
    { "cone:reader + writer on one fd", &test_concurrent_rw },
    { "cone:many fds", &test_many_fds<120> },
    { "cone:io starvation", &test_io_starvation },
    { "cone:thread", &test_thread },
    { "cone:threads and a mutex", &test_mt_mutex },
    { "cone:mguard", &test_mguard },
    { "cone:sse2 csr", &test_sse2_csr },
};
