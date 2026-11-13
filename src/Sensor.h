#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace monitor {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
using Milliseconds = std::chrono::milliseconds;
constexpr std::size_t sensorCount = 3;
enum class SensorId : std::size_t { Lidar, Radar, Camera };
enum class SensorStatus { Nominal, LatencyInjected };
struct SensorConfig { SensorId id; const char* name; Milliseconds interval; };
inline const std::array<SensorConfig, sensorCount> configs{{
    {SensorId::Lidar, "LiDAR", Milliseconds{100}},
    {SensorId::Radar, "Radar", Milliseconds{50}},
    {SensorId::Camera, "Camera", Milliseconds{33}}
}};
struct Telemetry {
    SensorId id{};
    std::uint64_t sequence = 0;
    Time timestamp{};
    SensorStatus status = SensorStatus::Nominal;
};
struct Snapshot {
    Telemetry latest{};
    std::uint64_t messages = 0;
    Time first{};
    Clock::duration maxGap{};
    std::uint64_t timelyStreak = 0;
};
class TelemetryStore {
public:
    explicit TelemetryStore(unsigned timelyIntervals = 2) : timelyIntervals_(timelyIntervals) {}
    void publish(const Telemetry& message);
    std::array<Snapshot, sensorCount> snapshot() const;
private:
    unsigned timelyIntervals_;
    mutable std::mutex mutex_;
    std::array<Snapshot, sensorCount> entries_{};
};
// A shared interruptible wait keeps even long sensor delays promptly stoppable.
class StopSignal {
public:
    void request();
    bool requested() const { return stopped_.load(); }
    bool waitUntil(Time deadline);
private:
    std::atomic<bool> stopped_{false};
    std::mutex mutex_;
    std::condition_variable condition_;
};
struct FaultSchedule {
    bool enabled = true;
    Milliseconds lidarBegin{4000}, lidarEnd{7000};
    Milliseconds cameraBegin{9000}, cameraEnd{11000};
    Milliseconds cameraInterval{120};
};
class Sensor {
public:
    using Publisher = std::function<void(const Telemetry&)>;
    Sensor(SensorConfig config, Publisher publish, StopSignal& stop,
           Time startup, FaultSchedule faults = {});
    Sensor(SensorConfig config, TelemetryStore& store, StopSignal& stop,
           Time startup, FaultSchedule faults = {});
    ~Sensor();
    Sensor(const Sensor&) = delete;
    Sensor& operator=(const Sensor&) = delete;
private:
    void run();
    SensorConfig config_;
    Publisher publish_;
    StopSignal& stop_;
    Time startup_;
    FaultSchedule faults_;
    std::thread thread_;
};
} // namespace monitor
