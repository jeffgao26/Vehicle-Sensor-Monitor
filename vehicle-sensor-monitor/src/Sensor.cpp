#include "Sensor.h"
#include <algorithm>
#include <utility>

namespace monitor {
void TelemetryStore::publish(const Telemetry& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& entry = entries_[static_cast<std::size_t>(message.id)];
    if (entry.messages == 0) entry.first = message.timestamp;
    else {
        const auto gap = message.timestamp - entry.latest.timestamp;
        entry.maxGap = std::max(entry.maxGap, gap);
        const auto interval = configs[static_cast<std::size_t>(message.id)].interval;
        entry.timelyStreak = gap >= Clock::duration::zero() && gap < interval * timelyIntervals_
            ? entry.timelyStreak + 1 : 0;
    }
    entry.latest = message;
    ++entry.messages;
}
std::array<Snapshot, sensorCount> TelemetryStore::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}
void StopSignal::request() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_.store(true);
    }
    condition_.notify_all();
}
bool StopSignal::waitUntil(Time deadline) {
    std::unique_lock<std::mutex> lock(mutex_);
    return condition_.wait_until(lock, deadline, [this] { return stopped_.load(); });
}
Sensor::Sensor(SensorConfig config, TelemetryStore& store, StopSignal& stop,
               Time startup, FaultSchedule faults)
    : Sensor(config, [&store](const Telemetry& message) { store.publish(message); },
             stop, startup, faults) {}
Sensor::Sensor(SensorConfig config, Publisher publish, StopSignal& stop,
               Time startup, FaultSchedule faults)
    : config_(config), publish_(std::move(publish)), stop_(stop), startup_(startup), faults_(faults),
      thread_(&Sensor::run, this) {}
Sensor::~Sensor() {
    stop_.request();
    if (thread_.joinable()) thread_.join();
}
void Sensor::run() {
    std::uint64_t sequence = 0;
    auto next = startup_ + config_.interval;
    while (!stop_.waitUntil(next)) {
        const auto now = Clock::now();
        const auto elapsed = now - startup_;
        if (faults_.enabled && config_.id == SensorId::Lidar &&
            elapsed >= faults_.lidarBegin && elapsed < faults_.lidarEnd) {
            next = startup_ + faults_.lidarEnd;
            continue;
        }
        const bool delayed = faults_.enabled && config_.id == SensorId::Camera &&
            elapsed >= faults_.cameraBegin && elapsed < faults_.cameraEnd;
        publish_({config_.id, ++sequence, now,
                        delayed ? SensorStatus::LatencyInjected : SensorStatus::Nominal});
        next = now + (delayed ? faults_.cameraInterval : config_.interval);
        // Resume normal cadence at the end of the configured latency window.
        if (delayed) next = std::min(next, startup_ + faults_.cameraEnd);
    }
}
} // namespace monitor
