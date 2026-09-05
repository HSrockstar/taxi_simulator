#pragma once

#include "simulator.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace taxi {

class HttpServer {
public:
    HttpServer(Simulator& simulator, std::uint16_t port, std::string webRoot);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    bool start();
    void stop();
    std::string lastError() const;

private:
    void serverLoop();
    void handleClient(std::uintptr_t clientSocket);
    void handleStream(std::uintptr_t clientSocket);
    std::string readRequest(std::uintptr_t clientSocket) const;
    std::string readStaticFile(const std::string& requestPath, std::string& contentType) const;
    static bool sendAll(std::uintptr_t socket, const std::string& data);
    static std::string buildResponse(int statusCode, const std::string& statusText,
                                     const std::string& contentType, const std::string& body);

    Simulator& simulator_;
    std::uint16_t port_;
    std::string webRoot_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> activeConnections_{0};
    std::uintptr_t listenSocket_ = static_cast<std::uintptr_t>(-1);
    std::string lastError_;
};

}  // namespace taxi
