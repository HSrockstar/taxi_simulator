#include "http_server.h"
#include "manual_structures.h"
#include "simulator.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

namespace {

int passed = 0;
int failed = 0;

void check(bool condition, const std::string& name) {
    if (condition) {
        ++passed;
        std::cout << "[通过] " << name << '\n';
    } else {
        ++failed;
        std::cerr << "[失败] " << name << '\n';
    }
}

std::string httpRequest(std::uint16_t port, const std::string& request) {
    const SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET) {
        return {};
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        closesocket(socketHandle);
        return {};
    }
    send(socketHandle, request.data(), static_cast<int>(request.size()), 0);
    std::string response;
    char buffer[4096];
    while (true) {
        const int count = recv(socketHandle, buffer, static_cast<int>(sizeof(buffer)), 0);
        if (count <= 0) break;
        response.append(buffer, static_cast<std::size_t>(count));
    }
    closesocket(socketHandle);
    return response;
}

void testGridIndex() {
    taxi::GridIndex grid;
    check(taxi::GridIndex::coordinateToCell(0) == 0, "坐标下边界映射");
    check(taxi::GridIndex::coordinateToCell(999) == 99, "坐标上边界映射");
    check(taxi::GridIndex::coordinateToCell(-1) == -1 &&
          taxi::GridIndex::coordinateToCell(1000) == -1, "非法坐标被拒绝");

    taxi::Driver first{1, 11, 12, 4.5};
    taxi::Driver second{2, 13, 14, 4.6};
    taxi::Driver third{3, 15, 16, 4.7};
    check(grid.addDriver(&first) && grid.addDriver(&second) && grid.addDriver(&third), "链表插入司机");
    taxi::GridCell& cell = grid.cell(1, 1);
    check(cell.idleCount == 3 && cell.head == &third, "链表头插法与计数正确");
    check(grid.removeDriver(&second) && third.next == &first && first.previous == &third,
          "O(1) 删除链表中部节点");
    check(grid.moveDriver(&first, 221, 332) && first.gridIndex == taxi::GridIndex::flatten(22, 33),
          "司机跨网格迁移");
}

void testOrderQueue() {
    taxi::OrderQueue queue;
    queue.push(taxi::Order{1});
    queue.push(taxi::Order{2});
    taxi::Order order;
    check(queue.tryPop(order) && order.id == 1, "订单队列遵循 FIFO");
    check(queue.tryPop(order) && order.id == 2 && !queue.tryPop(order), "队列尾部和空队列处理");

    constexpr int producerCount = 4;
    constexpr int perProducer = 250;
    std::thread producers[producerCount];
    for (int producer = 0; producer < producerCount; ++producer) {
        producers[producer] = std::thread([producer, &queue] {
            for (int index = 0; index < perProducer; ++index) {
                queue.push(taxi::Order{static_cast<std::uint64_t>(producer * perProducer + index + 1)});
            }
        });
    }
    for (std::thread& producer : producers) producer.join();
    bool seen[producerCount * perProducer + 1]{};
    int popped = 0;
    while (queue.tryPop(order)) {
        if (order.id <= producerCount * perProducer) seen[order.id] = true;
        ++popped;
    }
    bool allSeen = popped == producerCount * perProducer;
    for (int id = 1; id <= producerCount * perProducer; ++id) allSeen = allSeen && seen[id];
    check(allSeen, "多生产者并发入队不丢失订单");
}

void testMinHeap() {
    taxi::Driver drivers[5];
    for (int index = 0; index < 5; ++index) {
        drivers[index].id = index + 1;
        drivers[index].rating = 4.0 + index * 0.1;
    }
    taxi::MinHeap heap(2);
    heap.push({&drivers[0], 7.0, 10.0});
    heap.push({&drivers[1], 2.0, 6.0});
    heap.push({&drivers[2], 4.0, 8.0});
    heap.push({&drivers[3], 2.0, 5.0});
    heap.push({&drivers[4], 9.0, 12.0});
    check(heap.size() == 5, "最小堆手工扩容");
    check(heap.pop().driver->id == 4 && heap.pop().driver->id == 2,
          "最小堆按得分和距离排序");

    taxi::MinHeap tieHeap;
    drivers[0].rating = 4.5;
    drivers[1].rating = 4.8;
    tieHeap.push({&drivers[0], 1.0, 5.0});
    tieHeap.push({&drivers[1], 1.0, 5.0});
    check(tieHeap.pop().driver->id == 2, "平分时优先高评分司机");
}

void testSimulatorAlgorithms() {
    taxi::Simulator simulator(12345);
    simulator.testClearState();
    simulator.testAddDriver(0, 1, 105, 100, 4.0);
    simulator.testAddDriver(1, 2, 95, 100, 5.0);
    simulator.testPushOrder(1, 100, 100);
    simulator.testTick();
    check(simulator.testMatched() == 1 && simulator.testQueueSize() == 0,
          "订单匹配得分最优司机");
    check(simulator.testDriver(1).state == taxi::DriverState::Serving &&
          simulator.testDriver(0).state == taxi::DriverState::Idle,
          "司机不会被重复派单且高评分者胜出");
    for (int tick = 0; tick < 20; ++tick) simulator.testTick();
    check(simulator.testCompleted() == 1 && simulator.testDriver(1).state == taxi::DriverState::Idle,
          "行程完成后司机重新进入空闲状态");

    simulator.testClearState();
    simulator.testPushOrder(2, 500, 500);
    for (int tick = 0; tick < 10; ++tick) simulator.testTick();
    check(simulator.testCancelled() == 1 && simulator.testQueueSize() == 0,
          "订单等待十秒后超时取消");

    simulator.testClearState();
    simulator.testAddDriver(0, 10, 255, 205, 4.8);
    simulator.testPushOrder(10, 205, 205);
    simulator.testPushOrder(11, 205, 205);
    simulator.testPushOrder(12, 205, 205);
    simulator.testTick();
    check(simulator.testDriver(0).state == taxi::DriverState::Rebalancing,
          "远处富余司机被调往热点网格");
    simulator.testTick();
    simulator.testTick();
    check(simulator.testMatched() == 1 && simulator.testQueueSize() == 2,
          "调度到达后的司机参与下一轮撮合");

    const std::string snapshot = simulator.snapshotJson();
    check(snapshot.find("\"pending\":[") != std::string::npos &&
          snapshot.find("\"idle\":[") != std::string::npos &&
          snapshot.find("\"successRate\":") != std::string::npos,
          "快照包含网格数组和全局指标");
}

void testHttpServer() {
    constexpr std::uint16_t port = 18081;
    taxi::Simulator simulator(77);
    taxi::HttpServer server(simulator, port, "web");
    check(server.start(), "HTTP 服务启动");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    const std::string snapshot = httpRequest(port,
        "GET /api/snapshot HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    check(snapshot.find("HTTP/1.1 200 OK") != std::string::npos &&
          snapshot.find("\"drivers\":[") != std::string::npos,
          "HTTP 快照接口返回 JSON");

    const std::string pause = httpRequest(port,
        "POST /api/control/pause HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\n\r\n");
    check(pause.find("HTTP/1.1 200 OK") != std::string::npos && simulator.paused(),
          "HTTP 暂停控制接口");

    const std::string staticFile = httpRequest(port,
        "GET /style.css HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    check(staticFile.find("HTTP/1.1 200 OK") != std::string::npos &&
          staticFile.find("text/css") != std::string::npos,
          "HTTP 静态资源白名单");

    const std::string missing = httpRequest(port,
        "GET /secret.txt HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
    check(missing.find("HTTP/1.1 404 Not Found") != std::string::npos,
          "未授权静态路径返回 404");
    server.stop();
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    testGridIndex();
    testOrderQueue();
    testMinHeap();
    testSimulatorAlgorithms();
    testHttpServer();
    std::cout << "\n测试完成：通过 " << passed << "，失败 " << failed << '\n';
    return failed == 0 ? 0 : 1;
}
