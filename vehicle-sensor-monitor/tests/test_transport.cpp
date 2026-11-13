#include "Transport.h"
#include <iostream>
#include <cerrno>
#include <system_error>
#include <stdexcept>
#include <unistd.h>

using namespace monitor;
namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}
int main(int argc, char** argv) {
    try {
        const Time receipt = Clock::now();
        const Telemetry sent{SensorId::Camera, 42, receipt, SensorStatus::LatencyInjected};
        const auto decoded = decodeTelemetry(encodeTelemetry(sent), receipt);
        check(decoded && decoded->id == sent.id && decoded->sequence == 42 &&
              decoded->status == sent.status && decoded->timestamp == receipt, "protocol round trip");
        for (const auto* invalid : {"", "VSM2 0 1 0", "VSM1 3 1 0", "VSM1 0 0 0",
             "VSM1 0 1 2", "VSM1 -1 1 0", "VSM1 0 1 0 extra", "VSM1 0 1", "VSM1  0 1 0",
             "VSM1 0 18446744073709551616 0", "VSM1 0 1 0\n"})
            check(!decodeTelemetry(invalid, receipt), "reject malformed packet");
        if (argc > 1 && std::string(argv[1]) == "--protocol") {
            std::cout << "PASS: protocol round trip and malformed packet rejection\n";
            return 0;
        }
        TelemetryStore store;
        StopSignal stop;
        const std::string path = "/tmp/vsm-test-" + std::to_string(getpid()) + ".sock";
        {
            DatagramReceiver receiver(path, store, stop);
            bool rejected = false;
            try { DatagramReceiver duplicate(path, store, stop); }
            catch (const std::system_error&) { rejected = true; }
            check(rejected, "existing socket must not be replaced");
            DatagramSender sender(path);
            sender.send(sent);
            const auto deadline = Clock::now() + std::chrono::seconds{1};
            while (store.snapshot()[2].messages == 0 && Clock::now() < deadline)
                stop.waitUntil(Clock::now() + Milliseconds{5});
            check(store.snapshot()[2].latest.sequence == 42, "socket delivery");
            check(sender.dropped() == 0 && !receiver.failed(), "no transport errors");
        }
        check(access(path.c_str(), F_OK) != 0, "owned socket cleaned up");
        DatagramSender absent(path);
        absent.send(sent);
        check(absent.dropped() == 1, "missing receiver counted without blocking");
        std::cout << "PASS: protocol validation, socket delivery, ownership, cleanup, drops\n";
    } catch (const std::system_error& error) {
        if (argc == 1 && (error.code().value() == EPERM || error.code().value() == EACCES)) {
            std::cout << "SKIP: sandbox denies Unix sockets; protocol checks passed: " << error.what() << '\n';
            return 77;
        }
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
