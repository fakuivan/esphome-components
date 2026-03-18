from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager
import os
from pathlib import Path
import signal
import sys
import tempfile
import unittest

from aioesphomeapi import (
    APIClient,
    APIConnectionError,
    ReconnectLogic,
    UserService,
    UserServiceArgType,
)

from .host_sanitizer_utils import (
    RunningHostBinary,
    assert_host_starts_cleanly,
    start_host_binary,
    stop_host_binary,
)
from .test_pylontech_rs485_battery_emulator_e2e import (
    decode_frame,
    encode_request,
    read_frame_from_fd,
)


ROOT = Path(__file__).resolve().parents[1]
COMPONENTS_ROOT = ROOT / "components"
FIXTURE_PATH = ROOT / "tests" / "fixtures" / "pylontech_rs485_battery_emulator_api.yaml"


def find_python_pylontech_root() -> Path | None:
    for ancestor in Path(__file__).resolve().parents:
        candidate = ancestor / "python-pylontech"
        if (candidate / "pylontech" / "pylontech.py").exists():
            return candidate
        sibling_parent = ancestor.parent / "python-pylontech"
        if (sibling_parent / "pylontech" / "pylontech.py").exists():
            return sibling_parent
    return None


PYLONTECH_PYTHON_ROOT = find_python_pylontech_root()
if PYLONTECH_PYTHON_ROOT is not None and str(PYLONTECH_PYTHON_ROOT) not in sys.path:
    sys.path.insert(0, str(PYLONTECH_PYTHON_ROOT))

try:
    from pylontech.pylontech import Pylontech as PythonPylontech
except ImportError:
    PythonPylontech = None


PythonPylontechBase = PythonPylontech if PythonPylontech is not None else object


class PtySerialAdapter:
    def __init__(self, file_descriptor: int) -> None:
        self.file_descriptor = file_descriptor

    def write(self, data: bytes) -> None:
        while data:
            written = os.write(self.file_descriptor, data)
            data = data[written:]

    def readline(self) -> bytes:
        return read_frame_from_fd(self.file_descriptor)


class PtyPylontech(PythonPylontechBase):
    def __init__(self, file_descriptor: int) -> None:
        self.s = PtySerialAdapter(file_descriptor)


@asynccontextmanager
async def connected_api_client(port: int):
    client = APIClient(
        address="127.0.0.1",
        port=port,
        password="",
        noise_psk=None,
        client_info="pylontech-rs485-api-test",
    )
    connected_future: asyncio.Future[None] = asyncio.get_running_loop().create_future()

    async def on_connect() -> None:
        if not connected_future.done():
            connected_future.set_result(None)

    async def on_disconnect(expected_disconnect: bool) -> None:
        if not connected_future.done() and not expected_disconnect:
            connected_future.set_exception(
                APIConnectionError("Disconnected before fully connected")
            )

    async def on_connect_error(err: Exception) -> None:
        if not connected_future.done():
            connected_future.set_exception(err)

    reconnect_logic = ReconnectLogic(
        client=client,
        on_connect=on_connect,
        on_disconnect=on_disconnect,
        zeroconf_instance=None,
        name=f"127.0.0.1:{port}",
        on_connect_error=on_connect_error,
    )

    try:
        await reconnect_logic.start()
        await asyncio.wait_for(connected_future, timeout=15.0)
        yield client
    finally:
        await reconnect_logic.stop()
        await client.disconnect()


def get_service(services: list[UserService], name: str) -> UserService:
    for service in services:
        if service.name == name:
            return service
    raise AssertionError(f"Service {name!r} not found")


class PylontechRS485BatteryEmulatorAPIRoundTripTest(unittest.TestCase):
    def test_api_sets_and_clears_values(self) -> None:
        if PythonPylontech is None:
            self.skipTest(
                "python-pylontech and its dependencies are not importable in this environment"
            )

        with tempfile.TemporaryDirectory() as tmpdir:
            running: RunningHostBinary | None = None
            try:
                _, running = start_host_binary(tmpdir, FIXTURE_PATH, COMPONENTS_ROOT)
                assert_host_starts_cleanly(running)

                asyncio.run(self._exercise_round_trip(running))
                running.monitor.assert_healthy("verifying API-driven round trip")
            finally:
                if running is not None:
                    stdout = stop_host_binary(running)
                    if running.monitor.sanitizer_lines:
                        self.fail(f"Sanitizer error detected in host output:\n{stdout}")
                    if running.proc.returncode not in (0, -signal.SIGINT):
                        self.fail(
                            f"Unexpected host process exit code {running.proc.returncode}:\n{stdout}"
                        )

    async def _exercise_round_trip(self, running: RunningHostBinary) -> None:
        assert running.api_port is not None

        self.assert_frame_status(
            running.master_fd,
            encode_request(address=0x02, cid2=0x42, info=b"\x02"),
            0x91,
        )
        self.assert_frame_status(
            running.master_fd,
            encode_request(address=0x02, cid2=0x92, info=b"\x02"),
            0x91,
        )

        async with connected_api_client(running.api_port) as client:
            device_info = await client.device_info()
            self.assertEqual(device_info.name, "pylontech-rs485-api-e2e")

            _, services = await client.list_entities_services()
            analog_service = get_service(services, "set_battery_analog_values")
            management_service = get_service(services, "set_battery_management_info")
            clear_analog_service = get_service(services, "clear_battery_analog_values")
            clear_management_service = get_service(
                services, "clear_battery_management_info"
            )

            analog_arg_types = {arg.name: arg.type for arg in analog_service.args}
            self.assertEqual(analog_arg_types["battery_number"], UserServiceArgType.INT)
            self.assertEqual(analog_arg_types["cells"], UserServiceArgType.INT_ARRAY)
            self.assertEqual(
                analog_arg_types["temperatures"], UserServiceArgType.INT_ARRAY
            )

            management_arg_types = {
                arg.name: arg.type for arg in management_service.args
            }
            self.assertEqual(
                management_arg_types["charge_enable"], UserServiceArgType.BOOL
            )
            self.assertEqual(
                management_arg_types["full_charge_request"], UserServiceArgType.BOOL
            )

            clear_arg_types = {arg.name: arg.type for arg in clear_analog_service.args}
            self.assertEqual(clear_arg_types["battery_number"], UserServiceArgType.INT)
            self.assertEqual(len(clear_management_service.args), 1)

            await client.execute_service(
                analog_service,
                {
                    "battery_number": 1,
                    "cells": [3311, 3322, 3333, 3344],
                    "bms_temperature": 2988,
                    "temperatures": [2971, 2981, 2991],
                    "current": -123,
                    "voltage": 13310,
                    "remaining_capacity": 54321,
                    "module_capacity": 65432,
                    "cycles": 321,
                },
            )
            await client.execute_service(
                management_service,
                {
                    "battery_number": 1,
                    "charge_voltage_upper_limit": 55500,
                    "discharge_voltage_lower_limit": 42000,
                    "max_charge_current": 111,
                    "max_discharge_current": 222,
                    "charge_enable": True,
                    "discharge_enable": False,
                    "force_charge_1": True,
                    "force_charge_2": False,
                    "full_charge_request": True,
                },
            )
            await asyncio.sleep(0.2)

        pylontech = PtyPylontech(running.master_fd)
        analog_values = await asyncio.to_thread(pylontech.get_values_single, 2)
        management_info = await asyncio.to_thread(pylontech.get_management_info, 2)

        self.assertEqual(analog_values.NumberOfModule, 2)
        self.assertEqual(analog_values.NumberOfCells, 4)
        self.assertEqual(
            [round(value, 3) for value in analog_values.CellVoltages],
            [3.311, 3.322, 3.333, 3.344],
        )
        self.assertAlmostEqual(analog_values.AverageBMSTemperature, 25.7, places=3)
        self.assertEqual(
            [round(value, 1) for value in analog_values.GroupedCellsTemperatures],
            [24.0, 25.0, 26.0],
        )
        self.assertAlmostEqual(analog_values.Current, -12.3, places=3)
        self.assertAlmostEqual(analog_values.Voltage, 13.31, places=3)
        self.assertAlmostEqual(analog_values.RemainingCapacity, 54.321, places=3)
        self.assertAlmostEqual(analog_values.TotalCapacity, 65.432, places=3)
        self.assertEqual(analog_values.CycleNumber, 321)

        self.assertAlmostEqual(management_info.ChargeVoltageLimit, 55.5, places=3)
        self.assertAlmostEqual(management_info.DischargeVoltageLimit, 42.0, places=3)
        self.assertAlmostEqual(management_info.ChargeCurrentLimit, 11.1, places=3)
        self.assertAlmostEqual(management_info.DischargeCurrentLimit, 22.2, places=3)
        self.assertTrue(management_info.status.ChargeEnable)
        self.assertFalse(management_info.status.DischargeEnable)
        self.assertTrue(management_info.status.ChargeImmediately2)
        self.assertFalse(management_info.status.ChargeImmediately1)
        self.assertTrue(management_info.status.FullChargeRequest)

        async with connected_api_client(running.api_port) as client:
            _, services = await client.list_entities_services()
            clear_analog_service = get_service(services, "clear_battery_analog_values")
            clear_management_service = get_service(
                services, "clear_battery_management_info"
            )
            await client.execute_service(clear_analog_service, {"battery_number": 1})
            await client.execute_service(
                clear_management_service, {"battery_number": 1}
            )
            await asyncio.sleep(0.2)

        self.assert_frame_status(
            running.master_fd,
            encode_request(address=0x02, cid2=0x42, info=b"\x02"),
            0x91,
        )
        self.assert_frame_status(
            running.master_fd,
            encode_request(address=0x02, cid2=0x92, info=b"\x02"),
            0x91,
        )

    def assert_frame_status(self, fd: int, request: bytes, expected_cid2: int) -> None:
        os.write(fd, request)
        response = decode_frame(read_frame_from_fd(fd))
        self.assertEqual(response.cid2, expected_cid2)
        self.assertEqual(response.info, b"")
