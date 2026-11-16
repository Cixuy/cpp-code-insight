#include "http.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace ci {

namespace {
std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}
}  // namespace

int parse_request(const char* data, size_t len, Request& out) {
    out = Request{};
    const std::string buf(data, len);
    size_t head_end = buf.find("\r\n\r\n");
    if (head_end == std::string::npos) {
        if (len > 64 * 1024) { out.error = 431; out.error_message = "header too large"; }
        return 0;  // 头部还没收全
    }

    std::string head = buf.substr(0, head_end);
    size_t line_end = head.find("\r\n");
    std::string request_line = head.substr(0, line_end == std::string::npos ? head.size() : line_end);

    // METHOD SP TARGET SP VERSION
    size_t sp1 = request_line.find(' ');
    size_t sp2 = request_line.rfind(' ');
    if (sp1 == std::string::npos || sp2 == std::string::npos || sp1 == sp2) {
        out.error = 400; out.error_message = "bad request line"; return -1;
    }
    out.method = request_line.substr(0, sp1);
    std::string target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    out.version = request_line.substr(sp2 + 1);

    size_t q = target.find('?');
    if (q == std::string::npos) {
        out.path = target;
    } else {
        out.path = target.substr(0, q);
        out.query = target.substr(q + 1);
    }

    // headers
    size_t pos = (line_end == std::string::npos) ? head.size() : line_end + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        std::string line = head.substr(pos, (e == std::string::npos ? head.size() : e) - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            out.headers[to_lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
        }
        if (e == std::string::npos) break;
        pos = e + 2;
    }

    size_t body_start = head_end + 4;
    size_t content_length = 0;
    auto it = out.headers.find("content-length");
    if (it != out.headers.end()) {
        try { content_length = static_cast<size_t>(std::stoul(it->second)); } catch (...) { content_length = 0; }
    }
    if (content_length > 32 * 1024 * 1024) {
        out.error = 413; out.error_message = "body too large"; return -1;
    }
    if (len - body_start < content_length) {
        return 0;  // body 还没收全
    }
    out.body = buf.substr(body_start, content_length);
    return static_cast<int>(body_start + content_length);
}

std::string build_response(int status, const std::string& content_type,
                           const std::string& body) {
    const char* reason = "OK";
    switch (status) {
        case 200: reason = "OK"; break;
        case 400: reason = "Bad Request"; break;
        case 404: reason = "Not Found"; break;
        case 405: reason = "Method Not Allowed"; break;
        case 413: reason = "Payload Too Large"; break;
        case 431: reason = "Request Header Fields Too Large"; break;
        case 500: reason = "Internal Server Error"; break;
        default: reason = "Unknown"; break;
    }
    std::string resp;
    resp.reserve(body.size() + 160);
    resp += "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
    resp += "Content-Type: " + content_type + "\r\n";
    resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    resp += "Connection: keep-alive\r\n";
    resp += "Server: code-insight\r\n\r\n";
    resp += body;
    return resp;
}

std::string query_param(const Request& req, const std::string& key) {
    size_t pos = 0;
    while (pos < req.query.size()) {
        size_t amp = req.query.find('&', pos);
        std::string pair = req.query.substr(pos, (amp == std::string::npos ? req.query.size() : amp) - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key) {
            return pair.substr(eq + 1);
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return "";
}

}  // namespace ci
