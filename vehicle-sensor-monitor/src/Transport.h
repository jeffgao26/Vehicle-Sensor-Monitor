#pragma once
#include "Sensor.h"
#include <optional>
#include <string>
#include <string_view>
#include <sys/un.h>

namespace monitor {
// Text datagrams: VSM1 <sensor-id 0..2> <positive-sequence> <status 0..1>.
// Receiver assigns host steady-clock receipt time; no cross-process clock assumption.
std::string encodeTelemetry(const Telemetry& message);
std::optional<Telemetry> decodeTelemetry(std::string_view packet, Time receipt);
class DatagramSender {
public:
    explicit DatagramSender(const std::string& path);
    ~DatagramSender();
    DatagramSender(const DatagramSender&) = delete;
    DatagramSender& operator=(const DatagramSender&) = delete;
    void send(const Telemetry& message);
    std::uint64_t dropped() const { return dropped_.load(); }
private:
    int fd_ = -1;
    sockaddr_un destination_{};
    std::atomic<std::uint64_t> dropped_{0};
};
class DatagramReceiver {
public:
    DatagramReceiver(const std::string& path, TelemetryStore& store, StopSignal& stop);
    ~DatagramReceiver();
    DatagramReceiver(const DatagramReceiver&) = delete;
    DatagramReceiver& operator=(const DatagramReceiver&) = delete;
    void join();
    std::uint64_t rejected() const { return rejected_.load(); }
    bool failed() const { return failed_.load(); }
private:
    void run();
    std::string path_;
    int fd_ = -1;
    TelemetryStore& store_;
    StopSignal& stop_;
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<bool> failed_{false};
    std::thread thread_;
};
} // namespace monitor
