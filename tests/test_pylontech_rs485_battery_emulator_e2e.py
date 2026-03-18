from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import select
import signal
import tempfile
import time
import unittest

from .host_sanitizer_utils import (
    RunningHostBinary,
    assert_host_starts_cleanly,
    start_host_binary,
    stop_host_binary,
)


ROOT = Path(__file__).resolve().parents[1]
COMPONENTS_ROOT = ROOT / "components"
FIXTURE_PATH = (
    ROOT / "tests" / "fixtures" / "pylontech_rs485_battery_emulator_host.yaml"
)
ASAN_FIXTURE_PATH = (
    ROOT / "tests" / "fixtures" / "pylontech_rs485_battery_emulator_asan.yaml"
)
UBSAN_FIXTURE_PATH = (
    ROOT / "tests" / "fixtures" / "pylontech_rs485_battery_emulator_ubsan.yaml"
)
REQUEST_TIMEOUT = 5.0
NO_RESPONSE_TIMEOUT = 0.5


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


def assert_no_frame_from_fd(fd: int, timeout: float = NO_RESPONSE_TIMEOUT) -> None:
    readable, _, _ = select.select([fd], [], [], timeout)
    if readable:
        frame = os.read(fd, 512)
        raise AssertionError(f"Unexpected response frame received: {frame!r}")


class PylontechRS485BatteryEmulatorE2ETest(unittest.TestCase):
    maxDiff = None

    def test_manual_protocol_frames(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            running: RunningHostBinary | None = None
            try:
                _, running = start_host_binary(tmpdir, FIXTURE_PATH, COMPONENTS_ROOT)
                assert_host_starts_cleanly(running)

                os.write(running.master_fd, b"~20\r")
                assert_no_frame_from_fd(running.master_fd)
                running.monitor.assert_healthy("handling a truncated frame")

                os.write(running.master_fd, b"~2002464G0000FC6F\r")
                assert_no_frame_from_fd(running.master_fd)
                running.monitor.assert_healthy("handling an invalid-hex frame")

                bad_checksum_request = bytearray(
                    encode_request(address=0x02, cid2=0x4F)
                )
                bad_checksum_request[-2] = ord("0")
                os.write(running.master_fd, bytes(bad_checksum_request))
                assert_no_frame_from_fd(running.master_fd)
                running.monitor.assert_healthy("handling a bad-checksum frame")

                os.write(running.master_fd, b"~2002464F1000EC6A\r")
                assert_no_frame_from_fd(running.master_fd)
                running.monitor.assert_healthy(
                    "handling an unsupported-info-size frame"
                )

                os.write(running.master_fd, encode_request(address=0x02, cid2=0x4F))
                version_frame = decode_frame(read_frame_from_fd(running.master_fd))
                running.monitor.assert_healthy("handling the version request")
                self.assertEqual(version_frame.version, 0x20)
                self.assertEqual(version_frame.address, 0x02)
                self.assertEqual(version_frame.cid1, 0x46)
                self.assertEqual(version_frame.cid2, 0x00)
                self.assertEqual(version_frame.info, b"")

                os.write(
                    running.master_fd,
                    encode_request(address=0x02, cid2=0x42, info=b"\x02"),
                )
                analog_frame = decode_frame(read_frame_from_fd(running.master_fd))
                running.monitor.assert_healthy("handling the analog values request")
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
                    running.master_fd,
                    encode_request(address=0x02, cid2=0x92, info=b"\x02"),
                )
                management_frame = decode_frame(read_frame_from_fd(running.master_fd))
                running.monitor.assert_healthy("handling the management info request")
                self.assertEqual(management_frame.cid2, 0x00)
                management = decode_management_info(management_frame.info)
                self.assertEqual(management["pack_address"], 0x02)
                self.assertEqual(management["charge_voltage_limit"], 56000)
                self.assertEqual(management["discharge_voltage_limit"], 47000)
                self.assertEqual(management["max_charge_current"], 250)
                self.assertEqual(management["max_discharge_current"], 500)
                self.assertEqual(management["status_flags"], 0xD0)

                os.write(
                    running.master_fd,
                    encode_request(address=0x02, cid2=0x42, info=b"\x03"),
                )
                address_error_frame = decode_frame(
                    read_frame_from_fd(running.master_fd)
                )
                running.monitor.assert_healthy("handling the address error path")
                self.assertEqual(address_error_frame.cid2, 0x90)
                self.assertEqual(address_error_frame.info, b"")
            finally:
                if running is not None:
                    stdout = stop_host_binary(running)
                    if running.monitor.sanitizer_lines:
                        self.fail(f"Sanitizer error detected in host output:\n{stdout}")
                    if running.proc.returncode not in (0, -signal.SIGINT):
                        self.fail(
                            f"Unexpected host process exit code {running.proc.returncode}:\n{stdout}"
                        )

    def test_sanitizer_harness_detects_ubsan(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            running: RunningHostBinary | None = None
            try:
                _, running = start_host_binary(
                    tmpdir, UBSAN_FIXTURE_PATH, COMPONENTS_ROOT
                )
                assert_host_starts_cleanly(running)

                os.write(
                    running.master_fd,
                    encode_request(address=0x02, cid2=0x42, info=b"\x02"),
                )

                deadline = time.monotonic() + REQUEST_TIMEOUT
                while time.monotonic() < deadline:
                    if running.monitor.sanitizer_lines:
                        break
                    if running.proc.poll() is not None:
                        break
                    time.sleep(0.1)

                self.assertTrue(
                    running.monitor.sanitizer_lines,
                    msg="Expected UBSan output after deliberate signed overflow",
                )
                self.assertIn(
                    "runtime error:",
                    "\n".join(running.monitor.lines),
                )
                self.assertNotEqual(running.proc.poll(), 0)
            finally:
                if running is not None:
                    stdout = stop_host_binary(running)
                    self.assertIn("runtime error:", stdout)

    def test_sanitizer_harness_detects_asan(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            running: RunningHostBinary | None = None
            try:
                _, running = start_host_binary(
                    tmpdir, ASAN_FIXTURE_PATH, COMPONENTS_ROOT
                )
                assert_host_starts_cleanly(running)

                os.write(
                    running.master_fd,
                    encode_request(address=0x02, cid2=0x42, info=b"\x02"),
                )

                deadline = time.monotonic() + REQUEST_TIMEOUT
                while time.monotonic() < deadline:
                    if running.monitor.sanitizer_lines:
                        break
                    if running.proc.poll() is not None:
                        break
                    time.sleep(0.1)

                self.assertTrue(
                    running.monitor.sanitizer_lines,
                    msg="Expected ASan output after deliberate use-after-free",
                )
                self.assertIn(
                    "AddressSanitizer:",
                    "\n".join(running.monitor.lines),
                )
                self.assertNotEqual(running.proc.poll(), 0)
            finally:
                if running is not None:
                    stdout = stop_host_binary(running)
                    self.assertIn("AddressSanitizer:", stdout)


if __name__ == "__main__":
    unittest.main()
