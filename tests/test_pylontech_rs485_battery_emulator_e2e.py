from __future__ import annotations

import contextlib
from dataclasses import dataclass
import os
from pathlib import Path
import pty
import select
import signal
import subprocess
import sys
import tempfile
import time
import tty
import unittest

import esphome.config
from esphome.core import CORE
from esphome.platformio_api import get_idedata


ROOT = Path(__file__).resolve().parents[1]
COMPONENTS_ROOT = ROOT / "components"
FIXTURE_PATH = (
    ROOT / "tests" / "fixtures" / "pylontech_rs485_battery_emulator_host.yaml"
)
REQUEST_TIMEOUT = 5.0
STARTUP_TIMEOUT = 5.0


@dataclass(frozen=True)
class DecodedFrame:
    version: int
    address: int
    cid1: int
    cid2: int
    info: bytes


def twos_complement_checksum(data: bytes, bits: int) -> int:
    mask = (1 << bits) - 1
    return (~sum(data) + 1) & mask


def encode_length(info: bytes) -> int:
    lenid = len(info) * 2
    nibble_sum = (lenid & 0xF) + ((lenid >> 4) & 0xF) + ((lenid >> 8) & 0xF)
    lchecksum = ((~(nibble_sum & 0xF)) + 1) & 0xF
    return (lchecksum << 12) | lenid


def encode_request(
    address: int, cid2: int, info: bytes = b"", version: int = 0x20
) -> bytes:
    payload = (
        f"{version:02X}{address:02X}46{cid2:02X}{encode_length(info):04X}".encode()
        + info.hex().upper().encode()
    )
    checksum = twos_complement_checksum(payload, 16)
    return b"~" + payload + f"{checksum:04X}".encode() + b"\r"


def decode_frame(raw: bytes) -> DecodedFrame:
    if not raw.startswith(b"~") or not raw.endswith(b"\r"):
        raise AssertionError(f"Invalid frame markers: {raw!r}")

    body = raw[1:-5]
    checksum = int(raw[-5:-1], 16)
    actual_checksum = twos_complement_checksum(body, 16)
    if checksum != actual_checksum:
        raise AssertionError(
            f"Checksum mismatch: expected 0x{checksum:04X}, got 0x{actual_checksum:04X}"
        )

    version = int(body[0:2], 16)
    address = int(body[2:4], 16)
    cid1 = int(body[4:6], 16)
    cid2 = int(body[6:8], 16)
    length = int(body[8:12], 16)
    lenid = length & 0x0FFF
    info_ascii = body[12 : 12 + lenid]
    if len(info_ascii) != lenid:
        raise AssertionError(
            f"Expected {lenid} ASCII info bytes, got {len(info_ascii)}"
        )

    info = bytes.fromhex(info_ascii.decode()) if info_ascii else b""
    return DecodedFrame(
        version=version, address=address, cid1=cid1, cid2=cid2, info=info
    )


def decode_u16(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 2], byteorder="big", signed=False)


def decode_s16(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 2], byteorder="big", signed=True)


def decode_analog_values(info: bytes) -> dict[str, object]:
    data_flag = info[0]
    pack_address = info[1]
    offset = 2

    cell_count = info[offset]
    offset += 1
    cell_voltages = []
    for _ in range(cell_count):
        cell_voltages.append(decode_u16(info, offset))
        offset += 2

    temperature_count = info[offset]
    offset += 1
    temperatures = []
    for _ in range(temperature_count):
        temperatures.append(decode_u16(info, offset))
        offset += 2

    current = decode_s16(info, offset)
    offset += 2
    voltage = decode_u16(info, offset)
    offset += 2
    remaining_capacity = decode_u16(info, offset)
    offset += 2
    user_defined_items = info[offset]
    offset += 1
    total_capacity = decode_u16(info, offset)
    offset += 2
    cycle_count = decode_u16(info, offset)

    return {
        "data_flag": data_flag,
        "pack_address": pack_address,
        "cell_voltages": cell_voltages,
        "temperatures": temperatures,
        "current": current,
        "voltage": voltage,
        "remaining_capacity": remaining_capacity,
        "user_defined_items": user_defined_items,
        "total_capacity": total_capacity,
        "cycle_count": cycle_count,
    }


def decode_management_info(info: bytes) -> dict[str, object]:
    return {
        "pack_address": info[0],
        "charge_voltage_limit": decode_u16(info, 1),
        "discharge_voltage_limit": decode_u16(info, 3),
        "max_charge_current": decode_u16(info, 5),
        "max_discharge_current": decode_u16(info, 7),
        "status_flags": info[9],
    }


def get_binary_path(config_path: Path) -> Path:
    CORE.reset()
    CORE.config_path = config_path
    config = esphome.config.read_config(
        {"command": "compile", "config": str(config_path)}
    )
    if config is None:
        raise RuntimeError(f"Failed to read config from {config_path}")
    return Path(get_idedata(config).firmware_elf_path)


def read_frame_from_fd(fd: int, timeout: float = REQUEST_TIMEOUT) -> bytes:
    buffer = bytearray()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = max(0.0, deadline - time.monotonic())
        readable, _, _ = select.select([fd], [], [], remaining)
        if not readable:
            continue
        chunk = os.read(fd, 512)
        if not chunk:
            continue
        buffer.extend(chunk)
        if b"\r" in buffer:
            frame, _, _ = buffer.partition(b"\r")
            return bytes(frame + b"\r")
    raise TimeoutError("Timed out waiting for emulator response frame")


def terminate_process(proc: subprocess.Popen[str]) -> str:
    with contextlib.suppress(ProcessLookupError):
        proc.send_signal(signal.SIGINT)
    try:
        stdout, _ = proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        stdout, _ = proc.communicate(timeout=5)
    return stdout


class PylontechRS485BatteryEmulatorE2ETest(unittest.TestCase):
    maxDiff = None

    def test_manual_protocol_frames(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            master_fd, slave_fd = pty.openpty()
            proc: subprocess.Popen[str] | None = None
            uart_path: Path | None = None
            try:
                tty.setraw(master_fd)
                tty.setraw(slave_fd)
                slave_path = os.ttyname(slave_fd)
                uart_path = Path(tempfile.mktemp(prefix="pylontech-uart-", dir="/tmp"))
                uart_path.symlink_to(slave_path)

                config_path = (
                    Path(tmpdir) / "pylontech_rs485_battery_emulator_host.yaml"
                )
                config_path.write_text(
                    FIXTURE_PATH.read_text()
                    .replace("EXTERNAL_COMPONENT_PATH", str(COMPONENTS_ROOT))
                    .replace("UART_PORT_PATH", str(uart_path))
                )

                compile_proc = subprocess.run(
                    [sys.executable, "-m", "esphome", "compile", str(config_path)],
                    cwd=tmpdir,
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(
                    compile_proc.returncode,
                    0,
                    msg=(compile_proc.stdout or "") + (compile_proc.stderr or ""),
                )

                binary_path = get_binary_path(config_path)
                proc = subprocess.Popen(
                    [str(binary_path)],
                    cwd=tmpdir,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                )

                deadline = time.monotonic() + STARTUP_TIMEOUT
                while time.monotonic() < deadline:
                    if proc.poll() is not None:
                        stdout = proc.stdout.read() if proc.stdout is not None else ""
                        self.fail(
                            f"Host binary exited before test traffic started:\n{stdout}"
                        )
                    time.sleep(0.1)
                    break

                os.write(master_fd, encode_request(address=0x02, cid2=0x4F))
                version_frame = decode_frame(read_frame_from_fd(master_fd))
                self.assertEqual(version_frame.version, 0x20)
                self.assertEqual(version_frame.address, 0x02)
                self.assertEqual(version_frame.cid1, 0x46)
                self.assertEqual(version_frame.cid2, 0x00)
                self.assertEqual(version_frame.info, b"")

                os.write(
                    master_fd, encode_request(address=0x02, cid2=0x42, info=b"\x02")
                )
                analog_frame = decode_frame(read_frame_from_fd(master_fd))
                self.assertEqual(analog_frame.cid2, 0x00)
                analog = decode_analog_values(analog_frame.info)
                self.assertEqual(analog["data_flag"], 0x00)
                self.assertEqual(analog["pack_address"], 0x02)
                self.assertEqual(
                    analog["cell_voltages"],
                    [
                        3397,
                        3396,
                        3397,
                        3396,
                        3397,
                        3396,
                        3397,
                        3396,
                        3397,
                        3396,
                        3397,
                        3396,
                        3397,
                        3396,
                        3397,
                    ],
                )
                self.assertEqual(analog["temperatures"], [3011, 3011, 3011, 3021, 3021])
                self.assertEqual(analog["current"], -40)
                self.assertEqual(analog["voltage"], 50981)
                self.assertEqual(analog["remaining_capacity"], 51800)
                self.assertEqual(analog["user_defined_items"], 2)
                self.assertEqual(analog["total_capacity"], 60000)
                self.assertEqual(analog["cycle_count"], 2)

                os.write(
                    master_fd, encode_request(address=0x02, cid2=0x92, info=b"\x02")
                )
                management_frame = decode_frame(read_frame_from_fd(master_fd))
                self.assertEqual(management_frame.cid2, 0x00)
                management = decode_management_info(management_frame.info)
                self.assertEqual(management["pack_address"], 0x02)
                self.assertEqual(management["charge_voltage_limit"], 56000)
                self.assertEqual(management["discharge_voltage_limit"], 47000)
                self.assertEqual(management["max_charge_current"], 250)
                self.assertEqual(management["max_discharge_current"], 500)
                self.assertEqual(management["status_flags"], 0xD0)

                os.write(
                    master_fd, encode_request(address=0x02, cid2=0x42, info=b"\x03")
                )
                address_error_frame = decode_frame(read_frame_from_fd(master_fd))
                self.assertEqual(address_error_frame.cid2, 0x90)
                self.assertEqual(address_error_frame.info, b"")
            finally:
                if proc is not None:
                    stdout = terminate_process(proc)
                    if proc.returncode not in (0, -signal.SIGINT):
                        self.fail(
                            f"Unexpected host process exit code {proc.returncode}:\n{stdout}"
                        )
                if uart_path is not None:
                    uart_path.unlink(missing_ok=True)
                os.close(master_fd)
                os.close(slave_fd)


if __name__ == "__main__":
    unittest.main()
