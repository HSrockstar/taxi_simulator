// 参数扫描 harness（v2）：适配"路况感知 ETA 撮合"后的引擎。
// 以 TAXI_TESTING 模式同步驱动模拟引擎，对一组参数组合各跑若干模拟秒，
// 输出逐秒时间序列与汇总指标；从日志流精确统计调度事件、接驾 ETA 与路况系数分布。
// 用法: param_sweep.exe [seed] [sim_seconds]
#include "simulator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

using namespace taxi;

namespace {

struct Combo {
    const char* name;
    int driverCount;
    int orderRateMin;
    int orderRateMax;
    int matchRadius;
    int rebalanceRadius;
    int imbalanceThreshold;
    int orderTimeout;
};

// 题目约束：司机数 100、订单率 5-10 固定。新引擎语义（接驾=ETA、调度严格超过阈值）下的复测集合。
std::vector<Combo> buildCombos() {
    std::vector<Combo> combos;
    combos.push_back(Combo{"r6_d30_t1_w20", 100, 5, 10, 6, 30, 1, 20});   // 主推
    combos.push_back(Combo{"r3_d30_t1_w15", 100, 5, 10, 3, 30, 1, 15});   // 备选
    combos.push_back(Combo{"r3_d30_t1_w20", 100, 5, 10, 3, 30, 1, 20});
    combos.push_back(Combo{"r6_d30_t1_w15", 100, 5, 10, 6, 30, 1, 15});
    combos.push_back(Combo{"r6_d30_t1_w25", 100, 5, 10, 6, 30, 1, 25});   // 超时对照
    combos.push_back(Combo{"r6_d30_t2_w20", 100, 5, 10, 6, 30, 2, 20});   // 阈值对照
    combos.push_back(Combo{"r6_d20_t1_w20", 100, 5, 10, 6, 20, 1, 20});   // 调度半径对照
    combos.push_back(Combo{"ctrl_d1_t5_w20", 100, 5, 10, 5, 1, 5, 20});   // 机制弱化对照
    combos.push_back(Combo{"pure_default", 100, 5, 10, 3, 10, 2, 10});   // 默认参数
    combos.push_back(Combo{"r8_d30_t1_w20", 100, 5, 10, 8, 30, 1, 20});    // 半径悬崖验证
    combos.push_back(Combo{"r10_d30_t1_w20", 100, 5, 10, 10, 30, 1, 20});
    combos.push_back(Combo{"r12_d30_t1_w20", 100, 5, 10, 12, 30, 1, 20});
    return combos;
}

struct Sample {
    int t = 0;
    std::uint64_t queue = 0;
    std::uint64_t generated = 0, matched = 0, cancelled = 0, completed = 0;
    int idle = 0, enroute = 0, ontrip = 0, rebal = 0;
    int alertCells = 0;
    int maxPendingCell = 0;
    int trafficCells15 = 0;
    int trafficCells25 = 0;
    double avgTrafficFactor = 1.0;
};

// 从 snapshotJson 中提取名为 key 的数值数组（"pending":[…]/"idle":[…]/"traffic":[…]）
std::vector<double> extractArray(const std::string& json, const std::string& key) {
    std::vector<double> values;
    const std::size_t keyPos = json.find("\"" + key + "\":[");
    if (keyPos == std::string::npos) {
        return values;
    }
    std::size_t pos = keyPos + key.size() + 4;
    while (pos < json.size() && json[pos] != ']') {
        if (json[pos] == ',' || json[pos] == '[') {
            ++pos;
            continue;
        }
        const std::size_t start = pos;
        while (pos < json.size() && json[pos] != ',' && json[pos] != ']') {
            ++pos;
        }
        values.push_back(std::strtod(json.c_str() + start, nullptr));
    }
    return values;
}

// 提取日志 message 字段的 JSON 字符串值（消息不含引号/反斜杠，直接找首尾引号）
// 返回 false 表示没有更多消息；nextPos 输出该消息结束位置，供调用方推进游标
bool extractMessage(const std::string& json, std::size_t from, std::string* out,
                    std::size_t* nextPos) {
    const std::size_t keyPos = json.find("\"message\":\"", from);
    if (keyPos == std::string::npos) {
        return false;
    }
    const std::size_t start = keyPos + 11;
    const std::size_t end = json.find('"', start);
    if (end == std::string::npos) {
        return false;
    }
    out->assign(json, start, end - start);
    *nextPos = end + 1;
    return true;
}

double parseAfter(const std::string& text, const std::string& marker) {
    const std::size_t pos = text.find(marker);
    if (pos == std::string::npos) {
        return 0.0;
    }
    return std::strtod(text.c_str() + pos + marker.size(), nullptr);
}

int percentile(std::vector<double> values, double fraction) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(fraction * (values.size() - 1));
    return static_cast<int>(values[index]);
}

int percentileInt(std::vector<int> values, double fraction) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(fraction * (values.size() - 1));
    return values[index];
}

}  // namespace

int main(int argc, char** argv) {
    const std::uint32_t seed = argc > 1 ? static_cast<std::uint32_t>(std::atoi(argv[1])) : 20260808U;
    const int simSeconds = argc > 2 ? std::atoi(argv[2]) : 600;
    constexpr int kWarmupSeconds = 60;
    constexpr int kTps = kTicksPerSecond;

    std::ofstream series("sweep_timeseries.csv");
    series << "seed,combo,t,queue,generated,matched,cancelled,completed,"
              "idle,enroute,ontrip,rebal,alertCells,maxPendingCell,trafficCells15,trafficCells25\n";

    std::ofstream summary("sweep_summary.csv");
    summary << "seed,combo,drivers,orderRate,matchR,rebalR,thresh,timeout,"
               "ss_success_pct,ss_cancel_per_min,ss_completed_per_min,ss_match_per_sec,est_wait_s,"
               "avg_queue,p95_queue,max_queue,avg_idle_frac,avg_enroute,avg_ontrip,avg_rebal,"
               "rebal_events_per_min,avg_alert_cells,p90_alert_cells,max_alert_cells,max_pending_obs,"
               "eta_mean_s,eta_p90_s,traffic_share_pct,avg_traffic_factor,avg_grid_traffic,"
               "avg_traffic_cells15,avg_traffic_cells25,generated_total,cancelled_total,completed_total\n";
    summary << std::fixed;

    const std::vector<Combo> combos = buildCombos();
    for (const Combo& combo : combos) {
        SimulatorParams params;
        params.driverCount = combo.driverCount;
        params.orderRateMin = combo.orderRateMin;
        params.orderRateMax = combo.orderRateMax;
        params.matchRadius = combo.matchRadius;
        params.rebalanceRadius = combo.rebalanceRadius;
        params.imbalanceThreshold = combo.imbalanceThreshold;
        params.orderTimeout = combo.orderTimeout;

        // 每个 combo 都用全新 Simulator：初始司机布局由种子决定，保证组合间可比
        Simulator simulator(seed);
        simulator.updateParams(params);

        std::vector<Sample> samples;
        samples.reserve(simSeconds);
        std::vector<double> etaSamples;
        std::vector<double> trafficFactorSamples;
        std::uint64_t trafficAwareMatches = 0;
        std::uint64_t rebalLogCount = 0;
        std::uint64_t lastLogSequence = 0;

        Sample current;
        for (int tick = 1; tick <= simSeconds * kTps; ++tick) {
            simulator.testTick();

            // 每 5 tick 解析一次日志与路况（日志环形缓冲 50 条，0.5 秒间隔不会漏）
            if (tick % 5 == 0) {
                const std::string snapshot = simulator.snapshotJson();
                std::size_t cursor = snapshot.find("\"logs\":");
                if (cursor != std::string::npos) {
                    std::string message;
                    std::size_t nextPos = cursor;
                    while (extractMessage(snapshot, nextPos, &message, &nextPos)) {
                        const std::size_t seqPos = snapshot.rfind("\"sequence\":", nextPos);
                        const std::uint64_t sequence = seqPos == std::string::npos
                            ? 0 : std::strtoull(snapshot.c_str() + seqPos + 11, nullptr, 10);
                        if (sequence > lastLogSequence) {
                            lastLogSequence = sequence;
                            if (message.find("[派单成功]") == 0) {
                                const double eta = parseAfter(message, "预计接驾:");
                                const double factor = parseAfter(message, "平均路况系数:");
                                etaSamples.push_back(eta);
                                trafficFactorSamples.push_back(factor);
                                if (factor > 1.15) {
                                    ++trafficAwareMatches;
                                }
                            } else if (message.find("[运力调度]") == 0) {
                                ++rebalLogCount;
                            }
                        }
                    }
                }
                if (tick % 10 == 0) {
                    const std::vector<double> pending = extractArray(snapshot, "pending");
                    const std::vector<double> idle = extractArray(snapshot, "idle");
                    const std::vector<double> traffic = extractArray(snapshot, "traffic");
                    current.t = tick / kTps;
                    current.queue = simulator.testQueueSize();
                    current.generated = simulator.testGenerated();
                    current.matched = simulator.testMatched();
                    current.cancelled = simulator.testCancelled();
                    current.completed = simulator.testCompleted();
                    current.idle = current.enroute = current.ontrip = current.rebal = 0;
                    for (int slot = 0; slot < params.driverCount; ++slot) {
                        const Driver& driver = simulator.testDriver(slot);
                        switch (driver.state) {
                            case DriverState::Idle: ++current.idle; break;
                            case DriverState::EnRoute: ++current.enroute; break;
                            case DriverState::OnTrip: ++current.ontrip; break;
                            case DriverState::Rebalancing: ++current.rebal; break;
                        }
                    }
                    // 与前端热力图同判据：pending-idle 严格超过失衡阈值的格子为警报格
                    current.alertCells = 0;
                    current.maxPendingCell = 0;
                    current.trafficCells15 = 0;
                    current.trafficCells25 = 0;
                    double trafficSum = 0;
                    for (std::size_t index = 0; index < pending.size(); ++index) {
                        const int gap = static_cast<int>(pending[index] - idle[index]);
                        if (gap > params.imbalanceThreshold) {
                            ++current.alertCells;
                        }
                        current.maxPendingCell = std::max(current.maxPendingCell,
                                                          static_cast<int>(pending[index]));
                        trafficSum += traffic[index];
                        if (traffic[index] >= 1.5) ++current.trafficCells15;
                        if (traffic[index] >= 2.5) ++current.trafficCells25;
                    }
                    current.avgTrafficFactor = trafficSum / static_cast<double>(pending.size());
                    samples.push_back(current);
                    series << seed << ',' << combo.name << ',' << current.t << ','
                           << current.queue << ',' << current.generated << ',' << current.matched << ','
                           << current.cancelled << ',' << current.completed << ','
                           << current.idle << ',' << current.enroute << ',' << current.ontrip << ','
                           << current.rebal << ',' << current.alertCells << ',' << current.maxPendingCell
                           << ',' << current.trafficCells15 << ',' << current.trafficCells25 << '\n';
                }
            }
        }

        const Sample& end = samples.back();
        const Sample& warm = samples[std::min(kWarmupSeconds, simSeconds - 1)];
        const double windowSeconds = static_cast<double>(simSeconds - kWarmupSeconds);

        // 稳态窗口 [60s, end] 的增量指标
        const std::uint64_t winMatched = end.matched - warm.matched;
        const std::uint64_t winCancelled = end.cancelled - warm.cancelled;
        const std::uint64_t winCompleted = end.completed - warm.completed;
        const double finalized = static_cast<double>(winMatched + winCancelled);
        const double ssSuccess = finalized > 0 ? winMatched * 100.0 / finalized : 0.0;
        const double ssMatchPerSec = static_cast<double>(winMatched) / windowSeconds;
        const double ssCancelPerMin = static_cast<double>(winCancelled) * 60.0 / windowSeconds;
        const double ssCompletedPerMin = static_cast<double>(winCompleted) * 60.0 / windowSeconds;

        std::vector<int> queueSamples;
        std::vector<int> alertSamples;
        double sumQueue = 0, sumIdle = 0, sumEnroute = 0, sumOntrip = 0, sumRebal = 0, sumAlert = 0;
        double sumTraffic15 = 0, sumTraffic25 = 0, sumAvgTraffic = 0;
        int maxQueue = 0, maxAlert = 0, maxPendingObs = 0;
        for (const Sample& sample : samples) {
            if (sample.t < kWarmupSeconds) {
                continue;
            }
            queueSamples.push_back(static_cast<int>(sample.queue));
            alertSamples.push_back(sample.alertCells);
            sumQueue += static_cast<double>(sample.queue);
            sumIdle += static_cast<double>(sample.idle);
            sumEnroute += static_cast<double>(sample.enroute);
            sumOntrip += static_cast<double>(sample.ontrip);
            sumRebal += static_cast<double>(sample.rebal);
            sumAlert += static_cast<double>(sample.alertCells);
            sumTraffic15 += static_cast<double>(sample.trafficCells15);
            sumTraffic25 += static_cast<double>(sample.trafficCells25);
            sumAvgTraffic += sample.avgTrafficFactor;
            maxQueue = std::max(maxQueue, static_cast<int>(sample.queue));
            maxAlert = std::max(maxAlert, sample.alertCells);
            maxPendingObs = std::max(maxPendingObs, sample.maxPendingCell);
        }
        const double count = static_cast<double>(queueSamples.size());
        const double etaMean = etaSamples.empty() ? 0.0
            : std::accumulate(etaSamples.begin(), etaSamples.end(), 0.0) / etaSamples.size();
        const double avgTrafficFactor = trafficFactorSamples.empty() ? 0.0
            : std::accumulate(trafficFactorSamples.begin(), trafficFactorSamples.end(), 0.0)
              / trafficFactorSamples.size();
        const double trafficShare = etaSamples.empty() ? 0.0
            : trafficAwareMatches * 100.0 / etaSamples.size();

        summary << seed << ',' << combo.name << ',' << combo.driverCount << ','
                << combo.orderRateMin << '-' << combo.orderRateMax << ','
                << combo.matchRadius << ',' << combo.rebalanceRadius << ','
                << combo.imbalanceThreshold << ',' << combo.orderTimeout << ','
                << ssSuccess << ',' << ssCancelPerMin << ',' << ssCompletedPerMin << ','
                << ssMatchPerSec << ',' << (ssMatchPerSec > 0 ? sumQueue / count / ssMatchPerSec : 0.0) << ','
                << sumQueue / count << ',' << percentileInt(queueSamples, 0.95) << ',' << maxQueue << ','
                << sumIdle / count / combo.driverCount << ','
                << sumEnroute / count << ',' << sumOntrip / count << ',' << sumRebal / count << ','
                << rebalLogCount * 60.0 / simSeconds << ','
                << sumAlert / count << ',' << percentileInt(alertSamples, 0.90) << ',' << maxAlert << ','
                << maxPendingObs << ','
                << etaMean << ',' << percentile(etaSamples, 0.90) << ','
                << trafficShare << ',' << avgTrafficFactor << ',' << sumAvgTraffic / count << ','
                << sumTraffic15 / count << ',' << sumTraffic25 / count << ','
                << end.generated << ',' << end.cancelled << ',' << end.completed << '\n';

        std::cout << combo.name << " done" << std::endl;
    }

    summary.close();
    series.close();
    std::cout << "sweep complete: seed=" << seed << " simSeconds=" << simSeconds << std::endl;
    return 0;
}
