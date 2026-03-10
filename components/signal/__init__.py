from esphome import automation, codegen as cg, config_validation as cv
from esphome.const import CONF_ID, CONF_INITIAL_VALUE, CONF_TYPE, CONF_VALUE
from esphome.core import CORE, CoroPriority, coroutine_with_priority

CODEOWNERS = ["@esphome/core"]

DOMAIN = "signal"

PARAMETER_TYPE_TRANSLATIONS = {
    "string": "std::string",
    "boolean": "bool",
}

ALLOWED_TYPE_CHARSET = set("abcdefghijklmnopqrstuvwxyz0123456789_:*&[]<>, ")

signal_ns = cg.esphome_ns.namespace("signal")
SignalBase = signal_ns.class_("SignalBase")
Signal = signal_ns.class_("Signal")
SignalSetAction = signal_ns.class_("SignalSetAction", automation.Action)


def normalize_type(value: str) -> str:
    return PARAMETER_TYPE_TRANSLATIONS.get(value, value)


def validate_signal_type(value):
    value = cv.string_strict(value)
    if set(value.lower()) <= ALLOWED_TYPE_CHARSET:
        return value
    raise cv.Invalid("Signal type contains invalid characters")


def type_expression(value: str):
    return cg.RawExpression(normalize_type(value))


def get_signal(signal_id):
    signals = CORE.config.get(DOMAIN, [])
    for signal_config in signals:
        if signal_config.get(CONF_ID) == signal_id:
            return signal_config
    raise cv.Invalid(f"Signal id '{signal_id}' not found")


def get_signal_type_expression(signal_id):
    return type_expression(get_signal(signal_id)[CONF_TYPE])


MULTI_CONF = True

CONFIG_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.declare_id(SignalBase),
        cv.Required(CONF_TYPE): validate_signal_type,
        cv.Optional(CONF_INITIAL_VALUE): cv.string_strict,
    }
)


@coroutine_with_priority(CoroPriority.LATE)
async def to_code(config):
    type_ = type_expression(config[CONF_TYPE])
    template_args = cg.TemplateArguments(type_)
    res_type = Signal.template(template_args)

    if CONF_INITIAL_VALUE in config:
        rhs = Signal.new(template_args, cg.RawExpression(config[CONF_INITIAL_VALUE]))
    else:
        rhs = Signal.new(template_args)

    cg.Pvariable(config[CONF_ID], rhs, res_type)


@automation.register_action(
    "signal.set",
    SignalSetAction,
    cv.Schema(
        {
            cv.Required(CONF_ID): cv.use_id(SignalBase),
            cv.Required(CONF_VALUE): cv.templatable(cv.string_strict),
        }
    ),
    synchronous=True,
)
async def signal_set_to_code(config, action_id, template_arg, args):
    full_id, paren = await cg.get_variable_with_full_id(config[CONF_ID])
    template_arg = cg.TemplateArguments(full_id.type, *template_arg)
    var = cg.new_Pvariable(action_id, template_arg, paren)
    templ = await cg.templatable(
        config[CONF_VALUE], args, None, to_exp=cg.RawExpression
    )
    cg.add(var.set_value(templ))
    return var
