#!/usr/bin/env python3
"""Analyze simple_logger_responder capture logs.

This parses the log format emitted by main.py and prints:
- observed RX commands
- which RX commands were handled with a TX response
- which RX commands were left unhandled
- polling frequency and interval summaries by command and address
"""

from __future__ import annotations

import argparse
import re
from collections import Counter, defaultdict, deque
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from statistics import mean


COMMAND_NAMES = {
    0x42: "Get analog values",
    0x4F: "Get protocol version",
    0x61: "Get battery system analog data",
    0x92: "Get charge/discharge management info",
    0x96: "Get software version",
}

RX_PARSED_RE = re.compile(
    r"^(?P<timestamp>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2},\d{3}) INFO RX parsed: "
    r"ver=0x(?P<ver>[0-9A-F]{2}) adr=0x(?P<adr>[0-9A-F]{2}) cid1=0x(?P<cid1>[0-9A-F]{2}) "
    r"cid2=0x(?P<cid2>[0-9A-F]{2}) info=(?P<info>[0-9A-F]*)$"
)

TX_RE = re.compile(
    r"^(?P<timestamp>\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2},\d{3}) INFO TX "
    r"0x(?P<cid2>[0-9A-F]{2}).*$"
)


@dataclass(slots=True)
class RxEvent:
    timestamp: datetime
    version: int
    address: int
    cid2: int
    info: str
    handled: bool = False


def parse_timestamp(value: str) -> datetime:
    return datetime.strptime(value, "%Y-%m-%d %H:%M:%S,%f")


def command_name(cid2: int) -> str:
    return COMMAND_NAMES.get(cid2, f"Unknown 0x{cid2:02X}")


def summarize_intervals(events: list[RxEvent]) -> str:
    if len(events) < 2:
        return "n/a"
    intervals = [
        (events[index].timestamp - events[index - 1].timestamp).total_seconds()
        for index in range(1, len(events))
    ]
    return (
        f"avg {mean(intervals):.3f}s, min {min(intervals):.3f}s, "
        f"max {max(intervals):.3f}s"
    )


def analyze_log(path: Path) -> str:
    rx_events: list[RxEvent] = []
    pending: deque[RxEvent] = deque()
    tx_counts: Counter[int] = Counter()

    with path.open("r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.rstrip("\n")

            rx_match = RX_PARSED_RE.match(line)
            if rx_match:
                event = RxEvent(
                    timestamp=parse_timestamp(rx_match.group("timestamp")),
                    version=int(rx_match.group("ver"), 16),
                    address=int(rx_match.group("adr"), 16),
                    cid2=int(rx_match.group("cid2"), 16),
                    info=rx_match.group("info"),
                )
                rx_events.append(event)
                pending.append(event)
                continue

            tx_match = TX_RE.match(line)
            if tx_match:
                tx_cid2 = int(tx_match.group("cid2"), 16)
                tx_counts[tx_cid2] += 1
                while pending:
                    event = pending.pop()
                    if not event.handled:
                        event.handled = True
                        break

    rx_by_command: dict[int, list[RxEvent]] = defaultdict(list)
    rx_by_command_and_address: dict[tuple[int, int], list[RxEvent]] = defaultdict(list)
    handled_counts: Counter[int] = Counter()
    unhandled_counts: Counter[int] = Counter()

    for event in rx_events:
        rx_by_command[event.cid2].append(event)
        rx_by_command_and_address[(event.cid2, event.address)].append(event)
        if event.handled:
            handled_counts[event.cid2] += 1
        else:
            unhandled_counts[event.cid2] += 1

    lines: list[str] = []
    lines.append(f"Log file: {path}")
    lines.append(f"Total RX parsed events: {len(rx_events)}")
    lines.append("")

    lines.append("Commands observed:")
    for cid2 in sorted(rx_by_command):
        events = rx_by_command[cid2]
        addresses = ", ".join(
            f"0x{address:02X}"
            for address in sorted({event.address for event in events})
        )
        versions = ", ".join(
            f"0x{version:02X}"
            for version in sorted({event.version for event in events})
        )
        lines.append(
            f"- 0x{cid2:02X} {command_name(cid2)}: {len(events)} RX, addresses [{addresses}], request versions [{versions}], {summarize_intervals(events)}"
        )
    lines.append("")

    lines.append("Handled vs unhandled RX:")
    for cid2 in sorted(rx_by_command):
        handled = handled_counts[cid2]
        unhandled = unhandled_counts[cid2]
        lines.append(
            f"- 0x{cid2:02X} {command_name(cid2)}: handled {handled}, unhandled {unhandled}"
        )
    lines.append("")

    lines.append("TX responses emitted:")
    if tx_counts:
        for cid2 in sorted(tx_counts):
            lines.append(f"- 0x{cid2:02X}: {tx_counts[cid2]} TX")
    else:
        lines.append("- none")
    lines.append("")

    lines.append("Polling by command/address:")
    for cid2, address in sorted(rx_by_command_and_address):
        events = rx_by_command_and_address[(cid2, address)]
        handled = sum(1 for event in events if event.handled)
        lines.append(
            f"- 0x{cid2:02X} {command_name(cid2)} @ 0x{address:02X}: {len(events)} RX, handled {handled}, {summarize_intervals(events)}"
        )
    lines.append("")

    unhandled_events = [event for event in rx_events if not event.handled]
    lines.append("Unhandled queries:")
    if unhandled_events:
        for event in unhandled_events:
            lines.append(
                f"- {event.timestamp} 0x{event.cid2:02X} {command_name(event.cid2)} @ 0x{event.address:02X} info={event.info or '<empty>'}"
            )
    else:
        lines.append("- none")

    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Analyze simple_logger_responder logs."
    )
    parser.add_argument("logfile", type=Path, help="Path to the responder log file")
    args = parser.parse_args()

    print(analyze_log(args.logfile))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
