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

// 接客在途时长：按撮合距离估算（约 300 米/模拟秒，每 tick 30 米），最短 2 秒保证"前往接客"状态可见
int pickupTicksFor(double distance) {
    const int ticks = static_cast<int>(std::ceil(distance / (300.0 / kTicksPerSecond)));
    return std::clamp(ticks, 2 * kTicksPerSecond, 6 * kTicksPerSecond);
}

// 空闲司机游走：每 tick 5% 概率移动一格（平均每秒 0.5 步）；调度半径外的司机迈步时 70% 朝当前活跃热点漂移。
// 仅作为"撮合半径外运力静止"的背景补充，半径内运力仍由显式调度机制主导
constexpr double kIdleWanderProbability = 0.05;
constexpr double kIdleDriftBias = 0.7;

// 简化路况模型：按穿过网格的通行系数估算接驾 ETA，而非引入道路拓扑。
constexpr double kBaseRoadSpeedMetersPerSecond = 10.0;
constexpr double kRatingTimeCreditSeconds = 0.5;
constexpr int kTrafficRefreshTicks = 5 * kTicksPerSecond;

void raiseTrafficBlob(GridIndex& grid, int centerX, int centerY, int radius, double factor) {
    for (int offsetY = -radius; offsetY <= radius; ++offsetY) {
        for (int offsetX = -radius; offsetX <= radius; ++offsetX) {
            if (std::max(std::abs(offsetX), std::abs(offsetY)) > radius) {
                continue;
            }
            const int cellX = centerX + offsetX;
            const int cellY = centerY + offsetY;
            if (GridIndex::flatten(cellX, cellY) >= 0) {
                GridCell& cell = grid.cell(cellX, cellY);
                cell.trafficFactor = std::max(cell.trafficFactor, factor);
            }
        }
    }
}

}  // namespace

Simulator::Simulator(std::uint32_t seed)
    : seed_(seed), schedulerRandom_(seed ^ 0x9E3779B9U) {
    drivers_ = new Driver[kMaxDriverCount];
    activeOrders_ = new Order[kMaxDriverCount];
    std::lock_guard<std::mutex> lock(stateMutex_);
    resetStateLocked();
}

Simulator::~Simulator() {
    stop();
    delete[] drivers_;
    delete[] activeOrders_;
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
        controlCondition_.wait_for(lock, std::chrono::milliseconds(1000 / kTicksPerSecond), [this] {
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
    if (currentTick % kTrafficRefreshTicks == 0) {
        refreshTrafficLocked(currentTick);
    }
    processDriverTransitionsLocked(currentTick);
    // 订单率参数的语义是"单/秒"，每个模拟秒（kTicksPerSecond 个 tick）批量生成一次
    if (autoGenerate_ && currentTick % kTicksPerSecond == 0) {
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
        activeOrders_[index] = Order{};
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
    refreshTrafficLocked(0);
    logs_.push("[系统] 已初始化空闲司机，模拟时间归零");
}

void Simulator::refreshTrafficLocked(std::uint64_t currentTick) {
    for (int index = 0; index < kGridCount; ++index) {
        grid_.cellByIndex(index).trafficFactor = 1.0;
    }

    const int activeHotspot = static_cast<int>((currentTick / (30 * kTicksPerSecond)) % 3);
    const int hotCellX = kHotspotOrigins[activeHotspot][0] / kCellSize;
    const int hotCellY = kHotspotOrigins[activeHotspot][1] / kCellSize;
    raiseTrafficBlob(grid_, hotCellX, hotCellY, 4, 1.5);
    raiseTrafficBlob(grid_, hotCellX, hotCellY, 2, 2.5);

    // 两处周期性事故点由模拟时间确定，既有动态变化，也保持同种子场景可复现。
    const int phase = static_cast<int>(currentTick / kTrafficRefreshTicks);
    for (int incident = 0; incident < 2; ++incident) {
        const int cellX = (phase * 17 + 31 + incident * 43) % kGridSide;
        const int cellY = (phase * 29 + 19 + incident * 37) % kGridSide;
        raiseTrafficBlob(grid_, cellX, cellY, 2, 1.5);
        raiseTrafficBlob(grid_, cellX, cellY, 1, 2.5);
    }
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
        Driver& driver = drivers_[index];
        if (driver.activeOrderId != 0) {
            Order& order = activeOrders_[index];
            order.state = OrderState::Cancelled;
            ++cancelled_;
            std::ostringstream message;
            message << "[订单取消] 订单#" << order.id << " 司机下线服务中断";
            logs_.push(message.str());
        }
        grid_.removeDriver(&driver);
        driver = Driver{};
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
        if (driver.state == DriverState::Idle) {
            stepIdleDriverLocked(driver, currentTick);
            continue;
        }
        if (driver.state == DriverState::EnRoute || driver.state == DriverState::OnTrip) {
            stepTripDriverLocked(index, driver, currentTick);
            continue;
        }
        // 调度行程固定 2 秒（2 * kTicksPerSecond 个 tick），到达热点格后回归空闲并入格
        if (driver.readyTick > currentTick) {
            continue;
        }
        driver.x = driver.targetX;
        driver.y = driver.targetY;
        driver.state = DriverState::Idle;
        driver.readyTick = 0;
        driver.activeOrderId = 0;
        grid_.addDriver(&driver);
    }
}

void Simulator::stepIdleDriverLocked(Driver& driver, std::uint64_t currentTick) {
    if (!idleWander_) {
        return;
    }
    std::uniform_real_distribution<double> roll(0.0, 1.0);
    if (roll(schedulerRandom_) >= kIdleWanderProbability) {
        return;
    }

    // 调度半径外的空闲车做定向漂移（平台调度够不着），半径内保持纯随机游走，
    // 让"绿转红"运力转移的可视化仍由显式调度机制呈现
    const int (&origin)[2] = kHotspotOrigins[(currentTick / (30 * kTicksPerSecond)) % 3];
    const int hotCellX = origin[0] / kCellSize;
    const int hotCellY = origin[1] / kCellSize;
    const int myCellX = GridIndex::coordinateToCell(driver.x);
    const int myCellY = GridIndex::coordinateToCell(driver.y);
    const int gap = std::max(std::abs(myCellX - hotCellX), std::abs(myCellY - hotCellY));

    int stepX = 0;
    int stepY = 0;
    if (gap > params_.rebalanceRadius && roll(schedulerRandom_) < kIdleDriftBias) {
        stepX = (hotCellX > myCellX) - (hotCellX < myCellX);
        stepY = (hotCellY > myCellY) - (hotCellY < myCellY);
    } else {
        std::uniform_int_distribution<int> direction(-1, 1);
        stepX = direction(schedulerRandom_);
        stepY = direction(schedulerRandom_);
    }
    if (stepX == 0 && stepY == 0) {
        return;
    }
    const int newX = std::clamp(driver.x + stepX * kCellSize, 0, kMapSize - 1);
    const int newY = std::clamp(driver.y + stepY * kCellSize, 0, kMapSize - 1);
    grid_.moveDriver(&driver, newX, newY);
}

void Simulator::stepTripDriverLocked(int slot, Driver& driver, std::uint64_t currentTick) {
    if (driver.readyTick > currentTick) {
        // 剩余路程按剩余 tick 数均摊逐步推进，最后一 tick 由到达分支精确落点
        const double remaining = static_cast<double>(driver.readyTick - currentTick);
        driver.x += static_cast<int>(std::lround((driver.targetX - driver.x) / remaining));
        driver.y += static_cast<int>(std::lround((driver.targetY - driver.y) / remaining));
        return;
    }
    if (driver.state == DriverState::EnRoute) {
        // 到达上车点：接到乘客，转入行程中并生成前往目的地的行程
        driver.x = driver.targetX;
        driver.y = driver.targetY;
        driver.state = DriverState::OnTrip;
        std::uniform_int_distribution<int> destination(0, kMapSize - 1);
        std::uniform_int_distribution<int> tripDuration(8 * kTicksPerSecond, 20 * kTicksPerSecond);
        driver.readyTick = currentTick + static_cast<std::uint64_t>(tripDuration(schedulerRandom_));
        driver.targetX = destination(schedulerRandom_);
        driver.targetY = destination(schedulerRandom_);
        std::ostringstream message;
        message << "[行程开始] 订单#" << activeOrders_[slot].id << " 乘客已上车，司机#"
                << std::setw(3) << std::setfill('0') << driver.id << " 前往目的地";
        logs_.push(message.str());
        return;
    }
    // 行程到达目的地：订单流转为已完成，司机回归空闲并入格参与下一轮撮合
    driver.x = driver.targetX;
    driver.y = driver.targetY;
    driver.state = DriverState::Idle;
    driver.readyTick = 0;
    Order& order = activeOrders_[slot];
    order.state = OrderState::Completed;
    ++completed_;
    const double tripSeconds = static_cast<double>(currentTick - order.createdTick) / kTicksPerSecond;
    std::ostringstream message;
    message << "[行程完成] 订单#" << order.id << " 由司机#"
            << std::setw(3) << std::setfill('0') << driver.id
            << " 完成，全程耗时 " << std::fixed << std::setprecision(1) << tripSeconds << " 秒";
    logs_.push(message.str());
    driver.activeOrderId = 0;
    grid_.addDriver(&driver);
}

void Simulator::generateOrderBatchLocked(std::uint64_t currentTick) {
    std::uniform_int_distribution<int> countDistribution(params_.orderRateMin, params_.orderRateMax);
    std::uniform_int_distribution<int> coordinate(0, kMapSize - 1);
    std::uniform_int_distribution<int> hotspotOffset(0, 29);
    std::uniform_int_distribution<int> probability(1, 100);
    const int hotspotIndex = static_cast<int>((currentTick / (30 * kTicksPerSecond)) % 3);
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

        if (currentTick >= order.createdTick + static_cast<std::uint64_t>(params_.orderTimeout * kTicksPerSecond)) {
            --orderCell.pendingCount;
            order.state = OrderState::Cancelled;
            ++cancelled_;
            std::ostringstream message;
            message << "[订单取消] 订单#" << order.id << " 等待超过" << params_.orderTimeout << "秒";
            logs_.push(message.str());
            continue;
        }

        double distance = 0.0;
        double etaSeconds = 0.0;
        double score = 0.0;
        const auto startedAt = std::chrono::steady_clock::now();
        Driver* driver = findBestDriverLocked(order, distance, etaSeconds, score);
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
        const int slot = static_cast<int>(driver - drivers_);
        driver->state = DriverState::EnRoute;
        driver->readyTick = currentTick + static_cast<std::uint64_t>(pickupTicksFor(distance));
        driver->targetX = order.x;
        driver->targetY = order.y;
        driver->activeOrderId = order.id;
        order.state = OrderState::Matched;
        activeOrders_[slot] = order;
        ++matched_;

        std::ostringstream message;
        const double averageTrafficFactor = distance > 1e-9
            ? etaSeconds * kBaseRoadSpeedMetersPerSecond / distance
            : grid_.cell(cellX, cellY).trafficFactor;
        message << "[派单成功] 订单#" << order.id
                << " (位置:" << order.x << ',' << order.y << ") 匹配司机#"
                << std::setw(3) << std::setfill('0') << driver->id
                << " (评分:" << std::fixed << std::setprecision(1) << driver->rating
                << ", 距离:" << std::setprecision(1) << distance
                << "米, 预计接驾:" << std::setprecision(1) << etaSeconds
                << "秒, 平均路况系数:" << std::setprecision(2) << averageTrafficFactor
                << ", 得分:" << std::setprecision(2) << score
                << ", 耗时:" << elapsed << "us)";
        logs_.push(message.str());
    }
}

double Simulator::estimateTravelTimeLocked(int fromX, int fromY, int toX, int toY) const {
    const double deltaX = static_cast<double>(toX - fromX);
    const double deltaY = static_cast<double>(toY - fromY);
    const double distance = std::sqrt(deltaX * deltaX + deltaY * deltaY);
    if (distance <= 1e-9) {
        const int cellX = GridIndex::coordinateToCell(fromX);
        const int cellY = GridIndex::coordinateToCell(fromY);
        return grid_.cell(cellX, cellY).trafficFactor * 0.0;
    }

    const int fromCellX = GridIndex::coordinateToCell(fromX);
    const int fromCellY = GridIndex::coordinateToCell(fromY);
    const int toCellX = GridIndex::coordinateToCell(toX);
    const int toCellY = GridIndex::coordinateToCell(toY);
    const int segments = std::max(std::abs(toCellX - fromCellX),
                                  std::abs(toCellY - fromCellY)) + 1;
    const double segmentDistance = distance / segments;
    double weightedDistance = 0.0;
    for (int segment = 0; segment < segments; ++segment) {
        const double ratio = (static_cast<double>(segment) + 0.5) / segments;
        const int sampleX = std::clamp(static_cast<int>(std::lround(fromX + deltaX * ratio)),
                                       0, kMapSize - 1);
        const int sampleY = std::clamp(static_cast<int>(std::lround(fromY + deltaY * ratio)),
                                       0, kMapSize - 1);
        const int cellX = GridIndex::coordinateToCell(sampleX);
        const int cellY = GridIndex::coordinateToCell(sampleY);
        weightedDistance += segmentDistance * grid_.cell(cellX, cellY).trafficFactor;
    }
    return weightedDistance / kBaseRoadSpeedMetersPerSecond;
}

Driver* Simulator::findBestDriverLocked(const Order& order, double& distance,
                                        double& etaSeconds, double& score) {
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
                    const double candidateEta = estimateTravelTimeLocked(
                        current->x, current->y, order.x, order.y);
                    candidates.push(MatchCandidate{
                        current,
                        candidateEta - kRatingTimeCreditSeconds * current->rating,
                        candidateDistance,
                        candidateEta
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
    etaSeconds = best.etaSeconds;
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
                donor->readyTick = currentTick + 2 * kTicksPerSecond;
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
    // 与撮合同款 ETA 评分公式：只从富余网格取供体，
    // 半径内所有候选压入最小堆，堆顶即预计到达最快、评分最高的司机。
    const int centerX = targetCellX * kCellSize + kCellSize / 2;
    const int centerY = targetCellY * kCellSize + kCellSize / 2;
    MinHeap candidates;

    for (int radius = 1; radius <= params_.rebalanceRadius; ++radius) {
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
                if (candidate.idleCount - candidate.pendingCount <= 0) {
                    continue;
                }
                for (Driver* current = candidate.head; current != nullptr; current = current->next) {
                    const double deltaX = static_cast<double>(current->x - centerX);
                    const double deltaY = static_cast<double>(current->y - centerY);
                    const double distance = std::sqrt(deltaX * deltaX + deltaY * deltaY);
                    const double etaSeconds = estimateTravelTimeLocked(
                        current->x, current->y, centerX, centerY);
                    candidates.push(MatchCandidate{
                        current,
                        etaSeconds - kRatingTimeCreditSeconds * current->rating,
                        distance,
                        etaSeconds
                    });
                }
            }
        }
    }

    if (candidates.empty()) {
        return nullptr;
    }
    return candidates.pop().driver;
}

const char* Simulator::driverStateName(DriverState state) {
    switch (state) {
        case DriverState::Idle: return "IDLE";
        case DriverState::EnRoute: return "EN_ROUTE";
        case DriverState::OnTrip: return "ON_TRIP";
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
           << ",\"hotspotIndex\":" << ((tick_.load() / (30 * kTicksPerSecond)) % 3)
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
    output << "],\"traffic\":[";
    for (int index = 0; index < kGridCount; ++index) {
        if (index > 0) output << ',';
        output << std::fixed << std::setprecision(1) << grid_.cellByIndex(index).trafficFactor;
    }
    output << "],\"drivers\":[";
    for (int index = 0; index < params_.driverCount; ++index) {
        if (index > 0) output << ',';
        const Driver& driver = drivers_[index];
        output << "{\"id\":" << driver.id
               << ",\"x\":" << driver.x
               << ",\"y\":" << driver.y
               << ",\"targetX\":" << driver.targetX
               << ",\"targetY\":" << driver.targetY
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
    idleWander_ = false;
    for (int index = 0; index < kMaxDriverCount; ++index) {
        drivers_[index] = Driver{};
        activeOrders_[index] = Order{};
    }
}

void Simulator::testSetAutoGenerate(bool enabled) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    autoGenerate_ = enabled;
}

void Simulator::testSetIdleWander(bool enabled) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    idleWander_ = enabled;
}

void Simulator::testSetTrafficFactor(int cellX, int cellY, double factor) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    grid_.cell(cellX, cellY).trafficFactor = factor;
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

Order Simulator::testActiveOrder(int slot) const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return activeOrders_[slot];
}

std::size_t Simulator::testQueueSize() const { return orders_.size(); }
std::uint64_t Simulator::testGenerated() const { return generated_.load(); }
std::uint64_t Simulator::testMatched() const { std::lock_guard<std::mutex> lock(stateMutex_); return matched_; }
std::uint64_t Simulator::testCancelled() const { std::lock_guard<std::mutex> lock(stateMutex_); return cancelled_; }
std::uint64_t Simulator::testCompleted() const { std::lock_guard<std::mutex> lock(stateMutex_); return completed_; }
#endif

}  // namespace taxi
