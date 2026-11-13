#include "Watchdog.h"
#include <algorithm>
#include <utility>
#include <stdexcept>

namespace monitor {
const char* healthName(Health health) {
    switch (health) {
    case Health::Starting: return "STARTING";
    case Health::Healthy: return "HEALTHY";
    case Health::Degraded: return "DEGRADED";
    case Health::Offline: return "OFFLINE";
    }
    return "UNKNOWN";
}
HealthTracker::HealthTracker(Milliseconds interval, Time startup, Thresholds thresholds)
    : interval_(interval), startup_(startup), thresholds_(thresholds) {
    if (interval.count() <= 0 || thresholds.warningIntervals < 1 ||
        thresholds.failureIntervals <= thresholds.warningIntervals || thresholds.recoveryUpdates == 0)
        throw std::invalid_argument("Require interval > 0, 1 <= warn < fail, and recovery updates > 0");
}
std::optional<Transition> HealthTracker::evaluate(const Snapshot& snapshot, Time now) {
    const auto reference = snapshot.messages ? snapshot.latest.timestamp : startup_;
    const auto age = now - reference;
    Health next;
    if (age >= interval_ * thresholds_.failureIntervals) next = Health::Offline;
    else if (age >= interval_ * thresholds_.warningIntervals) next = Health::Degraded;
    else next = snapshot.messages ? Health::Healthy : Health::Starting;
    const bool unhealthy = state_ == Health::Degraded || state_ == Health::Offline;
    if (next == Health::Degraded || next == Health::Offline) {
        recoveryBaseline_ = snapshot.messages;
    } else if (unhealthy && next == Health::Healthy) {
        // Store-side streaks account for updates faster than watchdog polling.
        const auto required = thresholds_.recoveryUpdates;
        if (snapshot.messages - recoveryBaseline_ < required || snapshot.timelyStreak < required)
            next = Health::Degraded; // Traffic resumed, but stable recovery is still pending.
    }
    if (next == state_) return std::nullopt;
    Transition transition{state_, next,
        next == Health::Healthy && (state_ == Health::Degraded || state_ == Health::Offline),
        now, next == Health::Offline ? std::max(Clock::duration::zero(), age - interval_)
                                    : Clock::duration::zero()};
    state_ = next;
    return transition;
}
Watchdog::Watchdog(TelemetryStore& store, StopSignal& stop, Time startup,
                   Thresholds thresholds, Reporter reporter)
    : store_(store), stop_(stop),
      trackers_{{HealthTracker(configs[0].interval, startup, thresholds),
                 HealthTracker(configs[1].interval, startup, thresholds),
                 HealthTracker(configs[2].interval, startup, thresholds)}},
      reporter_(std::move(reporter)), thread_(&Watchdog::run, this) {}
Watchdog::~Watchdog() { stop_.request(); join(); }
void Watchdog::join() { if (thread_.joinable()) thread_.join(); }
void Watchdog::run() {
    while (!stop_.requested()) {
        const auto snapshots = store_.snapshot();
        const auto now = Clock::now();
        for (std::size_t i = 0; i < sensorCount; ++i) {
            if (auto transition = trackers_[i].evaluate(snapshots[i], now)) {
                auto& metric = metrics_[i];
                ++metric.transitions;
                if (transition->to == Health::Offline) {
                    ++metric.offlineDetections;
                    metric.totalDetectionLatency += transition->detectionLatency;
                }
                if (reporter_) reporter_(configs[i].id, *transition);
            }
        }
        if (stop_.waitUntil(now + Milliseconds{50})) break;
    }
}
} // namespace monitor
