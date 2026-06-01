from esphome import automation
import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_TEMPERATURE,
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_PERCENT,
)

AUTO_LOAD = ["sensor"]

CONF_AMBIENT_THERMAL_OUTPUT_THRESHOLD = "ambient_thermal_output_threshold"
CONF_AMBIENT_TEMPERATURE = "ambient_temperature"
CONF_CONFIDENCE = "confidence"
CONF_CONFIDENCE_TIME = "confidence_time"
CONF_ESTIMATED_STEADY_STATE_TEMPERATURE = "estimated_steady_state_temperature"
CONF_GAIN_LEARNING_THERMAL_OUTPUT_THRESHOLD = "gain_learning_thermal_output_threshold"
CONF_INITIAL_THERMAL_GAIN = "initial_thermal_gain"
CONF_MAX_THERMAL_GAIN = "max_thermal_gain"
CONF_MIN_THERMAL_GAIN = "min_thermal_gain"
CONF_NORMALIZED_THERMAL_OUTPUT = "normalized_thermal_output"
CONF_THERMAL_OUTPUT = "thermal_output"
CONF_THERMAL_OUTPUT_STABLE_TIME = "thermal_output_stable_time"
CONF_THERMAL_OUTPUT_STABLE_TOLERANCE = "thermal_output_stable_tolerance"
CONF_LEARNING_ENABLED = "learning_enabled"
CONF_SLOPE_FILTER_TIME_CONSTANT = "slope_filter_time_constant"
CONF_STABLE_TEMPERATURE_SLOPE = "stable_temperature_slope"
CONF_TEMPERATURE_SLOPE = "temperature_slope"
CONF_THERMAL_GAIN = "thermal_gain"
CONF_THERMAL_GAIN_LEARNING_TIME_CONSTANT = "thermal_gain_learning_time_constant"
CONF_AMBIENT_LEARNING_TIME_CONSTANT = "ambient_learning_time_constant"

UNIT_CELSIUS_PER_MINUTE = "°C/min"
UNIT_CELSIUS_PER_THERMAL_OUTPUT = "°C/thermal_output"

thermal_estimator_ns = cg.esphome_ns.namespace("thermal_estimator")
ThermalEstimator = thermal_estimator_ns.class_("ThermalEstimator", cg.PollingComponent)
SetLearningEnabledAction = thermal_estimator_ns.class_(
    "SetLearningEnabledAction", automation.Action
)


def _time_period_ms(value):
    value = cv.positive_time_period_milliseconds(value)
    if value.total_milliseconds == 0:
        raise cv.Invalid("Time period must be greater than 0")
    return value


def _positive_float(value):
    value = cv.float_(value)
    if value <= 0:
        raise cv.Invalid("Value must be greater than 0")
    return value


def _non_negative_float(value):
    value = cv.float_(value)
    if value < 0:
        raise cv.Invalid("Value must be non-negative")
    return value


def _validate_thermal_gain_range(config):
    if config[CONF_MIN_THERMAL_GAIN] > config[CONF_MAX_THERMAL_GAIN]:
        raise cv.Invalid(
            "min_thermal_gain must be less than or equal to max_thermal_gain",
            [CONF_MIN_THERMAL_GAIN],
        )
    if config[CONF_MIN_THERMAL_GAIN] > config[CONF_INITIAL_THERMAL_GAIN]:
        raise cv.Invalid(
            "initial_thermal_gain must be greater than or equal to min_thermal_gain",
            [CONF_INITIAL_THERMAL_GAIN],
        )
    if config[CONF_INITIAL_THERMAL_GAIN] > config[CONF_MAX_THERMAL_GAIN]:
        raise cv.Invalid(
            "initial_thermal_gain must be less than or equal to max_thermal_gain",
            [CONF_INITIAL_THERMAL_GAIN],
        )
    return config


SENSOR_TYPES = {
    CONF_AMBIENT_TEMPERATURE: sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_ESTIMATED_STEADY_STATE_TEMPERATURE: sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_THERMAL_GAIN: sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS_PER_THERMAL_OUTPUT,
        accuracy_decimals=2,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_CONFIDENCE: sensor.sensor_schema(
        unit_of_measurement=UNIT_PERCENT,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_NORMALIZED_THERMAL_OUTPUT: sensor.sensor_schema(
        accuracy_decimals=3,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_TEMPERATURE_SLOPE: sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS_PER_MINUTE,
        accuracy_decimals=3,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
}

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ThermalEstimator),
            cv.Required(CONF_TEMPERATURE): cv.use_id(sensor.Sensor),
            cv.Required(CONF_THERMAL_OUTPUT): cv.returning_lambda,
            cv.Optional(CONF_LEARNING_ENABLED, default=True): cv.boolean,
            cv.Optional(CONF_INITIAL_THERMAL_GAIN, default=25.0): _positive_float,
            cv.Optional(CONF_MIN_THERMAL_GAIN, default=1.0): _positive_float,
            cv.Optional(CONF_MAX_THERMAL_GAIN, default=150.0): _positive_float,
            cv.Optional(CONF_AMBIENT_THERMAL_OUTPUT_THRESHOLD): cv.float_range(
                min=0.0, max=1.0
            ),
            cv.Optional(CONF_GAIN_LEARNING_THERMAL_OUTPUT_THRESHOLD): cv.float_range(
                min=0.0, max=1.0
            ),
            cv.Optional(CONF_THERMAL_OUTPUT_STABLE_TOLERANCE): _non_negative_float,
            cv.Optional(CONF_STABLE_TEMPERATURE_SLOPE, default=0.03): _positive_float,
            cv.Optional(CONF_THERMAL_OUTPUT_STABLE_TIME): _time_period_ms,
            cv.Optional(
                CONF_AMBIENT_LEARNING_TIME_CONSTANT, default="6h"
            ): _time_period_ms,
            cv.Optional(
                CONF_THERMAL_GAIN_LEARNING_TIME_CONSTANT, default="24h"
            ): _time_period_ms,
            cv.Optional(
                CONF_SLOPE_FILTER_TIME_CONSTANT, default="5min"
            ): _time_period_ms,
            cv.Optional(CONF_CONFIDENCE_TIME, default="24h"): _time_period_ms,
        }
    )
    .extend({cv.Optional(metric): schema for metric, schema in SENSOR_TYPES.items()})
    .extend(cv.polling_component_schema("10s")),
    _validate_thermal_gain_range,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    temperature_sensor = await cg.get_variable(config[CONF_TEMPERATURE])
    cg.add(var.set_temperature_sensor(temperature_sensor))

    thermal_output_lambda = await cg.process_lambda(
        config[CONF_THERMAL_OUTPUT], [], return_type=cg.optional.template(float)
    )
    cg.add(var.set_thermal_output_lambda(thermal_output_lambda))
    cg.add(var.set_learning_enabled(config[CONF_LEARNING_ENABLED]))

    cg.add(var.set_initial_thermal_gain(config[CONF_INITIAL_THERMAL_GAIN]))
    cg.add(
        var.set_thermal_gain_limits(
            config[CONF_MIN_THERMAL_GAIN], config[CONF_MAX_THERMAL_GAIN]
        )
    )
    cg.add(
        var.set_ambient_thermal_output_threshold(
            config.get(CONF_AMBIENT_THERMAL_OUTPUT_THRESHOLD, 0.02)
        )
    )
    cg.add(
        var.set_gain_learning_thermal_output_threshold(
            config.get(CONF_GAIN_LEARNING_THERMAL_OUTPUT_THRESHOLD, 0.35)
        )
    )
    cg.add(
        var.set_thermal_output_stable_tolerance(
            config.get(CONF_THERMAL_OUTPUT_STABLE_TOLERANCE, 0.03)
        )
    )
    cg.add(var.set_stable_temperature_slope(config[CONF_STABLE_TEMPERATURE_SLOPE]))
    cg.add(
        var.set_thermal_output_stable_time(
            config.get(
                CONF_THERMAL_OUTPUT_STABLE_TIME,
                cv.positive_time_period_milliseconds("20min"),
            ).total_milliseconds
        )
    )
    cg.add(
        var.set_ambient_learning_time_constant(
            config[CONF_AMBIENT_LEARNING_TIME_CONSTANT].total_milliseconds
        )
    )
    cg.add(
        var.set_thermal_gain_learning_time_constant(
            config[CONF_THERMAL_GAIN_LEARNING_TIME_CONSTANT].total_milliseconds
        )
    )
    cg.add(
        var.set_slope_filter_time_constant(
            config[CONF_SLOPE_FILTER_TIME_CONSTANT].total_milliseconds
        )
    )
    cg.add(
        var.set_confidence_time(config[CONF_CONFIDENCE_TIME].total_milliseconds)
    )

    for metric in SENSOR_TYPES:
        if sensor_config := config.get(metric):
            sens = await sensor.new_sensor(sensor_config)
            cg.add(getattr(var, f"set_{metric}_sensor")(sens))


THERMAL_ESTIMATOR_ID_SCHEMA = automation.maybe_simple_id(
    {
        cv.Required(CONF_ID): cv.use_id(ThermalEstimator),
    }
)


@automation.register_action(
    "thermal_estimator.set_learning_enabled",
    SetLearningEnabledAction,
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(ThermalEstimator),
            cv.Required(CONF_LEARNING_ENABLED): cv.boolean,
        }
    ),
    synchronous=True,
)
async def set_learning_enabled_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    cg.add(var.set_learning_enabled(config[CONF_LEARNING_ENABLED]))
    return var


@automation.register_action(
    "thermal_estimator.freeze_learning",
    SetLearningEnabledAction,
    THERMAL_ESTIMATOR_ID_SCHEMA,
    synchronous=True,
)
async def freeze_learning_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    cg.add(var.set_learning_enabled(False))
    return var


@automation.register_action(
    "thermal_estimator.resume_learning",
    SetLearningEnabledAction,
    THERMAL_ESTIMATOR_ID_SCHEMA,
    synchronous=True,
)
async def resume_learning_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)
    cg.add(var.set_learning_enabled(True))
    return var
