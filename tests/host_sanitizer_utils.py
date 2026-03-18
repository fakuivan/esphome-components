from __future__ import annotations

import contextlib
from dataclasses import dataclass
import os
from pathlib import Path
import pty
import signal
import subprocess
import sys
import tempfile
import threading
import time
import tty

import esphome.config
from esphome.core import CORE
from esphome.platformio_api import get_idedata


STARTUP_TIMEOUT = 15.0
ASAN_OPTIONS = "detect_leaks=1:halt_on_error=1"
UBSAN_OPTIONS = "halt_on_error=1:print_stacktrace=1"
SANITIZER_MARKERS = (
    "AddressSanitizer:",
    "UndefinedBehaviorSanitizer:",
    "runtime error:",
)


class ProcessOutputMonitor:
    def __init__(self, proc: subprocess.Popen[str]) -> None:
        self.proc = proc
        self.lines: list[str] = []
        self.sanitizer_lines: list[str] = []
        self._thread = threading.Thread(target=self._read, daemon=True)
        self._thread.start()

    def _read(self) -> None:
        stdout = self.proc.stdout
        if stdout is None:
            return
        for line in stdout:
            stripped = line.rstrip()
            self.lines.append(stripped)
            if any(marker in stripped for marker in SANITIZER_MARKERS):
                self.sanitizer_lines.append(stripped)

    def join(self, timeout: float = 5.0) -> None:
        self._thread.join(timeout=timeout)

    def assert_healthy(self, context: str) -> None:
        if self.sanitizer_lines:
            tail = "\n".join(self.lines[-100:])
            raise AssertionError(f"Sanitizer error detected while {context}:\n{tail}")
        return_code = self.proc.poll()
        if return_code is None or return_code == 0:
            return
        if return_code in (-signal.SIGINT, -signal.SIGTERM, -signal.SIGKILL):
            return
        tail = "\n".join(self.lines[-100:])
        raise AssertionError(
            f"Host process exited unexpectedly while {context}: {return_code}\n{tail}"
        )


@dataclass
class RunningHostBinary:
    proc: subprocess.Popen[str]
    monitor: ProcessOutputMonitor
    master_fd: int
    slave_fd: int
    uart_path: Path


def sanitizer_env() -> dict[str, str]:
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = ASAN_OPTIONS
    env["UBSAN_OPTIONS"] = UBSAN_OPTIONS
    return env


def get_binary_path(config_path: Path) -> Path:
    CORE.reset()
    CORE.config_path = config_path
    config = esphome.config.read_config(
        {"command": "compile", "config": str(config_path)}
    )
    if config is None:
        raise RuntimeError(f"Failed to read config from {config_path}")
    return Path(get_idedata(config).firmware_elf_path)


def write_runtime_config(
    tmpdir: str, fixture_path: Path, components_root: Path, uart_path: Path
) -> Path:
    config_path = Path(tmpdir) / fixture_path.name
    config_path.write_text(
        fixture_path.read_text()
        .replace("EXTERNAL_COMPONENT_PATH", str(components_root))
        .replace("UART_PORT_PATH", str(uart_path))
    )
    return config_path


def start_host_binary(
    tmpdir: str, fixture_path: Path, components_root: Path
) -> tuple[Path, RunningHostBinary]:
    master_fd, slave_fd = pty.openpty()
    tty.setraw(master_fd)
    tty.setraw(slave_fd)
    slave_path = os.ttyname(slave_fd)
    uart_path = Path(tempfile.mktemp(prefix="pylontech-uart-", dir="/tmp"))
    uart_path.symlink_to(slave_path)

    config_path = write_runtime_config(tmpdir, fixture_path, components_root, uart_path)
    compile_proc = subprocess.run(
        [sys.executable, "-m", "esphome", "compile", str(config_path)],
        cwd=tmpdir,
        capture_output=True,
        text=True,
        env=sanitizer_env(),
    )
    if compile_proc.returncode != 0:
        raise AssertionError((compile_proc.stdout or "") + (compile_proc.stderr or ""))

    binary_path = get_binary_path(config_path)
    proc = subprocess.Popen(
        [str(binary_path)],
        cwd=tmpdir,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=sanitizer_env(),
    )
    monitor = ProcessOutputMonitor(proc)
    return config_path, RunningHostBinary(
        proc=proc,
        monitor=monitor,
        master_fd=master_fd,
        slave_fd=slave_fd,
        uart_path=uart_path,
    )


def stop_host_binary(running: RunningHostBinary) -> str:
    with contextlib.suppress(ProcessLookupError):
        running.proc.send_signal(signal.SIGINT)
    try:
        running.proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        running.proc.kill()
        running.proc.wait(timeout=5)
    running.monitor.join()
    if running.proc.stdout is not None:
        running.proc.stdout.close()
    stdout = "\n".join(running.monitor.lines)
    running.uart_path.unlink(missing_ok=True)
    os.close(running.master_fd)
    os.close(running.slave_fd)
    return stdout


def assert_host_starts_cleanly(running: RunningHostBinary) -> None:
    deadline = time.monotonic() + STARTUP_TIMEOUT
    while time.monotonic() < deadline:
        if running.proc.poll() is not None:
            stdout = "\n".join(running.monitor.lines)
            raise AssertionError(
                f"Host binary exited before test traffic started:\n{stdout}"
            )
        time.sleep(0.1)
        running.monitor.assert_healthy("starting the host binary")
        return
    raise AssertionError("Host binary did not finish startup checks in time")
