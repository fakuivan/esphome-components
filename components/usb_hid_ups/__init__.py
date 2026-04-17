import esphome.codegen as cg
from esphome.components import sensor, usb_host
from esphome.components.esp32 import (
    VARIANT_ESP32P4,
    VARIANT_ESP32S2,
    VARIANT_ESP32S3,
    only_on_variant,
)
import esphome.config_validation as cv
from esphome.const import (
    CONF_BATTERY_LEVEL,
    CONF_BATTERY_VOLTAGE,
    CONF_ID,
    CONF_UPDATE_INTERVAL,
    DEVICE_CLASS_BATTERY,
    DEVICE_CLASS_DURATION,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    UNIT_PERCENT,
    UNIT_SECOND,
    UNIT_VOLT,
)
from esphome.types import ConfigType

AUTO_LOAD = ["sensor", "usb_host"]
DEPENDENCIES = ["esp32", "usb_host"]

CONF_EXPLORE = "explore"
CONF_INPUT_VOLTAGE = "input_voltage"
CONF_INTERFACE_NUMBER = "interface_number"
CONF_INTERRUPT_IN_ENDPOINT = "interrupt_in_endpoint"
CONF_LOAD_PERCENT = "load_percent"
CONF_NORMALIZE_APC_PAGES = "normalize_apc_pages"
CONF_OUTPUT_VOLTAGE = "output_voltage"
CONF_REPORT_DESCRIPTOR_INDEX = "report_descriptor_index"
CONF_RUNTIME = "runtime"

usb_hid_ups_ns = cg.esphome_ns.namespace("usb_hid_ups")
USBHIDUPS = usb_hid_ups_ns.class_("USBHIDUPS", usb_host.USBClient)

SENSOR_TYPES: dict[str, cv.Schema] = {
    CONF_BATTERY_LEVEL: sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_BATTERY,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_RUNTIME: sensor.sensor_schema(
        unit_of_measurement=UNIT_SECOND,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_DURATION,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_LOAD_PERCENT: sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_INPUT_VOLTAGE: sensor.sensor_schema(
        unit_of_measurement=UNIT_VOLT,
        accuracy_decimals=2,
        device_class=DEVICE_CLASS_VOLTAGE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_OUTPUT_VOLTAGE: sensor.sensor_schema(
        unit_of_measurement=UNIT_VOLT,
        accuracy_decimals=2,
        device_class=DEVICE_CLASS_VOLTAGE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_BATTERY_VOLTAGE: sensor.sensor_schema(
        unit_of_measurement=UNIT_VOLT,
        accuracy_decimals=2,
        device_class=DEVICE_CLASS_VOLTAGE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
}


def _validate_metric_selection(config: ConfigType) -> ConfigType:
    if any(metric in config for metric in SENSOR_TYPES) or config[CONF_EXPLORE]:
        return config
    raise cv.Invalid(
        "Configure at least one UPS sensor or set explore: true to inspect a device"
    )


CONFIG_SCHEMA = cv.All(
    usb_host.usb_device_schema(USBHIDUPS)
    .extend(
        {
            cv.Optional(
                CONF_UPDATE_INTERVAL, default="30s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_INTERFACE_NUMBER): cv.int_range(min=0, max=31),
            cv.Optional(CONF_INTERRUPT_IN_ENDPOINT): cv.hex_uint8_t,
            cv.Optional(CONF_REPORT_DESCRIPTOR_INDEX, default=0): cv.int_range(
                min=0, max=7
            ),
            cv.Optional(CONF_EXPLORE, default=False): cv.boolean,
            cv.Optional(CONF_NORMALIZE_APC_PAGES, default=True): cv.boolean,
        }
    )
    .extend({cv.Optional(metric): schema for metric, schema in SENSOR_TYPES.items()}),
    _validate_metric_selection,
    cv.requires_component("usb_host"),
    only_on_variant(supported=[VARIANT_ESP32P4, VARIANT_ESP32S2, VARIANT_ESP32S3]),
    cv.only_with_framework("esp-idf"),
)


async def to_code(config: ConfigType) -> None:
    var = await usb_host.register_usb_client(config)

    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL].total_milliseconds))
    cg.add(var.set_explore(config[CONF_EXPLORE]))
    cg.add(var.set_normalize_apc_pages(config[CONF_NORMALIZE_APC_PAGES]))
    cg.add(var.set_report_descriptor_index(config[CONF_REPORT_DESCRIPTOR_INDEX]))

    if (interface_number := config.get(CONF_INTERFACE_NUMBER)) is not None:
        cg.add(var.set_interface_number(interface_number))

    if (interrupt_in_endpoint := config.get(CONF_INTERRUPT_IN_ENDPOINT)) is not None:
        cg.add(var.set_interrupt_in_endpoint(interrupt_in_endpoint))

    for metric in SENSOR_TYPES:
        if sensor_config := config.get(metric):
            sens = await sensor.new_sensor(sensor_config)
            cg.add(getattr(var, f"set_{metric}_sensor")(sens))
