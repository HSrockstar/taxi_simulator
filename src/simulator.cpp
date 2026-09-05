#include "simulator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace taxi {
namespace {

constexpr int kHotspotOrigins[3][2] = {
    {200, 200},
    {700, 200},
    {450, 700}
};

}  // namespace

Simulator::Simulator(std::uint32_t seed)
    : seed_(seed), schedulerRandom_(seed ^ 0x9E3779B9U) {
    drivers_ = new Driver[kMaxDriverCount];
    std::lock_guard<std::mutex> lock(stateMutex_);
    resetStateLocked();
}

Simulator::~Simulator() {
    stop();
    delete[] drivers_;
}

void Simulator::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    stopRequested_.store(false);
    schedulerThread_ = std::thread(&Simulator::schedulerLoop, this);
}

void Simulator::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    stopRequested_.store(true);
    controlCondition_.notify_all();
    if (schedulerThread_.joinable()) {
        schedulerThread_.join();
    }
}

void Simulator::pause() {
    paused_.store(true);
    std::lock_guard<std::mutex> lock(stateMutex_);
    logs_.push("[系统] 模拟已暂停");
}

void Simulator::resume() {
    paused_.store(false);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        logs_.push("[系统] 模拟已继续");
    }
    controlCondition_.notify_all();
}

void Simulator::requestReset() {
    resetRequested_.store(true);
    controlCondition_.notify_all();
}

bool Simulator::paused() const {
    return paused_.load();
}

void Simulator::schedulerLoop() {
    while (!stopRequested_.load()) {
        std::unique_lock<std::mutex> lock(controlMutex_);
        controlCondition_.wait_for(lock, std::chrono::seconds(1), [this] {
            return stopRequested_.load() || resetRequested_.load();
        });
        lock.unlock();
        if (stopRequested_.load()) {
            break;
        }
        if (resetRequested_.exchange(false)) {
            std::lock_guard<std::mutex> stateLock(stateMutex_);
            resetStateLocked();
            resetEpoch_.fetch_add(1);
            continue;
        }
        if (!paused_.load()) {
            executeTick();
        }
    }
}

void Simulator::executeTick() {
    const std::uint64_t currentTick = tick_.fetch_add(1) + 1;
    std::lock_guard<std::mutex> lock(stateMutex_);
    processDriverTransitionsLocked(currentTick);
    if (autoGenerate_) {
        generateOrderBatchLocked(currentTick);
    }
    processOrdersLocked(currentTick);
    rebalanceLocked(currentTick);
}

void Simulator::resetStateLocked() {
    orders_.clear();
    grid_.clear();
    logs_.clear();
    tick_.store(0);
    nextOrderId_.store(1);
    generated_.store(0);
    matched_ = 0;
    cancelled_ = 0;
    completed_ = 0;
    matchAttempts_ = 0;
    totalMatchMicros_ = 0;
    schedulerRandom_.seed(seed_ ^ 0x9E3779B9U);

    std::mt19937 initialRandom(seed_);
    std::uniform_int_distribution<int> coordinate(0, kMapSize - 1);
    std::uniform_real_distribution<double> rating(3.5, 5.0);
    for (int index = 0; index < kMaxDriverCount; ++index) {
        drivers_[index] = Driver{};
    }
    for (int index = 0; index < params_.driverCount; ++index) {
        Driver& driver = drivers_[index];
        driver.id = index + 1;
        driver.x = coordinate(initialRandom);
        driver.y = coordinate(initialRandom);
        driver.rating = rating(initialRandom);
        driver.state = DriverState::Idle;
        grid_.addDriver(&driver);
    }
    logs_.push("[系统] 已初始化空闲司机，模拟时间归零");
}

void Simulator::applyFleetChangeLocked(int previousCount, int newCount) {
    if (newCount > previousCount) {
        std::uniform_int_distribution<int> coordinate(0, kMapSize - 1);
        std::uniform_real_distribution<double> rating(3.5, 5.0);
        for (int index = previousCount; index < newCount; ++index) {
            Driver& driver = drivers_[index];
            driver = Driver{};
            driver.id = index + 1;
            driver.x = coordinate(schedulerRandom_);
            driver.y = coordinate(schedulerRandom_);
            driver.rating = rating(schedulerRandom_);
            driver.state = DriverState::Idle;
            grid_.addDriver(&driver);
        }
        return;
    }
    for (int index = previousCount - 1; index >= newCount; --index) {
        grid_.removeDriver(&drivers_[index]);
        drivers_[index] = Driver{};
    }
}

bool Simulator::updateParams(const SimulatorParams& params) {
    if (!SimulatorParams::valid(params)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(stateMutex_);
    const int previousCount = params_.driverCount;
    params_ = params;
    if (params.driverCount != previousCount) {
        applyFleetChangeLocked(previousCount, params.driverCount);
    }

    std::ostringstream message;
    message << "[系统] 参数更新：司机 " << params.driverCount
            << " · 订单率 " << params.orderRateMin << '-' << params.orderRateMax
            << " · 撮合半径 " << params.matchRadius
            << " · 调度半径 " << params.rebalanceRadius
            << " · 失衡阈值 " << params.imbalanceThreshold
            << " · 超时 " << params.orderTimeout << "秒";
    logs_.push(message.str());
    return true;
}

void Simulator::processDriverTransitionsLocked(std::uint64_t currentTick) {
    for (int index = 0; index < params_.driverCount; ++index) {
        Driver& driver = drivers_[index];
        if (driver.state == DriverState::Idle || driver.readyTick > currentTick) {
            continue;
        }
        const DriverState previousState = driver.state;
        driver.x = driver.targetX;
        driver.y = driver.targetY;
        driver.state = DriverState::Idle;
        driver.readyTick = 0;
        driver.activeOrderId = 0;
        grid_.addDriver(&driver);
        if (previousState == DriverState::Serving) {
            ++completed_;
        }
    }
}

void Simulator::generateOrderBatchLocked(std::uint64_t currentTick) {
    std::uniform_int_distribution<int> countDistribution(params_.orderRateMin, params_.orderRateMax);
    std::uniform_int_distribution<int> coordinate(0, kMapSize - 1);
    std::uniform_int_distribution<int> hotspotOffset(0, 29);
    std::uniform_int_distribution<int> probability(1, 100);
    const int hotspotIndex = static_cast<int>((currentTick / 30) % 3);
    const int count = countDistribution(schedulerRandom_);

    for (int index = 0; index < count; ++index) {
        Order order;
        order.id = nextOrderId_.fetch_add(1);
        order.createdTick = currentTick;
        if (probability(schedulerRandom_) <= 80) {
            order.x = kHotspotOrigins[hotspotIndex][0] + hotspotOffset(schedulerRandom_);
            order.y = kHotspotOrigins[hotspotIndex][1] + hotspotOffset(schedulerRandom_);
        } else {
            order.x = coordinate(schedulerRandom_);
            order.y = coordinate(schedulerRandom_);
        }
        orders_.push(order);
        generated_.fetch_add(1);
    }
}

void Simulator::processOrdersLocked(std::uint64_t currentTick) {
    const std::size_t batchSize = orders_.size();
    std::uniform_int_distribution<int> destination(0, kMapSize - 1);
    std::uniform_int_distribution<int> tripDuration(8, 20);

    for (std::size_t index = 0; index < batchSize; ++index) {
        Order order;
        if (!orders_.tryPop(order)) {
            break;
        }
        const int cellX = GridIndex::coordinateToCell(order.x);
        const int cellY = GridIndex::coordinateToCell(order.y);
        GridCell& orderCell = grid_.cell(cellX, cellY);
        if (!order.countedInGrid) {
            ++orderCell.pendingCount;
            order.countedInGrid = true;
        }

        if (currentTick >= order.createdTick + static_cast<std::uint64_t>(params_.orderTimeout)) {
            --orderCell.pendingCount;
            order.state = OrderState::Cancelled;
            ++cancelled_;
            std::ostringstream message;
            message << "[订单取消] 订单#" << order.id << " 等待超过" << params_.orderTimeout << "秒";
            logs_.push(message.str());
            continue;
        }

        double distance = 0.0;
        double score = 0.0;
        const auto startedAt = std::chrono::steady_clock::now();
        Driver* driver = findBestDriverLocked(order, distance, score);
        const auto endedAt = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(endedAt - startedAt).count();
        totalMatchMicros_ += static_cast<std::uint64_t>(elapsed);
        ++matchAttempts_;

        if (driver == nullptr) {
            orders_.push(order);
            continue;
        }

        --orderCell.pendingCount;
        grid_.removeDriver(driver);
        driver->state = DriverState::Serving;
        driver->readyTick = currentTick + static_cast<std::uint64_t>(tripDuration(schedulerRandom_));
        driver->targetX = destination(schedulerRandom_);
        driver->targetY = destination(schedulerRandom_);
        driver->activeOrderId = order.id;
        order.state = OrderState::Matched;
        ++matched_;

        std::ostringstream message;
        message << "[派单成功] 订单#" << order.id
                << " (位置:" << order.x << ',' << order.y << ") 匹配司机#"
                << std::setw(3) << std::setfill('0') << driver->id
                << " (评分:" << std::fixed << std::setprecision(1) << driver->rating
                << ", 距离:" << std::setprecision(1) << distance
                << "米, 得分:" << std::setprecision(2) << score
                << ", 耗时:" << elapsed << "us)";
        logs_.push(message.str());
    }
}

Driver* Simulator::findBestDriverLocked(const Order& order, double& distance, double& score) {
    const int centerX = GridIndex::coordinateToCell(order.x);
    const int centerY = GridIndex::coordinateToCell(order.y);
    MinHeap candidates;

    for (int radius = 0; radius <= params_.matchRadius; ++radius) {
        for (int offsetY = -radius; offsetY <= radius; ++offsetY) {
            for (int offsetX = -radius; offsetX <= radius; ++offsetX) {
                if (std::max(std::abs(offsetX), std::abs(offsetY)) != radius) {
                    continue;
                }
                const int cellX = centerX + offsetX;
                const int cellY = centerY + offsetY;
                if (GridIndex::flatten(cellX, cellY) < 0) {
                    continue;
                }
                Driver* current = grid_.cell(cellX, cellY).head;
                while (current != nullptr) {
                    const double deltaX = static_cast<double>(current->x - order.x);
                    const double deltaY = static_cast<double>(current->y - order.y);
                    const double candidateDistance = std::sqrt(deltaX * deltaX + deltaY * deltaY);
                    candidates.push(MatchCandidate{
                        current,
                        candidateDistance - current->rating,
                        candidateDistance
                    });
                    current = current->next;
                }
            }
        }
    }

    if (candidates.empty()) {
        return nullptr;
    }
    const MatchCandidate best = candidates.pop();
    distance = best.distance;
    score = best.score;
    return best.driver;
}

void Simulator::rebalanceLocked(std::uint64_t currentTick) {
    std::uniform_int_distribution<int> offset(0, kCellSize - 1);
    for (int targetY = 0; targetY < kGridSide; ++targetY) {
        for (int targetX = 0; targetX < kGridSide; ++targetX) {
            GridCell& target = grid_.cell(targetX, targetY);
            int imbalance = target.pendingCount - target.idleCount;
            if (imbalance < params_.imbalanceThreshold) {
                continue;
            }
            int moved = 0;
            while (imbalance >= params_.imbalanceThreshold && moved < 2) {
                Driver* donor = findDonorDriverLocked(targetX, targetY);
                if (donor == nullptr) {
                    break;
                }
                const int sourceIndex = donor->gridIndex;
                grid_.removeDriver(donor);
                donor->state = DriverState::Rebalancing;
                donor->targetX = targetX * kCellSize + offset(schedulerRandom_);
                donor->targetY = targetY * kCellSize + offset(schedulerRandom_);
                donor->readyTick = currentTick + 2;
                ++moved;
                --imbalance;

                std::ostringstream message;
                message << "[运力调度] 司机#" << donor->id << " 从网格#" << sourceIndex
                        << " 调往热点网格#" << GridIndex::flatten(targetX, targetY);
                logs_.push(message.str());
            }
        }
    }
}

Driver* Simulator::findDonorDriverLocked(int targetCellX, int targetCellY) {
    for (int radius = 1; radius <= params_.rebalanceRadius; ++radius) {
        GridCell* bestCell = nullptr;
        int bestSurplus = 0;
        for (int offsetY = -radius; offsetY <= radius; ++offsetY) {
            for (int offsetX = -radius; offsetX <= radius; ++offsetX) {
                if (std::max(std::abs(offsetX), std::abs(offsetY)) != radius) {
                    continue;
                }
                const int cellX = targetCellX + offsetX;
                const int cellY = targetCellY + offsetY;
                if (GridIndex::flatten(cellX, cellY) < 0) {
                    continue;
                }
                GridCell& candidate = grid_.cell(cellX, cellY);
                const int surplus = candidate.idleCount - candidate.pendingCount;
                if (surplus > bestSurplus) {
                    bestSurplus = surplus;
                    bestCell = &candidate;
                }
            }
        }
        if (bestCell != nullptr) {
            return bestCell->head;
        }
    }
    return nullptr;
}

const char* Simulator::driverStateName(DriverState state) {
    switch (state) {
        case DriverState::Idle: return "IDLE";
        case DriverState::Serving: return "SERVING";
        case DriverState::Rebalancing: return "REBALANCING";
    }
    return "UNKNOWN";
}

StreamToken Simulator::streamToken() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return StreamToken{tick_.load(), resetEpoch_.load(), logs_.lastSequence(), paused_.load()};
}

void Simulator::appendParamsJson(std::ostringstream& output) const {
    output << "{\"driverCount\":" << params_.driverCount
           << ",\"orderRateMin\":" << params_.orderRateMin
           << ",\"orderRateMax\":" << params_.orderRateMax
           << ",\"matchRadius\":" << params_.matchRadius
           << ",\"rebalanceRadius\":" << params_.rebalanceRadius
           << ",\"imbalanceThreshold\":" << params_.imbalanceThreshold
           << ",\"orderTimeout\":" << params_.orderTimeout
           << '}';
}

std::string Simulator::paramsJson() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::ostringstream output;
    appendParamsJson(output);
    return output.str();
}

std::string Simulator::snapshotJson() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    std::ostringstream output;
    const std::uint64_t finalized = matched_ + cancelled_;
    const double successRate = finalized == 0
        ? 0.0
        : static_cast<double>(matched_) * 100.0 / static_cast<double>(finalized);
    const double averageMicros = matchAttempts_ == 0
        ? 0.0
        : static_cast<double>(totalMatchMicros_) / static_cast<double>(matchAttempts_);

    output << "{\"tick\":" << tick_.load()
           << ",\"paused\":" << (paused_.load() ? "true" : "false")
           << ",\"hotspotIndex\":" << ((tick_.load() / 30) % 3)
           << ",\"pending\":[";
    for (int index = 0; index < kGridCount; ++index) {
        if (index > 0) output << ',';
        output << grid_.cellByIndex(index).pendingCount;
    }
    output << "],\"idle\":[";
    for (int index = 0; index < kGridCount; ++index) {
        if (index > 0) output << ',';
        output << grid_.cellByIndex(index).idleCount;
    }
    output << "],\"drivers\":[";
    for (int index = 0; index < params_.driverCount; ++index) {
        if (index > 0) output << ',';
        const Driver& driver = drivers_[index];
        output << "{\"id\":" << driver.id
               << ",\"x\":" << driver.x
               << ",\"y\":" << driver.y
               << ",\"rating\":" << std::fixed << std::setprecision(1) << driver.rating
               << ",\"state\":\"" << driverStateName(driver.state) << "\"}";
    }
    output << "],\"metrics\":{\"queueLength\":" << orders_.size()
           << ",\"generated\":" << generated_.load()
           << ",\"matched\":" << matched_
           << ",\"cancelled\":" << cancelled_
           << ",\"completed\":" << completed_
           << ",\"successRate\":" << std::fixed << std::setprecision(2) << successRate
           << ",\"totalMatchMicros\":" << totalMatchMicros_
           << ",\"averageMatchMicros\":" << std::fixed << std::setprecision(2) << averageMicros
           << "},\"params\":";
    appendParamsJson(output);
    output << ",\"logs\":";
    logs_.appendRecentJson(output, 50);
    output << '}';
    return output.str();
}

#ifdef TAXI_TESTING
void Simulator::testClearState() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    orders_.clear();
    grid_.clear();
    logs_.clear();
    tick_.store(0);
    generated_.store(0);
    matched_ = cancelled_ = completed_ = matchAttempts_ = totalMatchMicros_ = 0;
    autoGenerate_ = false;
    for (int index = 0; index < kMaxDriverCount; ++index) {
        drivers_[index] = Driver{};
    }
}

void Simulator::testSetAutoGenerate(bool enabled) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    autoGenerate_ = enabled;
}

void Simulator::testAddDriver(int slot, int id, int x, int y, double rating) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    drivers_[slot] = Driver{};
    drivers_[slot].id = id;
    drivers_[slot].x = x;
    drivers_[slot].y = y;
    drivers_[slot].rating = rating;
    drivers_[slot].state = DriverState::Idle;
    grid_.addDriver(&drivers_[slot]);
}

void Simulator::testPushOrder(std::uint64_t id, int x, int y, std::uint64_t createdTick) {
    orders_.push(Order{id, x, y, createdTick, OrderState::Waiting, false});
    generated_.fetch_add(1);
}

void Simulator::testTick() {
    executeTick();
}

Driver Simulator::testDriver(int slot) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return drivers_[slot];
}

std::size_t Simulator::testQueueSize() const { return orders_.size(); }
std::uint64_t Simulator::testGenerated() const { return generated_.load(); }
std::uint64_t Simulator::testMatched() const { std::lock_guard<std::mutex> lock(stateMutex_); return matched_; }
std::uint64_t Simulator::testCancelled() const { std::lock_guard<std::mutex> lock(stateMutex_); return cancelled_; }
std::uint64_t Simulator::testCompleted() const { std::lock_guard<std::mutex> lock(stateMutex_); return completed_; }
#endif

}  // namespace taxi
