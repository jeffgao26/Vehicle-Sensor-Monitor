#include "Sensor.h"
#include "Shutdown.h"
#include "Transport.h"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

using namespace monitor;
int main(int argc, char** argv) {
    try {
        std::string path;
        unsigned duration = 15;
        FaultSchedule faults;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "Usage: sensor_simulator --socket PATH [--duration SECONDS] [--no-faults]\n";
                return 0;
            }
            if (arg == "--no-faults") { faults.enabled = false; continue; }
            if (arg != "--socket" && arg != "--duration")
                throw std::invalid_argument("Unknown option: " + arg);
            if (++i == argc) throw std::invalid_argument("Missing value for " + arg);
            const std::string value = argv[i];
            if (arg == "--socket") { path = value; continue; }
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                throw std::invalid_argument("Duration must be an integer 1..3600");
            const auto parsed = std::stoul(value);
            if (parsed < 1 || parsed > 3600) throw std::invalid_argument("Duration must be 1..3600");
            duration = static_cast<unsigned>(parsed);
        }
        if (path.empty()) throw std::invalid_argument("--socket PATH is required");
        SignalHandlers signals;
        StopSignal stop;
        DatagramSender sender(path);
        const auto startup = Clock::now();
        std::vector<std::unique_ptr<Sensor>> sensors;
        for (const auto& config : configs)
            sensors.push_back(std::make_unique<Sensor>(config,
                [&sender](const Telemetry& message) { sender.send(message); }, stop, startup, faults));
        std::cout << "Simulator PID " << getpid() << " sends to " << path << "; simulated sensors only." << std::endl;
        signals.wait(stop, startup + std::chrono::seconds{duration});
        sensors.clear();
        std::cout << "Simulator stopped. Dropped datagrams: " << sender.dropped() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
