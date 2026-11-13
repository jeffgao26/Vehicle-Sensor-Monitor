#pragma once
#include "Sensor.h"
#include <functional>
#include <optional>

namespace monitor {
enum class Health { Starting, Healthy, Degraded, Offline };
const char* healthName(Health health);
struct Thresholds {
    unsigned warningIntervals = 2;
    unsigned failureIntervals = 5;
    unsigned recoveryUpdates = 3;
};
struct Transition {
    Health from;
    Health to;
    bool recovered;
    Time timestamp;
    // Time since the first expected but missing update, for OFFLINE decisions.
    Clock::duration detectionLatency{};
};
class HealthTracker {
public:
    HealthTracker(Milliseconds interval, Time startup, Thresholds thresholds = {});
    std::optional<Transition> evaluate(const Snapshot& snapshot, Time now);
    Health state() const { return state_; }
private:
    Milliseconds interval_;
    Time startup_;
    Thresholds thresholds_;
    Health state_ = Health::Starting;
    std::uint64_t recoveryBaseline_ = 0;
};
struct HealthMetrics {
    std::uint64_t transitions = 0;
    std::uint64_t offlineDetections = 0;
    Clock::duration totalDetectionLatency{};
};
class Watchdog {
public:
    using Reporter = std::function<void(SensorId, const Transition&)>;
    Watchdog(TelemetryStore& store, StopSignal& stop, Time startup,
             Thresholds thresholds = {}, Reporter reporter = {});
    ~Watchdog();
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;
    // Call after join(): worker-owned counters need no locks.
    const std::array<HealthMetrics, sensorCount>& metrics() const { return metrics_; }
    void join();
private:
    void run();
    TelemetryStore& store_;
    StopSignal& stop_;
    std::array<HealthTracker, sensorCount> trackers_;
    Reporter reporter_;
    std::array<HealthMetrics, sensorCount> metrics_{};
    std::thread thread_;
};
} // namespace monitor
