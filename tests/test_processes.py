"""End-to-end checks use real processes; pure timeout tests live in C++."""
import errno
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

monitor, simulator = map(os.path.abspath, sys.argv[1:3])
signals_only = "--signals" in sys.argv[3:]
require_ipc = "--require-ipc" in sys.argv[3:]
processes = []


def launch(command, output):
    handle = open(output, "w")
    try:
        process = subprocess.Popen(command, stdout=handle, stderr=subprocess.STDOUT)
    finally:
        handle.close()
    processes.append(process)
    return process


def wait_for(path, predicate, process, label, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = path.read_text() if path.exists() else ""
        if predicate(text):
            return text
        if process.poll() is not None:
            raise AssertionError(f"{label}: exited {process.returncode}\n{text}")
        time.sleep(0.02)
    raise AssertionError(f"{label}: timeout\n{path.read_text()}")


def all_states(text, state):
    return all(re.search(rf"{sensor}\s+{re.escape(state)}", text)
               for sensor in ("LiDAR", "Radar", "Camera"))


try:
    # /tmp keeps sockaddr_un paths short even when the platform's temp root is long.
    with tempfile.TemporaryDirectory(prefix="vsm-", dir="/tmp") as directory:
        root = Path(directory)
        if signals_only:
            for executable, extra in ((monitor, []), (simulator, ["--socket", str(root / "absent.sock")])):
                for signum in (signal.SIGINT, signal.SIGTERM):
                    log = root / f"signal-{Path(executable).name}-{signum}.log"
                    process = launch([executable, "--duration", "30", "--no-faults", *extra], log)
                    ready = (lambda text: all_states(text, "HEALTHY")) if executable == monitor else (lambda text: "sends to" in text)
                    wait_for(log, ready, process, "signal test ready")
                    process.send_signal(signum)
                    assert process.wait(timeout=5) == 0, log.read_text()
                    expected = "=== FINAL STATISTICS ===" if executable == monitor else "Simulator stopped."
                    assert expected in log.read_text()
            print("PASS: SIGINT and SIGTERM stop/join both executables cleanly")
            sys.exit(0)
        # Probe platform support without hiding IPC failures on CI.
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as probe:
            try:
                probe.bind(str(root / "probe.sock"))
            except OSError as error:
                if not require_ipc and error.errno in (errno.EPERM, errno.EACCES):
                    print(f"SKIP: sandbox denies Unix socket binding: {error}")
                    sys.exit(77)
                raise
        endpoint = str(root / "sensor.sock")
        log = root / "monitor.log"
        receiver = launch([monitor, "--socket", endpoint, "--duration", "30"], log)
        wait_for(log, lambda text: "Listening on" in text, receiver, "receiver ready")
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as client:
            client.sendto(b"malformed", endpoint)
            client.sendto(b"VSM1 0 1 0" + b"x" * 256, endpoint)
        command = [simulator, "--socket", endpoint, "--duration", "30", "--no-faults"]
        sender = launch(command, root / "sender.log")
        wait_for(log, lambda text: all_states(text, "HEALTHY"), receiver, "all healthy")
        sender.kill()  # Abrupt failure: no cooperative shutdown or final message.
        sender.wait(timeout=5)
        wait_for(log, lambda text: all_states(text, "OFFLINE"), receiver, "process loss detected")
        restarted = launch(command, root / "restart.log")
        wait_for(log, lambda text: all_states(text, "RECOVERED -> HEALTHY"), receiver, "restart recovery")
        restarted.send_signal(signal.SIGTERM)
        assert restarted.wait(timeout=5) == 0
        assert "Simulator stopped." in (root / "restart.log").read_text()
        receiver.send_signal(signal.SIGINT)
        assert receiver.wait(timeout=5) == 0
        text = log.read_text()
        assert "=== FINAL STATISTICS ===" in text
        assert "Rejected datagrams: 2" in text
        assert not Path(endpoint).exists(), "socket left behind after SIGINT"
        print("PASS: malformed datagrams, simulator kill/restart, SIGINT/SIGTERM, socket cleanup")
finally:
    for process in processes:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=5)
