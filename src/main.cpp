#include <cstdio>
#include <cstdlib>
#include <string>

#include "server.h"

#ifdef _WIN32
#include <winsock2.h>
#endif

int main(int argc, char** argv) {
    int port = 18080;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::printf("usage: code-insight [--port N]\n");
            return 0;
        }
    }

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
