#include <sys/select.h>

struct aio::event::io : uncopyable {
    struct fdevent : uncopyable {
        struct {
            fd_set *set;
            const int fd;
            const callback *cb;

            void operator+=(const callback *c) noexcept {
                assert("only one coroutine performs I/O on a single file" && cb == nullptr);
                cb = c;
                FD_SET(fd, set);
            }

            void operator-=(const callback *c) noexcept {
                assert("this callback was registered with +=" && c == cb);
                cb = nullptr;
                FD_CLR(fd, set);
            }

            void operator()() noexcept {
                if (const callback *c = cb) {
                    *this -= cb;
                    (*c)();
                }
            }
        } read, write;

        fdevent(int fd, io *ev) : read{&ev->r, fd, nullptr}, write{&ev->w, fd, nullptr} {}
        HASHABLE_BY_FIELD(fdevent, read.fd);
    };

    io() {
        FD_ZERO(&r);
        FD_ZERO(&w);
    }

    fdevent& operator[](int fd) {
        assert("all file descriptors must fit in fd_set" && fd < FD_SETSIZE);
        std::lock_guard<std::recursive_mutex> lock{m};
        return (fdevent&)*cbs.emplace(fd, this).first;
    }

    template <typename clock_t, typename clock_period_t>
    int operator()(std::chrono::duration<clock_t, clock_period_t> timeout) noexcept {
        struct timeval tv = {
            std::chrono::duration_cast<std::chrono::seconds>(timeout).count(),
            std::chrono::duration_cast<std::chrono::microseconds>(timeout).count() % 1000000,
        };
        return operator()(timeout == timeout.zero() ? NULL : &tv);
    }

    int operator()(struct timeval* timeout) noexcept {
        int max_fd = 0;
        fd_set rready, wready;
        std::unique_lock<std::recursive_mutex> lock{m};
        for (const fdevent& ev : cbs)
            if (max_fd <= ev.read.fd)
                max_fd = ev.read.fd + 1;
        rready = r;
        wready = w;
        UNLOCKED(lock, int got = select(max_fd, &rready, &wready, NULL, timeout));
        if (got > 0) for (auto it = cbs.begin(); it != cbs.end(); ) {
            if (FD_ISSET(it->read.fd, &rready))
                ((fdevent&)*it).read();
            if (FD_ISSET(it->write.fd, &wready))
                ((fdevent&)*it).write();
            // TODO wait for a while (this coroutine is likely to return)
            it = it->read.cb == nullptr && it->write.cb == nullptr ? cbs.erase(it) : ++it;
        }
        return got < 0 ? -1 : 0;
    }

private:
    fd_set r, w;
    std::recursive_mutex m;
    std::unordered_set<fdevent, typename fdevent::hash, typename fdevent::eq> cbs;
};
