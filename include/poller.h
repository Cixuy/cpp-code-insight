#include <cstdint>
#include <string>
#include <vector>

// 跨平台事件轮询抽象：Linux 用 epoll，Windows 用 WSAPoll。
// 目的不是"移植性"本身，而是让 Reactor 的网络核心与平台解耦，
// 上层的连接管理、协议解析、业务处理都是平台无关的。
namespace ci {

#ifdef _WIN32
using socket_t = uintptr_t;
constexpr socket_t kInvalidSocket = static_cast<socket_t>(~0ull);
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

enum class Event : uint8_t {
    None = 0,
    Readable = 1,
    Writable = 2,
    Error = 4,
};

inline Event operator|(Event a, Event b) {
    return static_cast<Event>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}
inline bool has(Event v, Event f) {
    return (static_cast<uint8_t>(v) & static_cast<uint8_t>(f)) != 0;
}

class Poller {
public:
    virtual ~Poller() = default;

    // 注册 / 修改 / 移除关注的事件
    virtual bool add(socket_t fd, Event ev) = 0;
    virtual bool modify(socket_t fd, Event ev) = 0;
    virtual bool remove(socket_t fd) = 0;

    // 等待事件就绪，超时单位毫秒；返回就绪数量
    struct Ready {
        socket_t fd;
        Event ev;
    };
    virtual int wait(std::vector<Ready>& out, int timeout_ms) = 0;

    static Poller* create();
};

// 把底层 errno/WSA 错误码转成可读字符串，日志和 JSON 错误体都用它
std::string last_error_string();

}  // namespace ci
