#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "analyzer.h"
#include "server.h"

#ifdef _WIN32
#include <winsock2.h>
#endif

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

// 命令行单文件扫描：CI 里不想起服务时直接用，退码 = critical 数 + warning 数（封顶 125）
static int scanOneFile(const std::string& path, bool json) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return 2;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string code = ss.str();

    ci::ScanResult r = ci::scan(path, code);
    int critical = 0, warning = 0;
    for (const auto& is : r.issues) {
        if (is.severity == "critical") ++critical;
        else if (is.severity == "warning") ++warning;
    }

    if (json) {
        std::printf("{\"file\":\"%s\",\"language\":\"%s\",\"lines\":%d,\"codeLines\":%d,"
                    "\"commentLines\":%d,\"maxNesting\":%d,\"cyclomatic\":%d,"
                    "\"todos\":%d,\"critical\":%d,\"warning\":%d,\"issues\":[",
                    jsonEscape(path).c_str(), jsonEscape(r.language).c_str(), r.total_lines,
                    r.code_lines, r.comment_lines, r.max_nesting, r.cyclomatic,
                    r.todo_count, critical, warning);
        for (size_t i = 0; i < r.issues.size(); ++i) {
            const auto& is = r.issues[i];
            std::printf("%s{\"rule\":\"%s\",\"severity\":\"%s\",\"line\":%d,\"message\":\"%s\"}",
                        i ? "," : "", jsonEscape(is.rule).c_str(), jsonEscape(is.severity).c_str(),
                        is.line, jsonEscape(is.message).c_str());
        }
        std::printf("]}\n");
    } else {
        std::printf("%s  (%s)\n", path.c_str(), r.language.c_str());
        std::printf("  行数 %d（代码 %d / 注释 %d）  最大嵌套 %d  圈复杂度 %d  TODO %d\n",
                    r.total_lines, r.code_lines, r.comment_lines, r.max_nesting, r.cyclomatic,
                    r.todo_count);
        std::printf("  问题 %zu 条：critical %d / warning %d\n", r.issues.size(), critical, warning);
        for (const auto& is : r.issues)
            std::printf("    [%-8s] %s:%d  %s\n", is.severity.c_str(), is.rule.c_str(), is.line,
                        is.message.c_str());
    }
    return critical + warning > 125 ? 125 : critical + warning;
}

// 目录批量扫描：CI 里真正的用法不是扫单个文件，而是扫整个仓。
// 按扩展名过滤，递归走完，最后给一张"文件数 / 问题数"汇总表，退码=问题总数（封顶 125）。
static int scanDir(const std::string& root, bool json) {
    namespace fs = std::filesystem;
    if (!fs::exists(root) || !fs::is_directory(root)) {
        std::fprintf(stderr, "error: %s 不是目录\n", root.c_str());
        return 2;
    }
    static const char* exts[] = {".c", ".cc", ".cpp", ".h", ".hpp", ".java", ".py", ".js", ".ts", ".go"};
    int files = 0, critical = 0, warning = 0, lines = 0, skipped = 0;
    std::vector<std::pair<std::string, std::pair<int, int>>> perFile;  // 文件 -> (critical, warning)

    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        std::string ext = e.path().extension().string();
        bool hit = false;
        for (const char* x : exts) if (ext == x) { hit = true; break; }
        if (!hit) continue;
        std::ifstream in(e.path(), std::ios::binary);
        if (!in) { ++skipped; continue; }
        std::stringstream ss;
        ss << in.rdbuf();
        ci::ScanResult r = ci::scan(e.path().string(), ss.str());
        int c = 0, w = 0;
        for (const auto& is : r.issues) {
            if (is.severity == "critical") ++c;
            else if (is.severity == "warning") ++w;
        }
        ++files;
        lines += r.total_lines;
        critical += c;
        warning += w;
        if (c || w) perFile.push_back({e.path().string(), {c, w}});
    }

    if (json) {
        std::printf("{\"root\":\"%s\",\"files\":%d,\"lines\":%d,\"critical\":%d,\"warning\":%d,\"skipped\":%d,\"offenders\":[",
                    jsonEscape(root).c_str(), files, lines, critical, warning, skipped);
        for (size_t i = 0; i < perFile.size(); ++i)
            std::printf("%s{\"file\":\"%s\",\"critical\":%d,\"warning\":%d}", i ? "," : "",
                        jsonEscape(perFile[i].first).c_str(), perFile[i].second.first, perFile[i].second.second);
        std::printf("]}\n");
    } else {
        std::printf("扫描目录 %s\n", root.c_str());
        std::printf("  文件 %d 个，%d 行；跳过无法打开 %d 个\n", files, lines, skipped);
        std::printf("  问题：critical %d / warning %d\n", critical, warning);
        if (!perFile.empty()) {
            std::printf("  有问题的文件：\n");
            for (size_t i = 0; i < perFile.size() && i < 30; ++i)
                std::printf("    %-52s critical %d / warning %d\n", perFile[i].first.c_str(),
                            perFile[i].second.first, perFile[i].second.second);
            if (perFile.size() > 30) std::printf("    …… 另有 %zu 个\n", perFile.size() - 30);
        }
    }
    int total = critical + warning;
    return total > 125 ? 125 : total;
}

int main(int argc, char** argv) {
    int port = 18080;
    std::string scanPath, scanDirPath;
    bool scanRequested = false;
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            // atoi 对 "abc"/"70000"/"" 一律给 0 或截断，端口 0 会随手绑一个随机端口，
            // 脚本再按 --port 去连就连不上，所以范围也一起校验
            const char* s = argv[++i];
            char* end = nullptr;
            long p = std::strtol(s, &end, 10);
            if (end == s || *end != '\0' || p < 1 || p > 65535) {
                std::fprintf(stderr, "error: --port 需要 1..65535 之间的整数（当前 \"%s\"）\n", s);
                return 2;
            }
            port = (int)p;
        } else if (arg == "--scan" || arg == "-s") {
            // 少了文件参数就报错退出：旧行为是静默落回"起服务"，
            // 脚本里 `--scan $FILE` 的变量为空时会挂成一个前台进程，CI 直接卡死
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                std::fprintf(stderr, "error: --scan 需要文件路径\n");
                std::fprintf(stderr, "usage: code-insight --scan <file> [--json]\n");
                return 2;
            }
            scanPath = argv[++i];
            scanRequested = true;
        } else if (arg == "--scan-dir") {
            // 同上：`--scan-dir $DIR` 变量为空时会静默起服务，CI 里表现为"扫描没结果、进程不退出"
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                std::fprintf(stderr, "error: --scan-dir 需要目录路径\n");
                std::fprintf(stderr, "usage: code-insight --scan-dir <dir> [--json]\n");
                return 2;
            }
            scanDirPath = argv[++i];
        } else if (arg == "--json") {
            json = true;
        } else if (arg == "--help" || arg == "-h") {
            std::printf("usage: code-insight [--port N]\n"
                        "       code-insight --scan <file> [--json]        # 单文件扫描，不起服务\n"
                        "       code-insight --scan-dir <dir> [--json]     # 递归扫目录，汇总问题数\n");
            return 0;
        } else {
            std::fprintf(stderr, "error: 未知选项 \"%s\"\n", arg.c_str());
            std::fprintf(stderr, "usage: code-insight [--port N] | --scan <file> | --scan-dir <dir> [--json]\n");
            return 2;
        }
    }

    if (!scanDirPath.empty()) return scanDir(scanDirPath, json);
    if (scanRequested) return scanOneFile(scanPath, json);

#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    ci::Server server(port);
    std::string err;
    if (!server.start(err)) {
        std::fprintf(stderr, "start failed: %s\n", err.c_str());
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    std::printf("code-insight listening on http://127.0.0.1:%d\n", port);
    std::fflush(stdout);
    server.run();

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
