#pragma once
#include "Sensor.h"
#include <csignal>

namespace monitor {
// Install before starting workers. The handler only sets a lock-free atomic flag;
// main performs all condition-variable notification and thread cleanup.
class SignalHandlers {
public:
    SignalHandlers();
    ~SignalHandlers();
    SignalHandlers(const SignalHandlers&) = delete;
    SignalHandlers& operator=(const SignalHandlers&) = delete;
    void wait(StopSignal& stop, Time deadline) const;
private:
    struct sigaction previousInterrupt_{};
    struct sigaction previousTerminate_{};
};
} // namespace monitor
