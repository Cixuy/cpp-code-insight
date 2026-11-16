#pragma once
#include <string>
#include <vector>

namespace ci {

struct Issue {
    std::string rule;      // 规则 ID
    std::string severity;  // info / warning / critical
    int line = 0;
    std::string message;
    std::string snippet;
};

struct ScanResult {
    std::string language;
    int total_lines = 0;
    int code_lines = 0;
    int comment_lines = 0;
    int max_nesting = 0;
    int cyclomatic = 1;      // 简化圈复杂度：1 + 分支数
    int todo_count = 0;
    std::vector<Issue> issues;
};

// 依据文件扩展名猜语言，失败返回 "unknown"
std::string detect_language(const std::string& filename);

ScanResult scan(const std::string& filename, const std::string& code);

}  // namespace ci
