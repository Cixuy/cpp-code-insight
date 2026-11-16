#include "analyzer.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <cstring>
#include <sstream>

namespace ci {

namespace {

struct Rule {
    std::string id;
    std::string severity;
    std::regex pattern;
    std::string message;
};

// 硬编码凭据、危险函数、调试残留：这三类在评审里出现频率最高
const std::vector<Rule>& rules() {
    static const std::vector<Rule> kRules = {
        {"SECRET_HARDCODED", "critical",
         std::regex(R"((password|passwd|secret|api[_-]?key|access[_-]?key|token)\s*[:=]\s*["'][^"']{6,}["'])",
                    std::regex::icase),
         "疑似硬编码凭据，应改为环境变量或配置中心"},
        {"C_DANGEROUS_FUNC", "warning",
         std::regex(R"(\b(strcpy|strcat|sprintf|gets|scanf)\s*\()"),
         "使用了不安全的 C 字符串函数，建议改用带长度限制的版本"},
        {"DEBUG_RESIDUE", "warning",
         std::regex(R"((console\.log|System\.out\.println|printStackTrace|fmt\.Println)\s*\()"),
         "调试输出残留，提交前应清理或降级为日志"},
        {"EMPTY_CATCH", "warning",
         std::regex(R"(catch\s*\([^)]*\)\s*\{\s*\})"),
         "空 catch 块会吞掉异常，至少应记录日志"},
        {"SQL_CONCAT", "critical",
         std::regex(R"((SELECT|INSERT|UPDATE|DELETE)[^;]{0,80}\+\s*[a-zA-Z_])", std::regex::icase),
         "SQL 由字符串拼接构造，存在注入风险，应使用参数化查询"},
    };
    return kRules;
}

bool is_comment_line(const std::string& t) {
    return t.rfind("//", 0) == 0 || t.rfind("#", 0) == 0 || t.rfind("/*", 0) == 0 ||
           t.rfind("*", 0) == 0 || t.rfind("--", 0) == 0;
}

}  // namespace

std::string detect_language(const std::string& filename) {
    auto ends = [&](const char* suf) {
        size_t n = std::strlen(suf);
        return filename.size() >= n && filename.compare(filename.size() - n, n, suf) == 0;
    };
    if (ends(".java")) return "java";
    if (ends(".cpp") || ends(".cc") || ends(".cxx") || ends(".h") || ends(".hpp")) return "cpp";
    if (ends(".py")) return "python";
    if (ends(".js") || ends(".ts") || ends(".vue")) return "javascript";
    if (ends(".go")) return "go";
    if (ends(".sql")) return "sql";
    return "unknown";
}

ScanResult scan(const std::string& filename, const std::string& code) {
    ScanResult r;
    r.language = detect_language(filename);

    std::istringstream in(code);
    std::string line;
    int lineno = 0;
    int depth = 0;

    while (std::getline(in, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        r.total_lines++;
        std::string t = line;
        t.erase(0, t.find_first_not_of(" \t"));
        if (t.empty()) continue;
        if (is_comment_line(t)) { r.comment_lines++; continue; }
        r.code_lines++;

        if (t.find("TODO") != std::string::npos || t.find("FIXME") != std::string::npos) r.todo_count++;

        // 括号深度近似嵌套层数
        for (char c : line) {
            if (c == '{') { ++depth; r.max_nesting = std::max(r.max_nesting, depth); }
            else if (c == '}') { depth = std::max(0, depth - 1); }
        }
        // 简化圈复杂度：分支关键字计数
        static const std::regex branch(R"(\b(if|else\s+if|for|while|case|catch|&&|\|\||\?)\b)");
        auto begin = std::sregex_iterator(line.begin(), line.end(), branch);
        r.cyclomatic += static_cast<int>(std::distance(begin, std::sregex_iterator()));

        for (const auto& rule : rules()) {
            std::smatch m;
            if (std::regex_search(line, m, rule.pattern)) {
                Issue iss;
                iss.rule = rule.id;
                iss.severity = rule.severity;
                iss.line = lineno;
                iss.message = rule.message;
                std::string snip = t;
                if (snip.size() > 80) snip = snip.substr(0, 80) + "...";
                iss.snippet = snip;
                r.issues.push_back(std::move(iss));
            }
        }
    }
    return r;
}

}  // namespace ci
