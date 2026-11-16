#include "poller.h"

#ifndef _WIN32
#include <sys/epoll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace ci {

namespace {
uint32_t to_epoll(Event ev) {
    uint32_t r = 0;
    if (has(ev, Event::Readable)) r |= EPOLLIN;
    if (has(ev, Event::Writable)) r |= EPOLLOUT;
    return r;
}

class EpollPoller : public Poller {
public:
    EpollPoller() { ep_ = ::epoll_create1(0); }
    ~EpollPoller() override { if (ep_ >= 0) ::close(ep_); }

    bool add(socket_t fd, Event ev) override { return ctl(fd, EPOLL_CTL_ADD, ev); }
    bool modify(socket_t fd, Event ev) override { return ctl(fd, EPOLL_CTL_MOD, ev); }
    bool remove(socket_t fd) override { return ctl(fd, EPOLL_CTL_DEL, Event::None); }

    int wait(std::vector<Ready>& out, int timeout_ms) override {
        epoll_event evs[256];
        int n = ::epoll_wait(ep_, evs, 256, timeout_ms);
        if (n < 0) return -1;
        out.clear();
        out.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            Event e = Event::None;
            if (evs[i].events & EPOLLIN) e = e | Event::Readable;
            if (evs[i].events & EPOLLOUT) e = e | Event::Writable;
            if (evs[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) e = e | Event::Error;
            out.push_back({evs[i].data.fd, e});
        }
        return n;
    }

private:
    bool ctl(socket_t fd, int op, Event ev) {
        epoll_event e{};
        e.data.fd = fd;
        // EPOLLRDHUP 让对端半关闭也能及时感知，避免连接悬挂
        e.events = to_epoll(ev) | EPOLLRDHUP;
        return ::epoll_ctl(ep_, op, fd, &e) == 0;
    }
    int ep_ = -1;
};
}  // namespace

Poller* Poller::create() { return new EpollPoller(); }

std::string last_error_string() { return std::strerror(errno); }

}  // namespace ci
#endif
