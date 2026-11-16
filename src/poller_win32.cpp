#include "poller.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <unordered_map>

namespace ci {

class WsaPoller : public Poller {
public:
    bool add(socket_t fd, Event ev) override { map_[fd] = ev; return true; }
    bool modify(socket_t fd, Event ev) override {
        if (map_.find(fd) == map_.end()) return false;
        map_[fd] = ev;
        return true;
    }
    bool remove(socket_t fd) override { return map_.erase(fd) > 0; }

    int wait(std::vector<Ready>& out, int timeout_ms) override {
        // WSAPoll 需要连续数组，每次按当前关注集合重建
        std::vector<WSAPOLLFD> arr;
        std::vector<socket_t> fds;
        arr.reserve(map_.size());
        fds.reserve(map_.size());
        for (const auto& kv : map_) {
            WSAPOLLFD p{};
            p.fd = static_cast<SOCKET>(kv.first);
            p.events = 0;
            if (has(kv.second, Event::Readable)) p.events |= POLLRDNORM;
            if (has(kv.second, Event::Writable)) p.events |= POLLWRNORM;
            arr.push_back(p);
            fds.push_back(kv.first);
        }
        if (arr.empty()) {
            Sleep(static_cast<DWORD>(timeout_ms < 0 ? 0 : timeout_ms));
            return 0;
        }
        int n = ::WSAPoll(arr.data(), static_cast<ULONG>(arr.size()), timeout_ms);
        if (n <= 0) return n;
        out.clear();
        for (size_t i = 0; i < arr.size(); ++i) {
            if (arr[i].revents == 0) continue;
            Event e = Event::None;
            if (arr[i].revents & (POLLRDNORM | POLLIN)) e = e | Event::Readable;
            if (arr[i].revents & (POLLWRNORM | POLLOUT)) e = e | Event::Writable;
            if (arr[i].revents & (POLLERR | POLLHUP | POLLNVAL)) e = e | Event::Error;
            out.push_back({fds[i], e});
        }
        return static_cast<int>(out.size());
    }

private:
    std::unordered_map<socket_t, Event> map_;
};

Poller* Poller::create() { return new WsaPoller(); }

std::string last_error_string() { return std::to_string(::WSAGetLastError()); }

}  // namespace ci
#endif
