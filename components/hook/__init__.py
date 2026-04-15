from esphome import automation, codegen as cg
import esphome.config_validation as cv
from esphome.components import signal as signal_component
from esphome.const import CONF_TRIGGER_ID
from esphome.core import CoroPriority, coroutine_with_priority

BUILD_CALLBACK_AUTOMATION = getattr(automation, "build_callback_automation", None)

CODEOWNERS = ["@esphome/core"]

AUTO_LOAD = ["signal"]
MULTI_CONF = True

CONF_SIGNAL = "signal"

hook_ns = cg.esphome_ns.namespace("hook")
HookTrigger = hook_ns.class_("HookTrigger", automation.Trigger.template())

CONFIG_SCHEMA = automation.validate_automation(
    {
        cv.Required(CONF_SIGNAL): cv.use_id(signal_component.SignalBase),
        cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(HookTrigger),
    },
    single=True,
)


@coroutine_with_priority(CoroPriority.LATE)
async def to_code(config):
    full_id, paren = await cg.get_variable_with_full_id(config[CONF_SIGNAL])
    args = [(signal_component.get_signal_type_expression(config[CONF_SIGNAL]), "x")]

    if BUILD_CALLBACK_AUTOMATION is not None:
        await BUILD_CALLBACK_AUTOMATION(paren, "add_on_value_callback", args, config)
        return

    template_arg = cg.TemplateArguments(full_id.type)
    trigger = cg.new_Pvariable(config[CONF_TRIGGER_ID], template_arg, paren)
    await automation.build_automation(trigger, args, config)
