#!/usr/bin/env python3
"""Log Pylontech RS485 traffic and reply to version requests.

This tool listens on a USB-to-RS485 adapter, logs every complete frame seen on
the bus, and replies to protocol version requests addressed to the configured
battery address.

It always replies to command 0x4F (get protocol version). It can also reply to
command 0x96 (get software version) when --software-version is provided.

Examples:
    python main.py --port /dev/ttyUSB0
    python main.py --port /dev/ttyUSB0 --address 0x02
    python main.py --port /dev/ttyUSB0 --software-version 0102030405
"""

from __future__ import annotations

import argparse
import importlib
import logging
import signal
import sys
import time
from dataclasses import dataclass
from typing import Any, Optional

try:
    serial = importlib.import_module("serial")
except ImportError as exc:  # pragma: no cover - depends on host environment
    raise SystemExit(
        "pyserial is required. Install it with: pip install pyserial"
    ) from exc

Serial = Any


SOI = 0x7E
EOI = 0x0D
ASCII_BYTES_PER_ENCODED_BYTE = 2
MIN_FRAME_SIZE = 14
MAX_ASCII_INFO_LENGTH = 0x0FFF

CID1_BATTERY_DATA = 0x46

CID2_NORMAL = 0x00
CID2_GET_PROTOCOL_VERSION = 0x4F
CID2_GET_SOFTWARE_VERSION = 0x96


@dataclass(slots=True)
class Frame:
    version: int
    address: int
    cid1: int
    cid2: int
    info: bytes
    raw: bytes


class FrameParseError(ValueError):
    pass


def parse_int(value: str) -> int:
    return int(value, 0)


def decode_hex_byte(high: int, low: int) -> int:
    return int(bytes((high, low)).decode("ascii"), 16)


def hex_string_checksum(payload_ascii: bytes) -> int:
    return ((~sum(payload_ascii)) + 1) & 0xFFFF


def add_length_checksum(length_ascii_bytes: int) -> int:
    if not 0 <= length_ascii_bytes <= MAX_ASCII_INFO_LENGTH:
        raise ValueError(f"INFO length out of range: {length_ascii_bytes}")
    nibble_sum = (
        ((length_ascii_bytes >> 8) & 0x0F)
        + ((length_ascii_bytes >> 4) & 0x0F)
        + (length_ascii_bytes & 0x0F)
    )
    checksum = (-nibble_sum) & 0x0F
    return (checksum << 12) | length_ascii_bytes


def parse_frame(raw: bytes) -> Frame:
    if len(raw) < MIN_FRAME_SIZE:
        raise FrameParseError("frame too short")
    if raw[0] != SOI:
        raise FrameParseError("invalid SOI")
    if raw[-1] != EOI:
        raise FrameParseError("invalid EOI")

    try:
        version = decode_hex_byte(raw[1], raw[2])
        address = decode_hex_byte(raw[3], raw[4])
        cid1 = decode_hex_byte(raw[5], raw[6])
        cid2 = decode_hex_byte(raw[7], raw[8])
        length_field = int(raw[9:13].decode("ascii"), 16)
        checksum_field = int(raw[-5:-1].decode("ascii"), 16)
    except (UnicodeDecodeError, ValueError) as exc:
        raise FrameParseError("invalid hex encoding") from exc

    length_checksum = (length_field >> 12) & 0x0F
    info_ascii_len = length_field & 0x0FFF
    expected_length_checksum = (
        -(
            ((info_ascii_len >> 8) & 0x0F)
            + ((info_ascii_len >> 4) & 0x0F)
            + (info_ascii_len & 0x0F)
        )
    ) & 0x0F
    if length_checksum != expected_length_checksum:
        raise FrameParseError("invalid length checksum")

    actual_info_ascii = raw[13:-5]
    if len(actual_info_ascii) != info_ascii_len:
        raise FrameParseError("INFO length mismatch")
    if len(actual_info_ascii) % 2 != 0:
        raise FrameParseError("INFO hex length is not even")

    expected_checksum = hex_string_checksum(raw[1:-5])
    if checksum_field != expected_checksum:
        raise FrameParseError("invalid checksum")

    try:
        info = bytes.fromhex(actual_info_ascii.decode("ascii"))
    except ValueError as exc:
        raise FrameParseError("invalid INFO hex") from exc

    return Frame(
        version=version,
        address=address,
        cid1=cid1,
        cid2=cid2,
        info=info,
        raw=raw,
    )


def encode_frame(
    version: int, address: int, cid1: int, cid2: int, info: bytes = b""
) -> bytes:
    header_ascii = f"{version:02X}{address:02X}{cid1:02X}{cid2:02X}{add_length_checksum(len(info) * 2):04X}".encode(
        "ascii"
    )
    info_ascii = info.hex().upper().encode("ascii")
    payload_ascii = header_ascii + info_ascii
    checksum_ascii = f"{hex_string_checksum(payload_ascii):04X}".encode("ascii")
    return bytes((SOI,)) + payload_ascii + checksum_ascii + bytes((EOI,))


def format_bytes(data: bytes) -> str:
    return " ".join(f"{byte:02X}" for byte in data)


def describe_frame(frame: Frame) -> str:
    return (
        f"ver=0x{frame.version:02X} adr=0x{frame.address:02X} "
        f"cid1=0x{frame.cid1:02X} cid2=0x{frame.cid2:02X} info={frame.info.hex().upper()}"
    )


class VersionResponder:
    def __init__(
        self,
        serial_port: Serial,
        *,
        address: int,
        protocol_version: int,
        software_version: Optional[bytes],
        reply_delay: float,
    ) -> None:
        self.serial_port = serial_port
        self.address = address
        self.protocol_version = protocol_version
        self.software_version = software_version
        self.reply_delay = reply_delay
        self._buffer = bytearray()
        self._running = True

    def stop(self, *_args: object) -> None:
        self._running = False

    def run(self) -> None:
        while self._running:
            chunk = self.serial_port.read(256)
            if not chunk:
                continue
            self._buffer.extend(chunk)
            self._process_buffer()

    def _process_buffer(self) -> None:
        while True:
            try:
                soi_index = self._buffer.index(SOI)
            except ValueError:
                self._buffer.clear()
                return

            if soi_index > 0:
                noise = bytes(self._buffer[:soi_index])
                logging.debug("RX noise: %s", format_bytes(noise))
                del self._buffer[:soi_index]

            try:
                eoi_index = self._buffer.index(EOI, 1)
            except ValueError:
                return

            raw_frame = bytes(self._buffer[: eoi_index + 1])
            del self._buffer[: eoi_index + 1]
            self._handle_frame(raw_frame)

    def _handle_frame(self, raw_frame: bytes) -> None:
        logging.info("RX raw: %s", format_bytes(raw_frame))
        try:
            frame = parse_frame(raw_frame)
        except FrameParseError as exc:
            logging.warning("RX invalid frame: %s", exc)
            return

        logging.info("RX parsed: %s", describe_frame(frame))

        if frame.cid1 != CID1_BATTERY_DATA:
            return
        if frame.address != self.address:
            return

        if frame.cid2 == CID2_GET_PROTOCOL_VERSION:
            if frame.info:
                logging.warning(
                    "Ignoring 0x4F request with unexpected INFO: %s",
                    frame.info.hex().upper(),
                )
                return
            self._send_protocol_version_reply(frame)
            return

        if frame.cid2 == CID2_GET_SOFTWARE_VERSION:
            if self.software_version is None:
                logging.debug(
                    "Ignoring 0x96 request because no software version is configured"
                )
                return
            if len(frame.info) != 1:
                logging.warning(
                    "Ignoring 0x96 request with invalid INFO length: %d",
                    len(frame.info),
                )
                return
            if frame.info[0] != self.address:
                logging.warning(
                    "Ignoring 0x96 request with INFO address 0x%02X for local address 0x%02X",
                    frame.info[0],
                    self.address,
                )
                return
            self._send_software_version_reply(frame)

    def _send_protocol_version_reply(self, request: Frame) -> None:
        response = encode_frame(
            version=self.protocol_version,
            address=request.address,
            cid1=CID1_BATTERY_DATA,
            cid2=CID2_NORMAL,
            info=b"",
        )
        self._write_response(response, "0x4F protocol version")

    def _send_software_version_reply(self, request: Frame) -> None:
        assert self.software_version is not None
        response = encode_frame(
            version=self.protocol_version,
            address=request.address,
            cid1=CID1_BATTERY_DATA,
            cid2=CID2_NORMAL,
            info=bytes((request.info[0],)) + self.software_version,
        )
        self._write_response(response, "0x96 software version")

    def _write_response(self, response: bytes, label: str) -> None:
        if self.reply_delay > 0:
            time.sleep(self.reply_delay)
        self.serial_port.write(response)
        self.serial_port.flush()
        logging.info("TX %s: %s", label, format_bytes(response))


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Log Pylontech RS485 packets and reply to version requests."
    )
    parser.add_argument(
        "--port", required=True, help="Serial device, for example /dev/ttyUSB0"
    )
    parser.add_argument("--baudrate", type=int, default=115200, help="Serial baud rate")
    parser.add_argument(
        "--address",
        type=parse_int,
        default=0x02,
        help="Target battery address in decimal or hex, for example 2 or 0x02",
    )
    parser.add_argument(
        "--protocol-version",
        type=parse_int,
        default=0x35,
        help="Version byte returned in 0x4F responses",
    )
    parser.add_argument(
        "--software-version",
        type=parse_software_version,
        help="Optional 5-byte software version as 10 hex digits for 0x96 responses",
    )
    parser.add_argument(
        "--reply-delay-ms",
        type=float,
        default=0.0,
        help="Optional delay before sending replies",
    )
    parser.add_argument(
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR"],
        help="Logging verbosity",
    )
    return parser


def parse_software_version(value: str) -> bytes:
    normalized = value.replace(" ", "")
    if len(normalized) != 10:
        raise argparse.ArgumentTypeError(
            "software version must be exactly 10 hex digits"
        )
    try:
        return bytes.fromhex(normalized)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("software version must be valid hex") from exc


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()

    if not 0 <= args.address <= 0xFF:
        parser.error("address must fit in one byte")
    if not 0 <= args.protocol_version <= 0xFF:
        parser.error("protocol version must fit in one byte")

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    serial_port = serial.Serial(
        port=args.port,
        baudrate=args.baudrate,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=0.2,
        write_timeout=0.5,
        exclusive=True,
    )

    responder = VersionResponder(
        serial_port,
        address=args.address,
        protocol_version=args.protocol_version,
        software_version=args.software_version,
        reply_delay=args.reply_delay_ms / 1000.0,
    )

    signal.signal(signal.SIGINT, responder.stop)
    signal.signal(signal.SIGTERM, responder.stop)

    logging.info(
        "Listening on %s at %d baud for address 0x%02X",
        args.port,
        args.baudrate,
        args.address,
    )
    if args.software_version is None:
        logging.info("0x96 replies disabled")
    else:
        logging.info(
            "0x96 replies enabled with payload %s", args.software_version.hex().upper()
        )

    try:
        responder.run()
    finally:
        serial_port.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
