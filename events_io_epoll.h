#include <unistd.h>
#include <sys/epoll.h>

struct aio::event::io : uncopyable {
    struct fdevent : uncopyable {
        const int fd;
        const int epoll;
        epoll_event params{EPOLLRDHUP | EPOLLHUP | EPOLLET | EPOLLONESHOT, {.ptr = this}};

        struct {
            fdevent *ev;
            const callback *cb;

            void operator+=(const callback *c) noexcept {
                assert("only one coroutine performs I/O on a single file" && cb == nullptr);
                cb = c;
                ev->params.events |= this == &ev->read ? EPOLLIN : EPOLLOUT;
                epoll_ctl(ev->epoll, EPOLL_CTL_MOD, ev->fd, &ev->params);
            }

            void operator-=(const callback *c) noexcept {
                assert("this callback was registered with +=" && c == cb);
                cb = nullptr;
                ev->params.events &= ~(this == &ev->read ? EPOLLIN : EPOLLOUT);
                epoll_ctl(ev->epoll, EPOLL_CTL_MOD, ev->fd, &ev->params);
            }

            void operator()() noexcept {
                if (const auto *c = cb) {
                    cb = nullptr;  // fd already unregistered due to EPOLLONESHOT
                    (*c)();
                }
            }
        } read{this, nullptr}, write{this, nullptr};

        fdevent(int fd) : fd{fd}, epoll{-1} {}
        fdevent(int fd, int epoll) : fd{fd}, epoll{epoll} {
            epoll_ctl(epoll, EPOLL_CTL_ADD, fd, &params);
        }

        ~fdevent() {
            if (epoll >= 0)
                epoll_ctl(epoll, EPOLL_CTL_DEL, fd, NULL);
        }

        HASHABLE_BY_FIELD(fdevent, fd);
    };

    io() {
        if ((epoll = epoll_create1(0)) < 0)
            assert("epoll_create1 must not fail" && 0), abort();
    }

    ~io() {
        ::close(epoll);
    }

    fdevent& operator[](int fd) {
        auto it = cbs.find(fd);  // `emplace` may construct a temporary even if a callback already exists
        if (it == cbs.end())
            it = cbs.emplace(fd, epoll).first;
        return (fdevent&)*it;
    }

    template <typename clock_t, typename clock_period_t>
    int operator()(std::chrono::duration<clock_t, clock_period_t> timeout) noexcept {
        return operator()(timeout == timeout.zero() ? -1 : std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
    }

    int operator()(int timeout) noexcept {
        epoll_event evs[32];
        int got = epoll_wait(epoll, evs, sizeof(evs) / sizeof(evs[0]), timeout);
        if (got > 0) for (epoll_event *ev = evs; ev != evs + got; ev++) {
            fdevent *c = (fdevent*)ev->data.ptr;
            if (ev->events & (EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP))
                c->read();
            if (ev->events & (EPOLLOUT | EPOLLERR | EPOLLHUP))
                c->write();
            if (c->read.cb == nullptr && c->write.cb == nullptr)
                cbs.erase(c->fd);  // TODO wait for a while (this coroutine is likely to return)
        }
        return got < 0 ? -1 : 0;
    }

private:
    int epoll;
    std::unordered_set<fdevent, typename fdevent::hash, typename fdevent::eq> cbs;
};
