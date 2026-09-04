#include "http_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <fstream>
#include <sstream>

namespace taxi {

HttpServer::HttpServer(Simulator& simulator, std::uint16_t port, std::string webRoot)
    : simulator_(simulator), port_(port), webRoot_(std::move(webRoot)) {}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start() {
    if (running_.load()) {
        return true;
    }

    WSADATA socketData{};
    if (WSAStartup(MAKEWORD(2, 2), &socketData) != 0) {
        lastError_ = "无法初始化 Winsock2";
        return false;
    }

    const SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        lastError_ = "无法创建监听套接字";
        WSACleanup();
        return false;
    }

    BOOL reuseAddress = TRUE;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuseAddress), sizeof(reuseAddress));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port_);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (bind(serverSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        lastError_ = "端口绑定失败，端口可能已被占用";
        closesocket(serverSocket);
        WSACleanup();
        return false;
    }
    if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
        lastError_ = "无法开始监听 HTTP 请求";
        closesocket(serverSocket);
        WSACleanup();
        return false;
    }

    listenSocket_ = static_cast<std::uintptr_t>(serverSocket);
    running_.store(true);
    thread_ = std::thread(&HttpServer::serverLoop, this);
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    const SOCKET serverSocket = static_cast<SOCKET>(listenSocket_);
    if (serverSocket != INVALID_SOCKET) {
        shutdown(serverSocket, SD_BOTH);
        closesocket(serverSocket);
        listenSocket_ = static_cast<std::uintptr_t>(INVALID_SOCKET);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    WSACleanup();
}

std::string HttpServer::lastError() const {
    return lastError_;
}

void HttpServer::serverLoop() {
    const SOCKET serverSocket = static_cast<SOCKET>(listenSocket_);
    while (running_.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(serverSocket, &readSet);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
        const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
        if (!running_.load()) {
            break;
        }
        if (ready <= 0) {
            continue;
        }
        const SOCKET client = accept(serverSocket, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            continue;
        }
        handleClient(static_cast<std::uintptr_t>(client));
        shutdown(client, SD_BOTH);
        closesocket(client);
    }
}

void HttpServer::handleClient(std::uintptr_t rawClientSocket) {
    const SOCKET client = static_cast<SOCKET>(rawClientSocket);
    char buffer[8192];
    const int received = recv(client, buffer, static_cast<int>(sizeof(buffer) - 1), 0);
    if (received <= 0) {
        return;
    }
    buffer[received] = '\0';
    std::istringstream request(std::string(buffer, static_cast<std::size_t>(received)));
    std::string method;
    std::string path;
    std::string version;
    request >> method >> path >> version;
    const std::size_t queryPosition = path.find('?');
    if (queryPosition != std::string::npos) {
        path.erase(queryPosition);
    }

    std::string response;
    if (method == "GET" && path == "/api/snapshot") {
        response = buildResponse(200, "OK", "application/json; charset=utf-8", simulator_.snapshotJson());
    } else if (method == "POST" && path == "/api/control/pause") {
        simulator_.pause();
        response = buildResponse(200, "OK", "application/json; charset=utf-8", "{\"ok\":true}");
    } else if (method == "POST" && path == "/api/control/resume") {
        simulator_.resume();
        response = buildResponse(200, "OK", "application/json; charset=utf-8", "{\"ok\":true}");
    } else if (method == "POST" && path == "/api/control/reset") {
        simulator_.requestReset();
        response = buildResponse(202, "Accepted", "application/json; charset=utf-8", "{\"ok\":true}");
    } else if (method == "GET") {
        std::string contentType;
        const std::string body = readStaticFile(path, contentType);
        if (contentType.empty()) {
            response = buildResponse(404, "Not Found", "text/plain; charset=utf-8", "页面不存在");
        } else {
            response = buildResponse(200, "OK", contentType, body);
        }
    } else {
        response = buildResponse(405, "Method Not Allowed", "text/plain; charset=utf-8", "请求方法不受支持");
    }
    sendAll(rawClientSocket, response);
}

std::string HttpServer::readStaticFile(const std::string& requestPath, std::string& contentType) const {
    std::string fileName;
    if (requestPath == "/" || requestPath == "/index.html") {
        fileName = "index.html";
        contentType = "text/html; charset=utf-8";
    } else if (requestPath == "/style.css") {
        fileName = "style.css";
        contentType = "text/css; charset=utf-8";
    } else if (requestPath == "/app.js") {
        fileName = "app.js";
        contentType = "application/javascript; charset=utf-8";
    } else {
        contentType.clear();
        return {};
    }

    std::ifstream file(webRoot_ + "/" + fileName, std::ios::binary);
    if (!file) {
        contentType.clear();
        return {};
    }
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

bool HttpServer::sendAll(std::uintptr_t rawSocket, const std::string& data) {
    const SOCKET socket = static_cast<SOCKET>(rawSocket);
    std::size_t sentTotal = 0;
    while (sentTotal < data.size()) {
        const int sent = send(socket, data.data() + sentTotal,
                              static_cast<int>(data.size() - sentTotal), 0);
        if (sent == SOCKET_ERROR || sent == 0) {
            return false;
        }
        sentTotal += static_cast<std::size_t>(sent);
    }
    return true;
}

std::string HttpServer::buildResponse(int statusCode, const std::string& statusText,
                                      const std::string& contentType, const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << statusCode << ' ' << statusText << "\r\n"
             << "Content-Type: " << contentType << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Cache-Control: no-store\r\n"
             << "X-Content-Type-Options: nosniff\r\n"
             << "Connection: close\r\n\r\n"
             << body;
    return response.str();
}

}  // namespace taxi
