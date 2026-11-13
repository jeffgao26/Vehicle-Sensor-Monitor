#include "Watchdog.h"
#include <iostream>
#include <stdexcept>

using namespace monitor;
namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
Snapshot received(Time timestamp, std::uint64_t sequence = 1) {
    Snapshot snapshot;
    snapshot.messages = sequence;
    snapshot.timelyStreak = sequence - 1;
    snapshot.latest = {SensorId::Lidar, sequence, timestamp, SensorStatus::Nominal};
    return snapshot;
}
void testTransitions() {
    const Time start{};
    HealthTracker tracker(Milliseconds{100}, start);
    auto snapshot = received(start + Milliseconds{100});
    auto transition = tracker.evaluate(snapshot, start + Milliseconds{100});
    check(transition && transition->to == Health::Healthy, "initial healthy");
    check(!tracker.evaluate(snapshot, start + Milliseconds{299}), "before warning boundary");
    transition = tracker.evaluate(snapshot, start + Milliseconds{300});
    check(transition && transition->from == Health::Healthy && transition->to == Health::Degraded,
          "healthy to degraded");
    check(!tracker.evaluate(snapshot, start + Milliseconds{599}), "before failure boundary");
    transition = tracker.evaluate(snapshot, start + Milliseconds{600});
    check(transition && transition->to == Health::Offline, "degraded to offline");
    check(transition->detectionLatency == Milliseconds{400}, "offline latency definition");
    check(!tracker.evaluate(snapshot, start + Milliseconds{650}), "no duplicate transitions");
    snapshot = received(start + Milliseconds{700}, 2);
    snapshot.timelyStreak = 0;
    transition = tracker.evaluate(snapshot, start + Milliseconds{700});
    check(transition && transition->to == Health::Degraded && !transition->recovered,
          "first resumed message is pending recovery");
    for (unsigned i = 1; i <= 3; ++i) {
        snapshot = received(start + Milliseconds{700 + 100 * i}, 2 + i);
        snapshot.timelyStreak = i;
        transition = tracker.evaluate(snapshot, snapshot.latest.timestamp);
        if (i < 3) {
            check(!transition, "recovery requires three timely gaps");
            check(!tracker.evaluate(snapshot, snapshot.latest.timestamp + Milliseconds{10}),
                  "repeated polling does not advance recovery");
        } else check(transition && transition->recovered, "offline recovery after stable traffic");
    }
    tracker.evaluate(snapshot, start + Milliseconds{1200});
    // Three updates can occur between watchdog checks; the store preserves the streak.
    snapshot = received(start + Milliseconds{1500}, 9);
    snapshot.timelyStreak = 3;
    transition = tracker.evaluate(snapshot, snapshot.latest.timestamp);
    check(transition && transition->recovered, "degraded recovery with skipped polling");
}
void testStartupAndConfiguration() {
    const Time start{};
    HealthTracker tracker(Milliseconds{100}, start);
    Snapshot empty;
    check(!tracker.evaluate(empty, start + Milliseconds{99}), "startup grace before first message");
    check(!tracker.evaluate(empty, start + Milliseconds{199}), "startup warning grace");
    check(tracker.evaluate(empty, start + Milliseconds{200})->to == Health::Degraded, "startup warning");
    check(tracker.evaluate(empty, start + Milliseconds{500})->to == Health::Offline, "never published offline");
    HealthTracker custom(Milliseconds{10}, start, {3, 7});
    check(!custom.evaluate(empty, start + Milliseconds{29}), "custom grace");
    check(custom.evaluate(empty, start + Milliseconds{30})->to == Health::Degraded, "custom warning");
    check(custom.evaluate(empty, start + Milliseconds{70})->to == Health::Offline, "custom failure");
    bool rejected = false;
    try { HealthTracker invalid(Milliseconds{100}, start, {5, 2}); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "invalid thresholds rejected");
    rejected = false;
    try { HealthTracker invalid(Milliseconds{100}, start, {2, 5, 0}); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "zero recovery requirement rejected");
}
void testCameraHysteresis() {
    const Time start{};
    TelemetryStore store;
    HealthTracker tracker(Milliseconds{33}, start);
    auto publish = [&](int milliseconds, std::uint64_t sequence) {
        const auto now = start + Milliseconds{milliseconds};
        store.publish({SensorId::Camera, sequence, now, SensorStatus::Nominal});
        return tracker.evaluate(store.snapshot()[2], now);
    };
    publish(33, 1);
    auto transition = tracker.evaluate(store.snapshot()[2], start + Milliseconds{100});
    check(transition && transition->to == Health::Degraded, "camera degraded");
    for (unsigned i = 1; i <= 5; ++i) {
        check(!publish(33 + 120 * i, i + 1), "slow messages cannot recover");
        check(tracker.state() == Health::Degraded, "camera remains degraded during slow cadence");
    }
    check(!publish(666, 7), "first timely gap");
    check(!publish(699, 8), "second timely gap");
    transition = publish(732, 9);
    check(transition && transition->recovered, "third timely gap recovers");
}
void testStore() {
    TelemetryStore store;
    const Time start{};
    store.publish({SensorId::Radar, 1, start, SensorStatus::Nominal});
    store.publish({SensorId::Radar, 2, start + Milliseconds{50}, SensorStatus::Nominal});
    store.publish({SensorId::Radar, 3, start + Milliseconds{180}, SensorStatus::LatencyInjected});
    const auto snapshot = store.snapshot();
    check(snapshot[1].messages == 3 && snapshot[1].latest.sequence == 3, "store counters");
    check(snapshot[1].maxGap == Milliseconds{130}, "maximum gap");
    check(snapshot[1].first == start, "first timestamp preserved");
    check(snapshot[0].messages == 0, "independent sensors");
}
void testShutdown() {
    TelemetryStore store;
    StopSignal stop;
    const auto start = Clock::now();
    {
        Watchdog watchdog(store, stop, start);
        Sensor lidar(configs[0], store, stop, start);
        Sensor radar(configs[1], store, stop, start);
        Sensor camera(configs[2], store, stop, start);
        // Destruction requests stop and joins even without an explicit request.
    }
    check(stop.requested(), "RAII shutdown requested");
    check(Clock::now() - start < std::chrono::seconds{2}, "prompt thread shutdown");
    check(stop.waitUntil(Clock::now() + std::chrono::hours{1}), "stop interrupts long wait");
}
}
int main() {
    try {
        testTransitions(); testStartupAndConfiguration(); testCameraHysteresis(); testStore(); testShutdown();
        std::cout << "PASS: transitions, recovery, startup, configuration, telemetry metrics, RAII shutdown\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
