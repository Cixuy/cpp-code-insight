#pragma once
#include <memory>
#include <string>
#include <unordered_map>

#include "http.h"
#include "poller.h"

namespace ci {

class Server {
public:
    Server(int port);
    ~Server();
    bool start(std::string& err);
    void run();          // 阻塞的事件循环
    void stop() { running_ = false; }
    int port() const { return port_; }

private:
    struct Conn {
        std::string in;
        std::string out;
        bool closing = false;
    };

    bool on_accept();
    void on_readable(socket_t fd);
    void on_writable(socket_t fd);
    void close_conn(socket_t fd);
    std::string route(const Request& req);

    int port_;
    socket_t listen_fd_ = kInvalidSocket;
    std::unique_ptr<Poller> poller_;
    std::unordered_map<socket_t, Conn> conns_;
    bool running_ = false;
};

}  // namespace ci
