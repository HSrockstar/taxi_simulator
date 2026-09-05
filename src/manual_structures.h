#pragma once

#include "model.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>

namespace taxi {

struct GridCell {
    Driver* head = nullptr;
    int idleCount = 0;
    int pendingCount = 0;
};

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

    bool addDriver(Driver* driver);
    bool removeDriver(Driver* driver);
    bool moveDriver(Driver* driver, int newX, int newY);
    void clear();

private:
    GridCell* cells_;
};

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
