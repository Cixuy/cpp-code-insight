#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace ci {

struct Request {
    std::string method;
    std::string path;
    std::string query;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    // 解析结果：缺内容时错误码非 0
    int error = 0;
    std::string error_message;
};

// 返回本次消费的字节数；0 表示数据不足，需要继续读；-1 表示解析出错
int parse_request(const char* data, size_t len, Request& out);

std::string build_response(int status, const std::string& content_type,
                           const std::string& body);

std::string query_param(const Request& req, const std::string& key);

}  // namespace ci
