#include "http_server.h"
#include "simulator.h"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> keepRunning{true};

BOOL WINAPI handleConsoleSignal(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT || signal == CTRL_BREAK_EVENT) {
        keepRunning.store(false);
        return TRUE;
    }
    return FALSE;
}

bool parseUnsigned(const char* text, unsigned long& value) {
    char* end = nullptr;
    value = std::strtoul(text, &end, 10);
    return text[0] != '\0' && end != nullptr && *end == '\0';
}

}  // namespace

int main(int argc, char* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(handleConsoleSignal, TRUE);

    std::uint16_t port = 8080;
    std::uint32_t seed = 20260808U;
    unsigned long durationSeconds = 0;
    bool openBrowser = true;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        unsigned long value = 0;
        if (argument == "--port" && index + 1 < argc && parseUnsigned(argv[++index], value) && value <= 65535) {
            port = static_cast<std::uint16_t>(value);
        } else if (argument == "--seed" && index + 1 < argc && parseUnsigned(argv[++index], value)) {
            seed = static_cast<std::uint32_t>(value);
        } else if (argument == "--duration" && index + 1 < argc && parseUnsigned(argv[++index], value)) {
            durationSeconds = value;
        } else if (argument == "--no-browser") {
            openBrowser = false;
        } else {
            std::cerr << "参数无效。用法: taxi_simulator.exe [--port 8080] [--seed 20260808]"
                      << " [--duration 秒数] [--no-browser]\n";
            return 2;
        }
    }

    taxi::Simulator simulator(seed);
    taxi::HttpServer server(simulator, port, "web");
    if (!server.start()) {
        std::cerr << "HTTP 服务启动失败：" << server.lastError() << '\n';
        return 1;
    }
    simulator.start();

    const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/";
    std::cout << "智能网约车模拟器已启动：" << url << '\n'
              << "按 Ctrl+C 安全退出。\n";
    if (openBrowser) {
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    const auto startedAt = std::chrono::steady_clock::now();
    while (keepRunning.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (durationSeconds > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - startedAt).count();
            if (static_cast<unsigned long>(elapsed) >= durationSeconds) {
                break;
            }
        }
    }

    server.stop();
    simulator.stop();
    std::cout << "模拟器已安全退出。\n";
    return 0;
}
