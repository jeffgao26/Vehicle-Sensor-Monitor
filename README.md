# Vehicle Sensor Health Monitor

A dependency-free C++17 application that simulates vehicle telemetry, detects missing or delayed updates, and requires stable traffic before declaring recovery. It demonstrates concurrent workers, bounded shared state, signal-safe shutdown coordination, and optional process isolation over Unix-domain datagrams.

**Sensors are simulated. There is no real vehicle hardware interface or hard real-time guarantee.**

## Build, demonstrate, test

Requires CMake 3.16+, Clang on macOS or GCC/Clang on Linux, and platform threads. Python 3 enables process integration tests; the application and C++ tests have no third-party dependencies.

```sh
git clone https://github.com/jeffgao26/Vehicle-Sensor-Monitor.git
cd Vehicle-Sensor-Monitor
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/sensor_monitor
ctest --test-dir build --output-on-failure
```

The default demo ends automatically after 15 seconds. Ctrl+C or SIGTERM also triggers stop, joins all workers, and prints statistics.

```sh
./build/sensor_monitor --duration 5 --no-faults
./build/sensor_monitor --warn 2 --fail 5 --recovery 3
./build/sensor_monitor --help
```

Integer options accept 1–3600. Duration is seconds; warning/failure thresholds multiply each sensor's nominal interval. Failure must exceed warning. `--recovery` is the required number of consecutive timely inter-arrival gaps after degradation. Fault windows and Camera's delayed interval are configurable in `FaultSchedule` in `src/Sensor.h`.

## Architecture and synchronization

| Thread | Responsibility |
| --- | --- |
| LiDAR | Publish every 100 ms (nominal 10 Hz) |
| Radar | Publish every 50 ms (nominal 20 Hz) |
| Camera | Publish every 33 ms (nominal ~30.3 Hz) |
| Watchdog | Snapshot and evaluate health every 50 ms |
| Main | Parse options, observe shutdown signals, coordinate lifetime and final report |

Each message has a sensor ID, increasing publication sequence, `steady_clock` timestamp, and simulated status (`Nominal` or `LatencyInjected`). A mutex protects a fixed array of latest messages, counters, maximum gaps, and timely-update streaks. The watchdog copies this array under a short lock and evaluates/reports outside it. Memory use stays bounded; there is no unbounded queue or busy waiting.

Workers use RAII: destructors request the shared stop and join, including during stack unwinding. The atomic stop flag and condition variable allow interrupted timed waits without lost notifications. No worker sleeps while holding the telemetry mutex. Sensors schedule from actual publication time instead of producing catch-up bursts after scheduler delays. The watchdog owns its metrics until joined.

SIGINT/SIGTERM handlers only set a compile-time-verified lock-free atomic flag. Main checks it using timed waits of at most 50 ms, then performs normal stop/notify/join operations. No mutex, terminal I/O, or condition-variable call occurs in the signal handler. Scheduling may increase actual shutdown latency. Previous handlers are restored after workers stop.

## Health and recovery hysteresis

`HealthTracker` uses supplied timestamps, allowing deterministic unit tests:

- **STARTING:** no telemetry yet; remains in this state until the warning threshold measured from startup.
- **HEALTHY:** latest update is younger than the warning threshold.
- **DEGRADED:** latest update is at least two nominal intervals old (default).
- **OFFLINE:** latest update is at least five nominal intervals old (default).

Once degraded/offline, a fresh message alone cannot restore HEALTHY. Recovery requires three consecutive inter-arrival gaps shorter than the warning threshold, plus at least three publications since the last observed timeout. A late gap resets the store's streak; another observed timeout resets the tracker's recovery baseline. This allows OS jitter while rejecting Camera's injected 120 ms cadence. Store-side counting includes multiple updates between watchdog polls; repeatedly polling the same message never advances recovery. Startup with fresh telemetry can become healthy immediately.

An offline sensor with resumed but unproven traffic becomes DEGRADED while recovery is pending. `RECOVERED -> HEALTHY` is printed only after the stability requirement succeeds; recovery is an event, not a persistent state. Only transitions are printed.

## Scheduled faults

All windows use one startup timestamp; there is no randomness:

| Window | Behavior |
| --- | --- |
| 0–4 s | Nominal publication |
| 4–7 s | LiDAR suppresses messages; publication resumes at 7 s |
| 9–11 s | Camera changes from 33 ms to 120 ms publication intervals |
| Other times | Nominal publication |

Camera's injected interval exceeds its 66 ms warning threshold and stays below its 165 ms offline threshold. Hysteresis keeps it degraded until stable updates return after 11 seconds. OS scheduling affects precise transition times and measured frequency.

Actual transition output from a macOS run:

```text
[0.06s] Radar   HEALTHY
[0.06s] Camera  HEALTHY
[0.11s] LiDAR   HEALTHY
[4.20s] LiDAR   DEGRADED
[4.46s] LiDAR   OFFLINE
[7.04s] LiDAR   DEGRADED
[7.31s] LiDAR   RECOVERED -> HEALTHY
[9.13s] Camera  DEGRADED
[11.12s] Camera  RECOVERED -> HEALTHY
```

## Separate-process failure demo

`--socket PATH` replaces the monitor's local sensor workers with one datagram receiver thread. The watchdog runs independently of the simulator, so loss of that process produces freshness timeouts. The simulator still uses three sensor threads and supports the same scheduled faults; their windows are relative to **simulator** startup. The monitor's startup grace period begins at **monitor** startup. Start the simulator promptly to avoid initial no-message warnings.

Run these commands from the project directory in a regular terminal. First terminal:

```sh
./build/sensor_monitor --socket /tmp/vehicle-health.sock --duration 60
```

Second terminal:

```sh
./build/sensor_simulator --socket /tmp/vehicle-health.sock --duration 60 --no-faults &
simulator_pid=$!
# Wait until the monitor shows all sensors HEALTHY, then simulate an abrupt crash:
kill -KILL "$simulator_pid"
wait "$simulator_pid"
# Watch the monitor report DEGRADED and OFFLINE for all three sensors.
# Restart it and watch stable traffic produce RECOVERED -> HEALTHY:
./build/sensor_simulator --socket /tmp/vehicle-health.sock --duration 30 --no-faults
```

Ctrl+C stops either foreground executable gracefully. `kill -KILL` intentionally bypasses the simulator's cleanup; the monitor remains alive. The monitor owns the socket pathname, refuses to overwrite an existing path, and removes its socket on normal or signal-driven shutdown. If the **monitor** is killed with SIGKILL, its stale socket may need manual removal before reuse. Use a unique short path for concurrent demos.

### Transport details

Datagrams use a small versioned text format, `VSM1 <id 0..2> <positive sequence> <status 0..1>`. The receiver rejects malformed, oversized, out-of-range, and unsupported-version packets and counts rejections. The receiver timestamps each accepted message using host `steady_clock` receipt time; the protocol does not serialize C++ objects or assume portable cross-process `time_point` representations. Thus IPC health measures receipt freshness, including transport scheduling.

Sender sockets are nonblocking: missing receivers/full queues count as dropped datagrams instead of stalling workers. The receiver uses a 50 ms `poll` timeout for bounded shutdown waits. Fatal receiver errors request shutdown and cause a nonzero exit. Sequence values are diagnostic, not replay protection: restarting the simulator resets them while monitor message totals continue accumulating. Use this local demo with one trusted producer per socket; sender authentication, duplicate detection, and physical sensor timestamp validation are future work.

## Metrics

The final table reports:

| Metric | Definition |
| --- | --- |
| Messages | Accepted publications/received datagrams per sensor |
| Frequency | `(messages - 1) / (last timestamp - first timestamp)`; includes injected gaps within that span |
| Maximum gap | Largest consecutive publication/receipt gap |
| Transitions | All state changes, including initial health, pending recovery, and successful recovery |
| Offline latency | Mean duration from first expected but missing message (`last timestamp + nominal interval`, or startup plus interval if never seen) to OFFLINE decision |

Frequency and maximum gap are zero with fewer than two messages; offline latency is `n/a` without an offline decision. Detection latency includes timeout policy plus polling/scheduling delay; it is not measured from the exact injected fault onset. These are simulated host observations, not hardware sampling rates or device timing guarantees.

## Tests, sanitizer, and CI

C++ tests use explicit checks enabled in release builds. Coverage includes timeout boundaries, startup, custom/invalid thresholds, no duplicate transitions, recovery from degraded/offline, no advancement from repeated polling, recovery across skipped polls, Camera hysteresis, telemetry metrics, RAII shutdown, protocol round trips, malformed datagrams, socket ownership, delivery, cleanup, and sender drops.

Python process tests exercise SIGINT and SIGTERM in both executables, then (where sockets are permitted) inject malformed datagrams, kill the simulator, verify all sensors go offline, restart it, verify recovery, and check socket cleanup. CTest applies timeouts to detect hangs.

```sh
cmake -S . -B build-tsan -DENABLE_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
./build-tsan/sensor_monitor
```

ThreadSanitizer requires a supported runtime/OS and can fail to initialize in containers or on unsupported memory layouts. It adds overhead and only detects races exercised by a run.

Some restricted sandboxes prohibit Unix socket binding. IPC tests explicitly report SKIPPED (exit 77) for permission restrictions; protocol and signal tests still run. To make IPC mandatory:

```sh
cmake -S . -B build -DREQUIRE_IPC_TESTS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The included `.github/workflows/ci.yml` builds Linux GCC, Clang, and Clang+ThreadSanitizer configurations, runs tests with IPC required, and runs the scheduled demo. The workflow is located at the repository root so GitHub can discover it. The workflow follows the [GitHub Actions workflow syntax](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax) and [checkout action usage](https://github.com/actions/checkout).

Local validation on macOS/AppleClang 17: builds, watchdog/protocol/signal tests, and the 15-second local demo passed; sanitizer tests passed without reported races. Socket binding was denied by the execution sandbox, so socket-delivery and process-kill/restart tests were skipped, not verified. Linux CI is configured but has not been run on GitHub.

## Limitations and next steps

Health currently measures freshness, not payload accuracy or reported device errors. Storage is latest-value only, with no delivery guarantee. Ordinary OS scheduling and output introduce jitter. Unix sockets and POSIX signal handling target macOS/Linux. The IPC endpoint is for a trusted local demo, not a production security boundary.

Next useful improvements are separate reported-device-fault health, sequence integrity checks with explicit producer sessions, and bounded latency histograms. A real hardware adapter should distinguish device acquisition timestamps from host receipt timestamps.
