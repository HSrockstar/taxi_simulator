#include "manual_structures.h"

#include <stdexcept>

namespace taxi {

// 格子数组一次性分配满 10000 个，运行期不再扩容
GridIndex::GridIndex() : cells_(new GridCell[kGridCount]) {}

GridIndex::~GridIndex() {
    delete[] cells_;
}

bool GridIndex::validCoordinate(int value) {
    return value >= 0 && value < kMapSize;
}

int GridIndex::coordinateToCell(int value) {
    if (!validCoordinate(value)) {
        return -1;
    }
    return value / kCellSize;
}

int GridIndex::flatten(int cellX, int cellY) {
    if (cellX < 0 || cellX >= kGridSide || cellY < 0 || cellY >= kGridSide) {
        return -1;
    }
    return cellY * kGridSide + cellX;
}

GridCell& GridIndex::cell(int cellX, int cellY) {
    const int index = flatten(cellX, cellY);
    if (index < 0) {
        throw std::out_of_range("网格坐标越界");
    }
    return cells_[index];
}

const GridCell& GridIndex::cell(int cellX, int cellY) const {
    const int index = flatten(cellX, cellY);
    if (index < 0) {
        throw std::out_of_range("网格坐标越界");
    }
    return cells_[index];
}

GridCell& GridIndex::cellByIndex(int index) {
    if (index < 0 || index >= kGridCount) {
        throw std::out_of_range("网格索引越界");
    }
    return cells_[index];
}

const GridCell& GridIndex::cellByIndex(int index) const {
    if (index < 0 || index >= kGridCount) {
        throw std::out_of_range("网格索引越界");
    }
    return cells_[index];
}

bool GridIndex::addDriver(Driver* driver) {
    if (driver == nullptr || driver->state != DriverState::Idle ||
        !validCoordinate(driver->x) || !validCoordinate(driver->y) || driver->gridIndex >= 0) {
        return false;
    }
    const int index = flatten(coordinateToCell(driver->x), coordinateToCell(driver->y));
    GridCell& target = cells_[index];
    // 头插法，O(1)；链表指针内嵌在司机节点里，不需要额外分配
    driver->previous = nullptr;
    driver->next = target.head;
    if (target.head != nullptr) {
        target.head->previous = driver;
    }
    target.head = driver;
    driver->gridIndex = index;
    ++target.idleCount;
    return true;
}

bool GridIndex::removeDriver(Driver* driver) {
    if (driver == nullptr || driver->gridIndex < 0 || driver->gridIndex >= kGridCount) {
        return false;
    }
    GridCell& source = cells_[driver->gridIndex];
    if (driver->previous != nullptr) {
        driver->previous->next = driver->next;
    } else {
        source.head = driver->next;
    }
    if (driver->next != nullptr) {
        driver->next->previous = driver->previous;
    }
    driver->previous = nullptr;
    driver->next = nullptr;
    driver->gridIndex = -1;
    --source.idleCount;
    return true;
}

bool GridIndex::moveDriver(Driver* driver, int newX, int newY) {
    if (driver == nullptr || !validCoordinate(newX) || !validCoordinate(newY)) {
        return false;
    }
    if (!removeDriver(driver)) {
        return false;
    }
    driver->x = newX;
    driver->y = newY;
    return addDriver(driver);
}

void GridIndex::clear() {
    for (int index = 0; index < kGridCount; ++index) {
        Driver* current = cells_[index].head;
        while (current != nullptr) {
            Driver* next = current->next;
            current->previous = nullptr;
            current->next = nullptr;
            current->gridIndex = -1;
            current = next;
        }
        cells_[index] = GridCell{};
    }
}

OrderQueue::~OrderQueue() {
    clear();
}

void OrderQueue::push(const Order& order) {
    // 节点连同订单副本在锁外构造好，锁只护指针操作
    Node* node = new Node(order);
    std::lock_guard<std::mutex> lock(mutex_);
    if (tail_ == nullptr) {
        head_ = node;
        tail_ = node;
    } else {
        tail_->next = node;
        tail_ = node;
    }
    ++size_;
}

bool OrderQueue::tryPop(Order& order) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (head_ == nullptr) {
        return false;
    }
    Node* node = head_;
    head_ = node->next;
    if (head_ == nullptr) {
        tail_ = nullptr;    // 队列取空时同步收尾指针，否则下次 push 会接在已释放的节点后
    }
    --size_;
    order = node->order;
    delete node;
    return true;
}

std::size_t OrderQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
}

void OrderQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    Node* current = head_;
    while (current != nullptr) {
        Node* next = current->next;
        delete current;
        current = next;
    }
    head_ = nullptr;
    tail_ = nullptr;
    size_ = 0;
}

MinHeap::MinHeap(std::size_t initialCapacity)
    : data_(new MatchCandidate[initialCapacity == 0 ? 1 : initialCapacity]),
      capacity_(initialCapacity == 0 ? 1 : initialCapacity) {}

MinHeap::~MinHeap() {
    delete[] data_;
}

void MinHeap::push(const MatchCandidate& candidate) {
    if (size_ == capacity_) {
        grow();    // 满则申请两倍空间整体搬移
    }
    std::size_t index = size_++;
    data_[index] = candidate;
    // 上浮：比父结点小就交换，一路升到该在的位置，O(log n)
    while (index > 0) {
        const std::size_t parent = (index - 1) / 2;
        if (!candidateLess(data_[index], data_[parent])) {
            break;
        }
        swap(index, parent);
        index = parent;
    }
}

MatchCandidate MinHeap::pop() {
    if (empty()) {
        throw std::underflow_error("最小堆为空");
    }
    MatchCandidate result = data_[0];
    // 末元素补到堆顶再下沉，每层在左右子女里挑更小的交换
    data_[0] = data_[--size_];
    std::size_t index = 0;
    while (true) {
        const std::size_t left = index * 2 + 1;
        const std::size_t right = left + 1;
        std::size_t smallest = index;
        if (left < size_ && candidateLess(data_[left], data_[smallest])) {
            smallest = left;
        }
        if (right < size_ && candidateLess(data_[right], data_[smallest])) {
            smallest = right;
        }
        if (smallest == index) {
            break;
        }
        swap(index, smallest);
        index = smallest;
    }
    return result;
}

const MatchCandidate& MinHeap::top() const {
    if (empty()) {
        throw std::underflow_error("最小堆为空");
    }
    return data_[0];
}

bool MinHeap::empty() const {
    return size_ == 0;
}

std::size_t MinHeap::size() const {
    return size_;
}

void MinHeap::clear() {
    size_ = 0;
}

void MinHeap::grow() {
    // 倍增扩容，均摊到每次 push 的搬移开销是常数
    const std::size_t newCapacity = capacity_ * 2;
    MatchCandidate* replacement = new MatchCandidate[newCapacity];
    for (std::size_t index = 0; index < size_; ++index) {
        replacement[index] = data_[index];
    }
    delete[] data_;
    data_ = replacement;
    capacity_ = newCapacity;
}

void MinHeap::swap(std::size_t left, std::size_t right) {
    const MatchCandidate temporary = data_[left];
    data_[left] = data_[right];
    data_[right] = temporary;
}

void LogRingBuffer::push(const std::string& message) {
    // 未满就接到尾部；写满后覆盖 start_ 处最旧条目，首指针取模前移
    std::size_t position = 0;
    if (size_ < kCapacity) {
        position = (start_ + size_) % kCapacity;
        ++size_;
    } else {
        position = start_;
        start_ = (start_ + 1) % kCapacity;
    }
    entries_[position] = LogEntry{nextSequence_++, message};
}

void LogRingBuffer::clear() {
    start_ = 0;
    size_ = 0;
    nextSequence_ = 1;
}

std::size_t LogRingBuffer::size() const {
    return size_;
}

std::uint64_t LogRingBuffer::lastSequence() const {
    return nextSequence_;
}

void LogRingBuffer::appendRecentJson(std::ostringstream& output, std::size_t limit) const {
    // 从最旧到最新输出最近 limit 条，下标取模回绕；前端拿 sequence 比对，只渲染没见过的
    const std::size_t count = size_ < limit ? size_ : limit;
    const std::size_t first = (start_ + size_ - count) % kCapacity;
    output << '[';
    for (std::size_t offset = 0; offset < count; ++offset) {
        if (offset > 0) {
            output << ',';
        }
        const LogEntry& entry = entries_[(first + offset) % kCapacity];
        output << "{\"sequence\":" << entry.sequence
               << ",\"message\":\"" << escapeJson(entry.message) << "\"}";
    }
    output << ']';
}

// 日志文本里会出现引号、换行这类字符，直接拼进 JSON 会破坏格式，逐个转义
std::string LogRingBuffer::escapeJson(const std::string& text) {
    std::string result;
    result.reserve(text.size() + 8);
    for (const char character : text) {
        switch (character) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += character; break;
        }
    }
    return result;
}

}  // namespace taxi
