#pragma once

#include "model.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>

namespace taxi {

// 手写核心数据结构：网格空间索引、订单队列、最小堆、日志环形缓冲。
// 题目要求网格/队列/堆不得用 STL 容器替代，实现全部基于原生数组与裸指针。

struct GridCell {
    Driver* head = nullptr;
    int idleCount = 0;
    int pendingCount = 0;
    // 道路通行系数：1.0 为畅通，数值越高代表同一路段耗时越长。
    // 该字段与供需计数共用网格索引，避免引入额外的空间数据库。
    double trafficFactor = 1.0;
};

// 100×100 网格矩阵：坐标到格子只做两次整除，O(1) 定位；
// 每格挂一条空闲司机双向链表，链表指针内嵌在 Driver 里，增删与跨格迁移均 O(1)。
class GridIndex {
public:
    GridIndex();
    ~GridIndex();

    GridIndex(const GridIndex&) = delete;
    GridIndex& operator=(const GridIndex&) = delete;

    static bool validCoordinate(int value);
    static int coordinateToCell(int value);
    static int flatten(int cellX, int cellY);

    GridCell& cell(int cellX, int cellY);
    const GridCell& cell(int cellX, int cellY) const;
    GridCell& cellByIndex(int index);
    const GridCell& cellByIndex(int index) const;

    // 只收空闲且尚未入格的司机（gridIndex < 0），重复插入会破坏链表，拒绝
    bool addDriver(Driver* driver);
    // 按司机记录的格号直接定位，节点在链表中部也无需从头查找
    bool removeDriver(Driver* driver);
    // 摘除、改坐标、入格三步封装，坐标与链表索引不会出现中间态
    bool moveDriver(Driver* driver, int newX, int newY);
    // 只清链表与计数，司机本体归 Simulator 的槽位数组所有，这里不 delete
    void clear();

private:
    GridCell* cells_;
};

// 先来先服务的链式队列：单链表带头尾指针，入队尾插、出队取头，均 O(1)。
// 自带互斥锁，节点由队列持有，出队时把订单副本交给调用方并释放节点。
class OrderQueue {
public:
    OrderQueue() = default;
    ~OrderQueue();

    OrderQueue(const OrderQueue&) = delete;
    OrderQueue& operator=(const OrderQueue&) = delete;

    void push(const Order& order);
    bool tryPop(Order& order);
    std::size_t size() const;
    void clear();

private:
    struct Node {
        explicit Node(const Order& value) : order(value) {}
        Order order;
        Node* next = nullptr;
    };

    mutable std::mutex mutex_;
    Node* head_ = nullptr;
    Node* tail_ = nullptr;
    std::size_t size_ = 0;
};

// 动态数组存储的二叉最小堆，堆顶即撮合代价最小的候选。
// 比较规则走 model.h 的 candidateLess 决胜链，同分候选的弹出顺序也是确定的。
class MinHeap {
public:
    explicit MinHeap(std::size_t initialCapacity = 16);
    ~MinHeap();

    MinHeap(const MinHeap&) = delete;
    MinHeap& operator=(const MinHeap&) = delete;

    void push(const MatchCandidate& candidate);
    MatchCandidate pop();
    const MatchCandidate& top() const;
    bool empty() const;
    std::size_t size() const;
    void clear();

private:
    void grow();
    void swap(std::size_t left, std::size_t right);

    MatchCandidate* data_;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
};

struct LogEntry {
    std::uint64_t sequence = 0;
    std::string message;
};

// 定长 200 的日志环形缓冲：写满覆盖最旧条目，内存占用恒定。
// 每条日志带单调递增序号，HTTP 层据此给前端做增量推送，不重不漏。
class LogRingBuffer {
public:
    static constexpr std::size_t kCapacity = 200;

    void push(const std::string& message);
    void clear();
    std::size_t size() const;
    std::uint64_t lastSequence() const;
    void appendRecentJson(std::ostringstream& output, std::size_t limit) const;

private:
    static std::string escapeJson(const std::string& text);

    LogEntry entries_[kCapacity];
    std::size_t start_ = 0;
    std::size_t size_ = 0;
    std::uint64_t nextSequence_ = 1;
};

}  // namespace taxi
