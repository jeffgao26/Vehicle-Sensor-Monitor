#include "Shutdown.h"
#include <algorithm>
#include <cerrno>
#include <system_error>

namespace monitor {
namespace {
std::atomic<bool> interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free, "Signal handler requires lock-free atomics");
extern "C" void onSignal(int) { interrupted.store(true, std::memory_order_relaxed); }
}
SignalHandlers::SignalHandlers() {
    interrupted.store(false, std::memory_order_relaxed);
    struct sigaction action{};
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, &previousInterrupt_) != 0)
        throw std::system_error(errno, std::generic_category(), "install SIGINT handler");
    if (sigaction(SIGTERM, &action, &previousTerminate_) != 0) {
        const int error = errno;
        sigaction(SIGINT, &previousInterrupt_, nullptr);
        throw std::system_error(error, std::generic_category(), "install SIGTERM handler");
    }
}
SignalHandlers::~SignalHandlers() {
    sigaction(SIGINT, &previousInterrupt_, nullptr);
    sigaction(SIGTERM, &previousTerminate_, nullptr);
}
void SignalHandlers::wait(StopSignal& stop, Time deadline) const {
    while (!stop.requested() && !interrupted.load(std::memory_order_relaxed) && Clock::now() < deadline)
        stop.waitUntil(std::min(deadline, Clock::now() + Milliseconds{50}));
    stop.request();
}
} // namespace monitor
