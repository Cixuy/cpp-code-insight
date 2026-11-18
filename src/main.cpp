#include <cstdio>
#include <cstdlib>
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

int main(int argc, char** argv) {
    int port = 18080;
    std::string scanPath;
    bool json = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if ((arg == "--scan" || arg == "-s") && i + 1 < argc) {
            scanPath = argv[++i];
        } else if (arg == "--json") {
            json = true;
        } else if (arg == "--help" || arg == "-h") {
            std::printf("usage: code-insight [--port N]\n"
                        "       code-insight --scan <file> [--json]   # 单文件扫描，不起服务\n");
            return 0;
        }
    }

    if (!scanPath.empty()) return scanOneFile(scanPath, json);

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
