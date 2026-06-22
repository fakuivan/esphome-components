from esphome import automation
from esphome.components import binary_sensor, esp32
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.automation import maybe_simple_id
from esphome.const import CONF_ID, CONF_NAME
from esphome.core import CORE

DOMAIN = "mi_stick_ble_remote"
CONF_ADVERTISE_ON_BOOT = "advertise_on_boot"
CONF_CONNECTED = "connected"
CONF_LAST_POWER_REPORT_OK = "last_power_report_ok"
CONF_POWER_PRESS_DURATION = "power_press_duration"
CONF_PRODUCT_ID = "product_id"
CONF_SUSPENDED = "suspended"
CONF_VENDOR_ID = "vendor_id"

DEFAULT_NAME = "Xiaomi RC"
DEFAULT_VENDOR_ID = 0x2717
DEFAULT_PRODUCT_ID = 0x32B9
DEFAULT_POWER_PRESS_DURATION = "120ms"

mi_stick_ble_remote_ns = cg.esphome_ns.namespace(DOMAIN)
MiStickBLERemote = mi_stick_ble_remote_ns.class_("MiStickBLERemote", cg.Component)
StartAdvertisingAction = mi_stick_ble_remote_ns.class_(
    "StartAdvertisingAction", automation.Action
)
StopAdvertisingAction = mi_stick_ble_remote_ns.class_(
    "StopAdvertisingAction", automation.Action
)
PowerAction = mi_stick_ble_remote_ns.class_("PowerAction", automation.Action)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(MiStickBLERemote),
        # Xiaomi's Bluetooth service accepts names from bt_rc_name.conf; this is
        # the English stock-remote name observed in that allowlist.
        cv.Optional(CONF_NAME, default=DEFAULT_NAME): cv.string_strict,
        # Observed from the paired stock Xiaomi RC in Android dumpsys input.
        cv.Optional(CONF_VENDOR_ID, default=DEFAULT_VENDOR_ID): cv.hex_uint16_t,
        cv.Optional(CONF_PRODUCT_ID, default=DEFAULT_PRODUCT_ID): cv.hex_uint16_t,
        cv.Optional(
            CONF_POWER_PRESS_DURATION, default=DEFAULT_POWER_PRESS_DURATION
        ): cv.positive_time_period_milliseconds,
        # Keep this enabled while we still want the Mi Stick's boot scan to
        # auto-pair the device. Disable it after manual-only pairing is desired.
        cv.Optional(CONF_ADVERTISE_ON_BOOT, default=True): cv.boolean,
        cv.Optional(CONF_CONNECTED): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_LAST_POWER_REPORT_OK): binary_sensor.binary_sensor_schema(),
        cv.Optional(CONF_SUSPENDED): binary_sensor.binary_sensor_schema(),
    }
).extend(cv.COMPONENT_SCHEMA)

AUTO_LOAD = ["binary_sensor"]
DEPENDENCIES = ["esp32"]


async def to_code(config):
    if not CORE.is_esp32:
        raise cv.Invalid("The mi_stick_ble_remote component only supports ESP32.")
    if CORE.using_arduino:
        raise cv.Invalid("The mi_stick_ble_remote component requires the ESP-IDF framework.")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_name(config[CONF_NAME]))
    cg.add(var.set_vendor_id(config[CONF_VENDOR_ID]))
    cg.add(var.set_product_id(config[CONF_PRODUCT_ID]))
    cg.add(
        var.set_power_press_duration_ms(
            config[CONF_POWER_PRESS_DURATION].total_milliseconds
        )
    )
    cg.add(var.set_advertise_on_boot(config[CONF_ADVERTISE_ON_BOOT]))

    if CONF_CONNECTED in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_CONNECTED])
        cg.add(var.set_connected_sensor(sens))
    if CONF_LAST_POWER_REPORT_OK in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_LAST_POWER_REPORT_OK])
        cg.add(var.set_last_power_report_ok_sensor(sens))
    if CONF_SUSPENDED in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_SUSPENDED])
        cg.add(var.set_suspended_sensor(sens))

    esp32.include_builtin_idf_component("bt")
    esp32.include_builtin_idf_component("esp_hid")
    esp32.add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)
    esp32.add_idf_sdkconfig_option("CONFIG_BT_BLUEDROID_ENABLED", True)
    esp32.add_idf_sdkconfig_option("CONFIG_BT_BLE_ENABLED", True)
    esp32.add_idf_sdkconfig_option("CONFIG_BTDM_CTRL_MODE_BLE_ONLY", True)
    esp32.add_idf_sdkconfig_option("CONFIG_GATTS_ENABLE", True)


SIMPLE_ACTION_SCHEMA = maybe_simple_id(
    {
        cv.Required(CONF_ID): cv.use_id(MiStickBLERemote),
    }
)


@automation.register_action(
    f"{DOMAIN}.start_advertising",
    StartAdvertisingAction,
    SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
async def start_advertising_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    f"{DOMAIN}.stop_advertising",
    StopAdvertisingAction,
    SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
async def stop_advertising_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    f"{DOMAIN}.power",
    PowerAction,
    SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
async def power_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)
