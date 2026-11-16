#include "server.h"

#include <cstring>
#include <sstream>

#include "analyzer.h"
#include "json_util.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_type = int;
#define CI_CLOSE closesocket
#define CI_ERRNO WSAGetLastError()
#define CI_WOULDBLOCK (WSAEWOULDBLOCK)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
using socklen_type = socklen_t;
#define CI_CLOSE ::close
#define CI_ERRNO errno
#define CI_WOULDBLOCK (EAGAIN)
#endif

namespace ci {

namespace {
bool set_nonblocking(socket_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    return ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode) == 0;
#else
    int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

std::string scan_to_json(const ScanResult& r, const std::string& filename) {
    std::ostringstream js;
    js << "{";
    js << "\"file\":\"" << json_escape(filename) << "\",";
    js << "\"language\":\"" << r.language << "\",";
    js << "\"total_lines\":" << r.total_lines << ",";
    js << "\"code_lines\":" << r.code_lines << ",";
    js << "\"comment_lines\":" << r.comment_lines << ",";
    js << "\"max_nesting\":" << r.max_nesting << ",";
    js << "\"cyclomatic\":" << r.cyclomatic << ",";
    js << "\"todo_count\":" << r.todo_count << ",";
    js << "\"issue_count\":" << r.issues.size() << ",";
    js << "\"issues\":[";
    for (size_t i = 0; i < r.issues.size(); ++i) {
        const auto& is = r.issues[i];
        if (i) js << ",";
        js << "{\"rule\":\"" << is.rule << "\",\"severity\":\"" << is.severity
           << "\",\"line\":" << is.line << ",\"message\":\"" << json_escape(is.message)
           << "\",\"snippet\":\"" << json_escape(is.snippet) << "\"}";
    }
    js << "]}";
    return js.str();
}
}  // namespace

Server::Server(int port) : port_(port), poller_(Poller::create()) {}

Server::~Server() {
    if (listen_fd_ != kInvalidSocket) CI_CLOSE(listen_fd_);
}

bool Server::start(std::string& err) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ == kInvalidSocket) { err = "socket() failed: " + last_error_string(); return false; }

    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        err = "bind() failed on port " + std::to_string(port_) + ": " + last_error_string();
        return false;
    }
    if (::listen(listen_fd_, 512) != 0) { err = "listen() failed: " + last_error_string(); return false; }
    if (!set_nonblocking(listen_fd_)) { err = "nonblocking listen fd failed"; return false; }
    if (!poller_->add(listen_fd_, Event::Readable)) { err = "poller add listen fd failed"; return false; }
    running_ = true;
    return true;
}

bool Server::on_accept() {
    for (;;) {
        sockaddr_in peer{};
        socklen_type len = sizeof(peer);
        socket_t fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd == kInvalidSocket) {
            if (CI_ERRNO == CI_WOULDBLOCK
#ifdef _WIN32
                || CI_ERRNO == WSAEWOULDBLOCK
#endif
            ) return true;
            return false;  // 真实错误交给上层循环判断
        }
        set_nonblocking(fd);
        int nodelay = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        conns_[fd] = Conn{};
        poller_->add(fd, Event::Readable);
    }
}

void Server::close_conn(socket_t fd) {
    poller_->remove(fd);
    conns_.erase(fd);
    CI_CLOSE(fd);
}

void Server::on_readable(socket_t fd) {
    char buf[8192];
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;

    for (;;) {
        int n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            it->second.in.append(buf, static_cast<size_t>(n));
            if (it->second.in.size() > 32 * 1024 * 1024) { close_conn(fd); return; }
            continue;
        }
        if (n == 0) { close_conn(fd); return; }
        if (CI_ERRNO == CI_WOULDBLOCK
#ifdef _WIN32
            || CI_ERRNO == WSAEWOULDBLOCK
#endif
        ) break;
        close_conn(fd);
        return;
    }

    // 一次读循环内可能包含多个流水线请求，逐个处理
    for (;;) {
        Request req;
        int consumed = parse_request(it->second.in.data(), it->second.in.size(), req);
        // 数据不足只代表本轮解析结束，不能直接返回，
        // 否则已排队的响应不会被发送出去（客户端会一直等）
        if (consumed == 0) break;
        if (consumed < 0) {
            int status = req.error ? req.error : 400;
            std::string body = "{\"error\":\"" + json_escape(req.error_message) + "\"}";
            it->second.out += build_response(status, "application/json", body);
            break;
        }
        it->second.in.erase(0, static_cast<size_t>(consumed));
        std::string body = route(req);
        it->second.out += build_response(200, "application/json", body);
    }

    poller_->modify(fd, Event::Readable | Event::Writable);
    on_writable(fd);
}

void Server::on_writable(socket_t fd) {
    auto it = conns_.find(fd);
    if (it == conns_.end()) return;
    while (!it->second.out.empty()) {
        int n = ::send(fd, it->second.out.data(),
                       static_cast<int>(it->second.out.size()), 0);
        if (n > 0) {
            it->second.out.erase(0, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && (CI_ERRNO == CI_WOULDBLOCK
#ifdef _WIN32
                      || CI_ERRNO == WSAEWOULDBLOCK
#endif
        )) return;
        close_conn(fd);
        return;
    }
    poller_->modify(fd, Event::Readable);
}

std::string Server::route(const Request& req) {
    if (req.method == "GET" && req.path == "/health") {
        return "{\"status\":\"ok\",\"service\":\"code-insight\"}";
    }
    if (req.method == "POST" && req.path == "/scan") {
        std::string filename = query_param(req, "filename");
        if (filename.empty()) filename = "inline.txt";
        ScanResult r = scan(filename, req.body);
        return scan_to_json(r, filename);
    }
    return "{\"error\":\"not found\"}";
}

void Server::run() {
    std::vector<Poller::Ready> ready;
    while (running_) {
        int n = poller_->wait(ready, 500);
        if (n < 0) continue;
        for (const auto& r : ready) {
            if (r.fd == listen_fd_) {
                if (has(r.ev, Event::Readable)) on_accept();
                continue;
            }
            if (has(r.ev, Event::Error)) { close_conn(r.fd); continue; }
            if (has(r.ev, Event::Readable)) on_readable(r.fd);
            if (has(r.ev, Event::Writable)) on_writable(r.fd);
        }
    }
}

}  // namespace ci
