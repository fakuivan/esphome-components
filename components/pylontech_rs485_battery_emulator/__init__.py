import esphome.codegen as cg
from esphome.components import uart
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@fahmula"]
DEPENDENCIES = ["uart"]

DOMAIN = "pylontech_rs485_battery_emulator"

CONF_PROTOCOL_VERSION = "protocol_version"
CONF_PACKS = "packs"
CONF_BATTERY_NUMBER = "battery_number"
CONF_ANALOG_VALUES = "analog_values"
CONF_CHARGE_DISCHARGE_MANAGEMENT_INFO = "charge_discharge_management_info"
CONF_CELL_VOLTAGES = "cell_voltages"
CONF_BMS_TEMPERATURE = "bms_temperature"
CONF_CELL_TEMPERATURES = "cell_temperatures"
CONF_CURRENT = "current"
CONF_VOLTAGE = "voltage"
CONF_REMAINING_CAPACITY = "remaining_capacity"
CONF_MODULE_CAPACITY = "module_capacity"
CONF_CYCLES = "cycles"
CONF_CHARGE_VOLTAGE_UPPER_LIMIT = "charge_voltage_upper_limit"
CONF_DISCHARGE_VOLTAGE_LOWER_LIMIT = "discharge_voltage_lower_limit"
CONF_MAX_CHARGE_CURRENT = "max_charge_current"
CONF_MAX_DISCHARGE_CURRENT = "max_discharge_current"
CONF_STATUS_FLAGS = "status_flags"
CONF_CHARGE_ENABLE = "charge_enable"
CONF_DISCHARGE_ENABLE = "discharge_enable"
CONF_FORCE_CHARGE_1 = "force_charge_1"
CONF_FORCE_CHARGE_2 = "force_charge_2"
CONF_FULL_CHARGE_REQUEST = "full_charge_request"

ns = cg.esphome_ns.namespace("pylontech_rs485_battery_emulator")
PylontechRS485BatteryEmulator = ns.class_(
    "PylontechRS485BatteryEmulator", cg.Component, uart.UARTDevice
)
PylontechRS485BatteryPack = ns.class_("PylontechRS485BatteryPack")
StaticVector = cg.esphome_ns.class_("StaticVector")

optional_uint16 = cg.optional.template(cg.uint16)
optional_int16 = cg.optional.template(cg.int16)
optional_uint32 = cg.optional.template(cg.uint32)
optional_bool = cg.optional.template(cg.bool_)
cell_voltage_vector = StaticVector.template(
    cg.uint16, cg.RawExpression("::pylontech_rs485::commands::MAX_CELL_COUNT")
)
cell_temperature_vector = StaticVector.template(
    cg.uint16,
    cg.RawExpression("::pylontech_rs485::commands::MAX_CELL_TEMPERATURE_PROBE_COUNT"),
)
optional_cell_voltage_vector = cg.optional.template(cell_voltage_vector)
optional_cell_temperature_vector = cg.optional.template(cell_temperature_vector)

STATUS_FLAGS_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_CHARGE_ENABLE): cv.returning_lambda,
        cv.Required(CONF_DISCHARGE_ENABLE): cv.returning_lambda,
        cv.Required(CONF_FORCE_CHARGE_1): cv.returning_lambda,
        cv.Required(CONF_FORCE_CHARGE_2): cv.returning_lambda,
        cv.Required(CONF_FULL_CHARGE_REQUEST): cv.returning_lambda,
    }
)

ANALOG_VALUES_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_CELL_VOLTAGES): cv.returning_lambda,
        cv.Required(CONF_BMS_TEMPERATURE): cv.returning_lambda,
        cv.Required(CONF_CELL_TEMPERATURES): cv.returning_lambda,
        cv.Required(CONF_CURRENT): cv.returning_lambda,
        cv.Required(CONF_VOLTAGE): cv.returning_lambda,
        cv.Optional(CONF_REMAINING_CAPACITY): cv.returning_lambda,
        cv.Required(CONF_MODULE_CAPACITY): cv.returning_lambda,
        cv.Required(CONF_CYCLES): cv.returning_lambda,
    }
)

CHARGE_DISCHARGE_MANAGEMENT_INFO_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_CHARGE_VOLTAGE_UPPER_LIMIT): cv.returning_lambda,
        cv.Required(CONF_DISCHARGE_VOLTAGE_LOWER_LIMIT): cv.returning_lambda,
        cv.Required(CONF_MAX_CHARGE_CURRENT): cv.returning_lambda,
        cv.Required(CONF_MAX_DISCHARGE_CURRENT): cv.returning_lambda,
        cv.Required(CONF_STATUS_FLAGS): STATUS_FLAGS_SCHEMA,
    }
)

PACK_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PylontechRS485BatteryPack),
        cv.Required(CONF_BATTERY_NUMBER): cv.int_range(min=1, max=15),
        cv.Required(CONF_ANALOG_VALUES): ANALOG_VALUES_SCHEMA,
        cv.Required(
            CONF_CHARGE_DISCHARGE_MANAGEMENT_INFO
        ): CHARGE_DISCHARGE_MANAGEMENT_INFO_SCHEMA,
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PylontechRS485BatteryEmulator),
        cv.Optional(CONF_PROTOCOL_VERSION, default=0x20): cv.hex_uint8_t,
        cv.Required(CONF_PACKS): cv.ensure_list(PACK_SCHEMA),
    }
).extend(uart.UART_DEVICE_SCHEMA)


async def _process_and_add_lambda(config, key, setter, return_type):
    template_ = await cg.process_lambda(config[key], [], return_type=return_type)
    return cg.add(setter(template_))


async def to_code(config):
    cg.add_library("Embedded Template Library", None)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_protocol_version(config[CONF_PROTOCOL_VERSION]))

    for pack_config in config[CONF_PACKS]:
        pack = cg.new_Pvariable(pack_config[CONF_ID], pack_config[CONF_BATTERY_NUMBER])
        cg.add(var.add_pack(pack))

        analog_values = pack_config[CONF_ANALOG_VALUES]
        await _process_and_add_lambda(
            analog_values,
            CONF_CELL_VOLTAGES,
            pack.set_cell_voltages,
            optional_cell_voltage_vector,
        )
        await _process_and_add_lambda(
            analog_values,
            CONF_BMS_TEMPERATURE,
            pack.set_bms_temperature,
            optional_uint16,
        )
        await _process_and_add_lambda(
            analog_values,
            CONF_CELL_TEMPERATURES,
            pack.set_cell_temperatures,
            optional_cell_temperature_vector,
        )
        await _process_and_add_lambda(
            analog_values,
            CONF_CURRENT,
            pack.set_current,
            optional_int16,
        )
        await _process_and_add_lambda(
            analog_values,
            CONF_VOLTAGE,
            pack.set_voltage,
            optional_uint16,
        )
        if CONF_REMAINING_CAPACITY in analog_values:
            await _process_and_add_lambda(
                analog_values,
                CONF_REMAINING_CAPACITY,
                pack.set_remaining_capacity,
                optional_uint32,
            )
        await _process_and_add_lambda(
            analog_values,
            CONF_MODULE_CAPACITY,
            pack.set_module_capacity,
            optional_uint32,
        )
        await _process_and_add_lambda(
            analog_values,
            CONF_CYCLES,
            pack.set_cycles,
            optional_uint16,
        )

        management_info = pack_config[CONF_CHARGE_DISCHARGE_MANAGEMENT_INFO]
        await _process_and_add_lambda(
            management_info,
            CONF_CHARGE_VOLTAGE_UPPER_LIMIT,
            pack.set_charge_voltage_upper_limit,
            optional_uint16,
        )
        await _process_and_add_lambda(
            management_info,
            CONF_DISCHARGE_VOLTAGE_LOWER_LIMIT,
            pack.set_discharge_voltage_lower_limit,
            optional_uint16,
        )
        await _process_and_add_lambda(
            management_info,
            CONF_MAX_CHARGE_CURRENT,
            pack.set_max_charge_current,
            optional_uint16,
        )
        await _process_and_add_lambda(
            management_info,
            CONF_MAX_DISCHARGE_CURRENT,
            pack.set_max_discharge_current,
            optional_uint16,
        )

        status_flags = management_info[CONF_STATUS_FLAGS]
        await _process_and_add_lambda(
            status_flags,
            CONF_CHARGE_ENABLE,
            pack.set_charge_enable,
            optional_bool,
        )
        await _process_and_add_lambda(
            status_flags,
            CONF_DISCHARGE_ENABLE,
            pack.set_discharge_enable,
            optional_bool,
        )
        await _process_and_add_lambda(
            status_flags,
            CONF_FORCE_CHARGE_1,
            pack.set_force_charge_1,
            optional_bool,
        )
        await _process_and_add_lambda(
            status_flags,
            CONF_FORCE_CHARGE_2,
            pack.set_force_charge_2,
            optional_bool,
        )
        await _process_and_add_lambda(
            status_flags,
            CONF_FULL_CHARGE_REQUEST,
            pack.set_full_charge_request,
            optional_bool,
        )
