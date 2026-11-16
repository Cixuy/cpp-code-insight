#pragma once
#include <string>

namespace ci {

// 极简 JSON 转义：只处理引号、反斜杠和控制字符，
// 够用于本项目的响应体，不引入第三方库。
std::string json_escape(const std::string& s);

}  // namespace ci
