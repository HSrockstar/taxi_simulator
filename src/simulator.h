#pragma once

#include "manual_structures.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <thread>

namespace taxi {

struct StreamToken {
    std::uint64_t tick = 0;
    std::uint64_t epoch = 0;
    std::uint64_t logSequence = 0;
    bool paused = false;
};

inline bool operator!=(const StreamToken& left, const StreamToken& right) {
    return left.tick != right.tick || left.epoch != right.epoch ||
           left.logSequence != right.logSequence || left.paused != right.paused;
}

class Simulator {
public:
    explicit Simulator(std::uint32_t seed = 20260808U);
    ~Simulator();

    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;

    void start();
    void stop();
    void pause();
    void resume();
    void requestReset();

    bool paused() const;
    std::string snapshotJson() const;
    std::string paramsJson() const;
    StreamToken streamToken() const;
    bool updateParams(const SimulatorParams& params);

#ifdef TAXI_TESTING
    void testClearState();
    void testSetAutoGenerate(bool enabled);
    void testAddDriver(int slot, int id, int x, int y, double rating);
    void testPushOrder(std::uint64_t id, int x, int y, std::uint64_t createdTick = 0);
    void testTick();
    Driver testDriver(int slot) const;
    Order testActiveOrder(int slot) const;
    std::size_t testQueueSize() const;
    std::uint64_t testGenerated() const;
    std::uint64_t testMatched() const;
    std::uint64_t testCancelled() const;
    std::uint64_t testCompleted() const;
#endif

private:
    void schedulerLoop();
    void executeTick();
    void resetStateLocked();
    void processDriverTransitionsLocked(std::uint64_t currentTick);
    void stepTripDriverLocked(int slot, Driver& driver, std::uint64_t currentTick);
    void generateOrderBatchLocked(std::uint64_t currentTick);
    void processOrdersLocked(std::uint64_t currentTick);
    void rebalanceLocked(std::uint64_t currentTick);
    void applyFleetChangeLocked(int previousCount, int newCount);
    void appendParamsJson(std::ostringstream& output) const;
    Driver* findBestDriverLocked(const Order& order, double& distance, double& score);
    Driver* findDonorDriverLocked(int targetCellX, int targetCellY);
    static const char* driverStateName(DriverState state);

    std::uint32_t seed_;
    GridIndex grid_;
    OrderQueue orders_;
    Driver* drivers_ = nullptr;
    // 撮合成功后的在途订单，按司机槽位索引，行程结束时在此流转为已完成
    Order* activeOrders_ = nullptr;
    SimulatorParams params_;
    LogRingBuffer logs_;

    mutable std::mutex stateMutex_;
    std::mutex controlMutex_;
    std::condition_variable controlCondition_;
    std::thread schedulerThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> resetRequested_{false};
    std::atomic<std::uint64_t> tick_{0};
    std::atomic<std::uint64_t> resetEpoch_{0};
    std::atomic<std::uint64_t> nextOrderId_{1};
    std::atomic<std::uint64_t> generated_{0};

    std::mt19937 schedulerRandom_;
    bool autoGenerate_ = true;
    std::uint64_t matched_ = 0;
    std::uint64_t cancelled_ = 0;
    std::uint64_t completed_ = 0;
    std::uint64_t matchAttempts_ = 0;
    std::uint64_t totalMatchMicros_ = 0;
};

}  // namespace taxi
