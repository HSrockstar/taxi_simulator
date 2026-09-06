#include "http_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

namespace taxi {
namespace {

// 单请求读取上限，防止异常客户端把服务内存撑爆
constexpr std::size_t kMaxRequestBytes = 64 * 1024;
// 引擎 tick 为 100ms：推流间隔略宽于一个 tick，前端仍能拿到逐拍细粒度位移
constexpr int kStreamIntervalMs = 120;
constexpr int kStreamPollMs = 30;
constexpr int kStreamPingSeconds = 10;

// 请求头大小写不敏感地提取 Content-Length
int parseContentLength(const std::string& headers) {
    std::string lowered;
    lowered.reserve(headers.size());
    for (const char character : headers) {
        lowered += static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    const std::size_t position = lowered.find("content-length:");
    if (position == std::string::npos) {
        return 0;
    }
    std::size_t index = position + 15;
    while (index < lowered.size() && std::isspace(static_cast<unsigned char>(lowered[index]))) {
        ++index;
    }
    int value = 0;
    while (index < lowered.size() && std::isdigit(static_cast<unsigned char>(lowered[index]))) {
        value = value * 10 + (lowered[index] - '0');
        if (value > static_cast<int>(kMaxRequestBytes)) {
            return static_cast<int>(kMaxRequestBytes);
        }
        ++index;
    }
    return value;
}

// 从扁平 JSON 对象中提取整数字段（本项目请求体只含简单键值对）
bool extractIntField(const std::string& json, const char* key, int& value) {
    const std::string pattern = std::string("\"") + key + "\"";
    const std::size_t keyPosition = json.find(pattern);
    if (keyPosition == std::string::npos) {
        return false;
    }
    const std::size_t colon = json.find(':', keyPosition + pattern.size());
    if (colon == std::string::npos) {
        return false;
    }
    std::size_t index = colon + 1;
    while (index < json.size() && std::isspace(static_cast<unsigned char>(json[index]))) {
        ++index;
    }
    const bool negative = index < json.size() && json[index] == '-';
    if (negative) {
        ++index;
    }
    if (index >= json.size() || !std::isdigit(static_cast<unsigned char>(json[index]))) {
        return false;
    }
    long parsed = 0;
    while (index < json.size() && std::isdigit(static_cast<unsigned char>(json[index]))) {
        parsed = parsed * 10 + (json[index] - '0');
        if (parsed > 1000000) {
            return false;
        }
        ++index;
    }
    value = static_cast<int>(negative ? -parsed : parsed);
    return true;
}

// 静态路径白名单：拒绝 ".."、反斜杠和 URL 编码字符，堵住目录穿越
bool isSafeStaticPath(const std::string& path) {
    if (path.empty() || path.front() != '/' || path.find("..") != std::string::npos ||
        path.find('\\') != std::string::npos) {
        return false;
    }
    for (const unsigned char character : path) {
        if (!std::isalnum(character) && character != '/' && character != '.' &&
            character != '-' && character != '_') {
            return false;
        }
    }
    return true;
}

std::string contentTypeFor(const std::string& fileName) {
    const std::size_t extensionStart = fileName.find_last_of('.');
    if (extensionStart == std::string::npos) {
        return {};
    }
    const std::string extension = fileName.substr(extensionStart);
    if (extension == ".html") return "text/html; charset=utf-8";
    if (extension == ".css") return "text/css; charset=utf-8";
    if (extension == ".js") return "application/javascript; charset=utf-8";
    if (extension == ".json") return "application/json; charset=utf-8";
    if (extension == ".svg") return "image/svg+xml";
    if (extension == ".png") return "image/png";
    if (extension == ".webp") return "image/webp";
    if (extension == ".ico") return "image/x-icon";
    if (extension == ".woff2") return "font/woff2";
    return {};
}

}  // namespace

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
    // 等待连接线程退出（SSE 线程每个轮询周期都会检查 running_）
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (activeConnections_.load() > 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    WSACleanup();
}

std::string HttpServer::lastError() const {
    return lastError_;
}

void HttpServer::serverLoop() {
    const SOCKET serverSocket = static_cast<SOCKET>(listenSocket_);
    // select 带 200ms 超时轮询，accept 不会永久阻塞，stop() 才能及时收线
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
        ++activeConnections_;
        // 连接线程分离运行，单个请求抛异常就地吞掉，不能拖垮整个服务
        std::thread([this, client] {
            try {
                handleClient(static_cast<std::uintptr_t>(client));
            } catch (...) {
            }
            shutdown(client, SD_BOTH);
            closesocket(client);
            --activeConnections_;
        }).detach();
    }
}

std::string HttpServer::readRequest(std::uintptr_t rawClientSocket) const {
    const SOCKET client = static_cast<SOCKET>(rawClientSocket);
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < kMaxRequestBytes) {
        const int received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received <= 0) {
            return {};
        }
        request.append(buffer, static_cast<std::size_t>(received));
    }
    const std::size_t headerEnd = request.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return {};
    }
    const int contentLength = parseContentLength(request.substr(0, headerEnd));
    while (contentLength > 0 &&
           request.size() < headerEnd + 4 + static_cast<std::size_t>(contentLength) &&
           request.size() < kMaxRequestBytes) {
        const int received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received <= 0) {
            return {};
        }
        request.append(buffer, static_cast<std::size_t>(received));
    }
    return request;
}

void HttpServer::handleClient(std::uintptr_t rawClientSocket) {
    const std::string request = readRequest(rawClientSocket);
    if (request.empty()) {
        return;
    }
    std::istringstream parser(request);
    std::string method;
    std::string path;
    std::string version;
    parser >> method >> path >> version;
    const std::size_t queryPosition = path.find('?');
    if (queryPosition != std::string::npos) {
        path.erase(queryPosition);
    }

    std::string response;
    if (method == "GET" && path == "/api/stream") {
        handleStream(rawClientSocket);
        return;
    }
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
    } else if (method == "POST" && path == "/api/params") {
        const std::size_t bodyStart = request.find("\r\n\r\n");
        const std::string body = bodyStart == std::string::npos ? "" : request.substr(bodyStart + 4);
        SimulatorParams params;
        const bool applied =
            extractIntField(body, "driverCount", params.driverCount) &&
            extractIntField(body, "orderRateMin", params.orderRateMin) &&
            extractIntField(body, "orderRateMax", params.orderRateMax) &&
            extractIntField(body, "matchRadius", params.matchRadius) &&
            extractIntField(body, "rebalanceRadius", params.rebalanceRadius) &&
            extractIntField(body, "imbalanceThreshold", params.imbalanceThreshold) &&
            extractIntField(body, "orderTimeout", params.orderTimeout) &&
            simulator_.updateParams(params);
        response = applied
            ? buildResponse(200, "OK", "application/json; charset=utf-8",
                            "{\"ok\":true,\"params\":" + simulator_.paramsJson() + "}")
            : buildResponse(400, "Bad Request", "application/json; charset=utf-8",
                            "{\"ok\":false,\"error\":\"参数无效\"}");
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

void HttpServer::handleStream(std::uintptr_t rawClientSocket) {
    std::ostringstream headers;
    headers << "HTTP/1.1 200 OK\r\n"
            << "Content-Type: text/event-stream; charset=utf-8\r\n"
            << "Cache-Control: no-store\r\n"
            << "X-Accel-Buffering: no\r\n"
            << "Connection: keep-alive\r\n\r\n";
    if (!sendAll(rawClientSocket, headers.str())) {
        return;
    }

    StreamToken lastToken{};
    auto lastSentAt = std::chrono::steady_clock::now() - std::chrono::seconds(kStreamPingSeconds);
    while (running_.load()) {
        // 状态有变化才推完整快照，最密 120ms 一帧；空闲间隔超 10 秒补一条 ping 保活
        const auto now = std::chrono::steady_clock::now();
        const StreamToken token = simulator_.streamToken();
        if (token != lastToken && now - lastSentAt >= std::chrono::milliseconds(kStreamIntervalMs)) {
            const std::string payload = "data: " + simulator_.snapshotJson() + "\n\n";
            if (!sendAll(rawClientSocket, payload)) {
                return;
            }
            lastToken = token;
            lastSentAt = now;
        } else if (now - lastSentAt >= std::chrono::seconds(kStreamPingSeconds)) {
            // SSE 注释行，客户端会忽略；防止空闲连接被代理掐断
            if (!sendAll(rawClientSocket, ": ping\n\n")) {
                return;
            }
            lastSentAt = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kStreamPollMs));
    }
}

std::string HttpServer::readStaticFile(const std::string& requestPath, std::string& contentType) const {
    if (!isSafeStaticPath(requestPath)) {
        contentType.clear();
        return {};
    }

    std::string fileName = requestPath == "/" ? "index.html" : requestPath.substr(1);
    contentType = contentTypeFor(fileName);
    // 无扩展名的路径回落首页，浏览器里直接刷新子路径不 404
    if (contentType.empty() && fileName.find('.') == std::string::npos) {
        fileName = "index.html";
        contentType = "text/html; charset=utf-8";
    }
    if (contentType.empty()) {
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
