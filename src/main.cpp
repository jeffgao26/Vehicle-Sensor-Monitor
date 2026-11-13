#include "Watchdog.h"
#include "Shutdown.h"
#include "Transport.h"
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace monitor;
namespace {
unsigned positive(const std::string& value) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("Expected a positive integer");
    const auto number = std::stoul(value);
    if (number == 0 || number > 3600) throw std::invalid_argument("Value must be 1..3600");
    return static_cast<unsigned>(number);
}
double seconds(Clock::duration duration) { return std::chrono::duration<double>(duration).count(); }
}
int main(int argc, char** argv) {
    try {
        unsigned duration = 15;
        Thresholds thresholds;
        FaultSchedule faults;
        std::string socketPath;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "Usage: sensor_monitor [--duration SECONDS] [--warn INTERVALS] "
                             "[--fail INTERVALS] [--recovery UPDATES] [--no-faults] [--socket PATH]\n";
                return 0;
            }
            if (arg == "--no-faults") { faults.enabled = false; continue; }
            if (arg == "--socket") {
                if (++i == argc || std::string(argv[i]).empty())
                    throw std::invalid_argument("Missing socket path");
                socketPath = argv[i];
                continue;
            }
            if (arg != "--recovery" && arg != "--duration" && arg != "--warn" && arg != "--fail")
                throw std::invalid_argument("Unknown option: " + arg);
            if (++i == argc) throw std::invalid_argument("Missing value for " + arg);
            const auto value = positive(argv[i]);
            if (arg == "--duration") duration = value;
            else if (arg == "--warn") thresholds.warningIntervals = value;
            else if (arg == "--fail") thresholds.failureIntervals = value;
            else thresholds.recoveryUpdates = value;
        }
        if (thresholds.failureIntervals <= thresholds.warningIntervals)
            throw std::invalid_argument("--fail must exceed --warn");
        SignalHandlers signals;
        TelemetryStore store(thresholds.warningIntervals);
        StopSignal stop;
        const auto startup = Clock::now();
        std::cout << "=== VEHICLE SENSOR HEALTH MONITOR ===\n"
                  << "Simulated sensors; elapsed steady-clock time in seconds.\n\n";
        Watchdog watchdog(store, stop, startup, thresholds,
            [startup](SensorId id, const Transition& transition) {
                std::cout << '[' << std::fixed << std::setprecision(2)
                          << seconds(transition.timestamp - startup) << "s] "
                          << std::left << std::setw(8) << configs[static_cast<std::size_t>(id)].name;
                if (transition.recovered) std::cout << "RECOVERED -> ";
                std::cout << healthName(transition.to) << std::endl;
            });
        std::vector<std::unique_ptr<Sensor>> sensors;
        std::unique_ptr<DatagramReceiver> receiver;
        if (socketPath.empty()) {
            for (const auto& config : configs)
                sensors.push_back(std::make_unique<Sensor>(config, store, stop, startup, faults));
        } else {
            receiver = std::make_unique<DatagramReceiver>(socketPath, store, stop);
            std::cout << "Listening on " << socketPath << std::endl;
        }
        signals.wait(stop, startup + std::chrono::seconds{duration});
        stop.request();
        sensors.clear(); // Each RAII destructor joins its worker.
        if (receiver) receiver->join();
        watchdog.join();
        const bool receiverFailed = receiver && receiver->failed();
        if (receiver) std::cout << "Rejected datagrams: " << receiver->rejected() << '\n';
        receiver.reset();
        const auto snapshots = store.snapshot();
        std::cout << "\n=== FINAL STATISTICS ===\n"
                  << "Sensor   Messages   Frequency(Hz)  Max gap(ms)  Transitions  Offline latency(ms)\n";
        for (std::size_t i = 0; i < sensorCount; ++i) {
            const auto& entry = snapshots[i];
            const auto& metric = watchdog.metrics()[i];
            const double span = seconds(entry.latest.timestamp - entry.first);
            const double frequency = entry.messages > 1 && span > 0 ? (entry.messages - 1) / span : 0;
            std::cout << std::left << std::setw(9) << configs[i].name
                      << std::setw(11) << entry.messages << std::setw(15) << frequency
                      << std::setw(13) << seconds(entry.maxGap) * 1000
                      << std::setw(13) << metric.transitions;
            if (metric.offlineDetections)
                std::cout << seconds(metric.totalDetectionLatency) * 1000 / metric.offlineDetections;
            else std::cout << "n/a";
            std::cout << '\n';
        }
        if (receiverFailed) {
            std::cerr << "Error: socket receiver failed\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
