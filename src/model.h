#pragma once

#include <chrono>
#include <cstdint>

namespace taxi {

// 城市与网格规模：1000×1000 米的城市划成 100×100 格，每格 10×10 米
constexpr int kMapSize = 1000;
constexpr int kGridSide = 100;
constexpr int kCellSize = kMapSize / kGridSide;
constexpr int kGridCount = kGridSide * kGridSide;
// 司机规模：默认 100 辆是题目设定，槽位数组按上限预分配，扩编不再重新分配内存
constexpr int kDriverCount = 100;
constexpr int kMaxDriverCount = 500;
// 撮合/调度半径的默认值，运行时可经看板或 /api/params 热更新
constexpr int kDirectMatchRadius = 3;
constexpr int kRebalanceRadius = 10;
// 引擎节拍：调度线程每 1/kTicksPerSecond 秒推进一个 tick，
// 所有以 tick 计的时长（接客、行程、调度、超时）按该值与秒互转
constexpr int kTicksPerSecond = 10;

// 司机四态：空闲 → 接客途中 → 行程中 → 空闲；被调度时走调度中，到达热点回空闲
enum class DriverState {
    Idle,         // 在网格索引中，可被撮合或调度
    EnRoute,      // 接客途中，已从空间索引摘除，不会被再次选中
    OnTrip,       // 载客行程中
    Rebalancing   // 被调度派往热点格，在途
};

// 订单四态：等待 → 已匹配 → 已完成；等待超过时限未匹配则取消
enum class OrderState {
    Waiting,     // 在队列中排队
    Matched,     // 已匹配司机，接客或行程中
    Completed,   // 到达目的地
    Cancelled    // 超时取消，或司机下线导致的服务中断
};

struct Driver {
    int id = 0;
    int x = 0;
    int y = 0;
    double rating = 0.0;              // 服务评分 3.5–5.0，参与撮合代价
    DriverState state = DriverState::Idle;
    Driver* previous = nullptr;       // 所在格链表的侵入式双向指针
    Driver* next = nullptr;
    int gridIndex = -1;               // 所在格扁平下标，-1 表示不在任何格中
    std::uint64_t readyTick = 0;      // 当前在途状态预计结束的 tick
    int targetX = 0;                  // 在途目标点（上车点/目的地/热点落点）
    int targetY = 0;
    std::uint64_t activeOrderId = 0;  // 关联的在途订单号，0 表示无
};

struct Order {
    std::uint64_t id = 0;
    int x = 0;
    int y = 0;
    std::uint64_t createdTick = 0;
    OrderState state = OrderState::Waiting;
    // 是否已计入所在格 pendingCount：撮合失败重新入队时该标记保持，
    // 同一订单不会被重复计入等待数
    bool countedInGrid = false;
};

// 撮合候选：司机连同代价分数入堆；distance/etaSeconds 另存一份供日志与同分决胜用
struct MatchCandidate {
    Driver* driver = nullptr;
    double score = 0.0;
    double distance = 0.0;
    double etaSeconds = 0.0;
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

// 撮合决胜链：分数小者优先，同分比距离近、再比评分高、最后比编号小。
// 每级比较带 1e-9 容差，浮点累加路径不同带来的噪声不至于翻结果；
// 四级下来任何两个候选必分胜负，堆的弹出结果与插入顺序无关，同种子才可复现。
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
