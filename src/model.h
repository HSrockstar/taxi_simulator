#pragma once

#include <chrono>
#include <cstdint>

namespace taxi {

constexpr int kMapSize = 1000;
constexpr int kGridSide = 100;
constexpr int kCellSize = kMapSize / kGridSide;
constexpr int kGridCount = kGridSide * kGridSide;
constexpr int kDriverCount = 100;
constexpr int kMaxDriverCount = 500;
constexpr int kDirectMatchRadius = 3;
constexpr int kRebalanceRadius = 10;

enum class DriverState {
    Idle,
    Serving,
    Rebalancing
};

enum class OrderState {
    Waiting,
    Matched,
    Completed,
    Cancelled
};

struct Driver {
    int id = 0;
    int x = 0;
    int y = 0;
    double rating = 0.0;
    DriverState state = DriverState::Idle;
    Driver* previous = nullptr;
    Driver* next = nullptr;
    int gridIndex = -1;
    std::uint64_t readyTick = 0;
    int targetX = 0;
    int targetY = 0;
    std::uint64_t activeOrderId = 0;
};

struct Order {
    std::uint64_t id = 0;
    int x = 0;
    int y = 0;
    std::uint64_t createdTick = 0;
    OrderState state = OrderState::Waiting;
    bool countedInGrid = false;
};

struct MatchCandidate {
    Driver* driver = nullptr;
    double score = 0.0;
    double distance = 0.0;
};

struct SimulatorParams {
    int driverCount = kDriverCount;
    int orderRateMin = 5;
    int orderRateMax = 10;
    int matchRadius = kDirectMatchRadius;
    int rebalanceRadius = kRebalanceRadius;
    int imbalanceThreshold = 2;
    int orderTimeout = 10;

    static bool valid(const SimulatorParams& params) {
        return params.driverCount >= 10 && params.driverCount <= kMaxDriverCount
            && params.orderRateMin >= 0 && params.orderRateMin <= params.orderRateMax
            && params.orderRateMax <= 50
            && params.matchRadius >= 1 && params.matchRadius <= 20
            && params.rebalanceRadius >= 1 && params.rebalanceRadius <= 30
            && params.imbalanceThreshold >= 1 && params.imbalanceThreshold <= 20
            && params.orderTimeout >= 1 && params.orderTimeout <= 60;
    }
};

inline bool candidateLess(const MatchCandidate& left, const MatchCandidate& right) {
    constexpr double kEpsilon = 1e-9;
    if (left.score < right.score - kEpsilon) {
        return true;
    }
    if (left.score > right.score + kEpsilon) {
        return false;
    }
    if (left.distance < right.distance - kEpsilon) {
        return true;
    }
    if (left.distance > right.distance + kEpsilon) {
        return false;
    }
    if (left.driver->rating > right.driver->rating + kEpsilon) {
        return true;
    }
    if (left.driver->rating < right.driver->rating - kEpsilon) {
        return false;
    }
    return left.driver->id < right.driver->id;
}

}  // namespace taxi
