from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.const import CONF_ON_STATE_CHANGE
import esphome.final_validate as fv
from esphome.const import CONF_ALL, CONF_ID, CONF_TIMEOUT, CONF_TRIGGER_ID

BUILD_CALLBACK_AUTOMATION = getattr(automation, "build_callback_automation", None)

DOMAIN = "freshness"
MAX_TIMEOUT_MS = (2**32) - 1

freshness_ns = cg.esphome_ns.namespace(DOMAIN)
Freshness = freshness_ns.class_("Freshness", cg.Component)
BaseAction = freshness_ns.class_("BaseAction", automation.Action, cg.Parented)
FeedAction = freshness_ns.class_("FeedAction", BaseAction)
FreshCondition = freshness_ns.class_(
    "FreshCondition", automation.Condition, cg.Parented
)
StateChangeTrigger = freshness_ns.class_(
    "StateChangeTrigger", automation.Trigger.template(cg.bool_)
)


def _node_name(node_id) -> str:
    return str(node_id)


def _validate_timeout(value):
    value = cv.positive_time_period_milliseconds(value)
    if value.total_milliseconds > MAX_TIMEOUT_MS:
        raise cv.Invalid(
            f"Freshness timeout must be <= 2^32 - 1 ms ({MAX_TIMEOUT_MS}ms, 49d17h2m47.295s)"
        )
    return value


def _build_index():
    configs = fv.full_config.get().get(DOMAIN, [])
    return {config[CONF_ID]: config for config in configs}


def _validate_cycles(configs_by_id):
    state = {}
    path = []

    def visit(node_id):
        node_state = state.get(node_id, 0)
        if node_state == 1:
            cycle_start = path.index(node_id)
            cycle = path[cycle_start:] + [node_id]
            cycle_names = " -> ".join(_node_name(item) for item in cycle)
            raise cv.Invalid(
                f"Freshness dependency cycle detected: {cycle_names}", [CONF_ALL]
            )
        if node_state == 2:
            return

        state[node_id] = 1
        path.append(node_id)
        for child_id in configs_by_id[node_id].get(CONF_ALL, []):
            visit(child_id)
        path.pop()
        state[node_id] = 2

    for node_id in configs_by_id:
        visit(node_id)


def _build_parents(configs_by_id):
    parents_by_id = {node_id: [] for node_id in configs_by_id}
    for parent_id, config in configs_by_id.items():
        for child_id in config.get(CONF_ALL, []):
            if child_id == parent_id:
                raise cv.Invalid(
                    f"Freshness '{_node_name(parent_id)}' cannot depend on itself",
                    [CONF_ALL],
                )
            parents_by_id[child_id].append(parent_id)
    return parents_by_id


def _resolve_timeout(node_id, configs_by_id, parents_by_id, cache):
    if node_id in cache:
        return cache[node_id]

    config = configs_by_id[node_id]
    if timeout := config.get(CONF_TIMEOUT):
        cache[node_id] = timeout
        return timeout

    parent_ids = parents_by_id[node_id]
    if not parent_ids:
        cache[node_id] = None
        return None

    parent_timeouts = []
    for parent_id in parent_ids:
        parent_timeout = _resolve_timeout(
            parent_id, configs_by_id, parents_by_id, cache
        )
        if parent_timeout is None:
            cache[node_id] = None
            return None
        parent_timeouts.append(parent_timeout)

    resolved_timeout = parent_timeouts[0]
    resolved_ms = resolved_timeout.total_milliseconds
    for parent_timeout in parent_timeouts[1:]:
        if parent_timeout.total_milliseconds != resolved_ms:
            parent_names = ", ".join(_node_name(parent_id) for parent_id in parent_ids)
            raise cv.Invalid(
                f"Freshness '{_node_name(node_id)}' has multiple parents with conflicting timeouts: {parent_names}",
                [CONF_TIMEOUT],
            )

    cache[node_id] = resolved_timeout
    return resolved_timeout


def _final_validate(config):
    configs_by_id = _build_index()
    _validate_cycles(configs_by_id)
    parents_by_id = _build_parents(configs_by_id)
    cache = {}

    resolved_timeout = _resolve_timeout(
        config[CONF_ID], configs_by_id, parents_by_id, cache
    )
    if CONF_ALL not in config and resolved_timeout is None:
        raise cv.Invalid(
            f"Freshness '{_node_name(config[CONF_ID])}' requires a timeout or a parent with a resolvable timeout",
            [CONF_TIMEOUT],
        )

    if resolved_timeout is not None and CONF_TIMEOUT not in config:
        config[CONF_TIMEOUT] = resolved_timeout

    return config


MULTI_CONF = True

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Freshness),
        cv.Optional(CONF_TIMEOUT): _validate_timeout,
        cv.Optional(CONF_ALL): cv.All(
            cv.ensure_list(cv.use_id(Freshness)),
            cv.Length(min=1),
        ),
        cv.Optional(CONF_ON_STATE_CHANGE): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(StateChangeTrigger),
            }
        ),
    }
).extend(cv.COMPONENT_SCHEMA)

FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_name(cg.LogStringLiteral(config[CONF_ID].id)))

    if timeout := config.get(CONF_TIMEOUT):
        cg.add(var.set_timeout(timeout.total_milliseconds))

    dependencies = config.get(CONF_ALL, [])
    cg.add(var.reserve_dependencies(len(dependencies)))
    for dependency_id in dependencies:
        dependency = await cg.get_variable(dependency_id)
        cg.add(var.add_dependency(dependency))

    for conf in config.get(CONF_ON_STATE_CHANGE, []):
        if BUILD_CALLBACK_AUTOMATION is not None:
            await BUILD_CALLBACK_AUTOMATION(
                var,
                "add_on_state_callback",
                [(cg.bool_, "x")],
                conf,
            )
            continue

        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.bool_, "x")], conf)


FRESHNESS_ID_SCHEMA = automation.maybe_simple_id(
    {
        cv.Required(CONF_ID): cv.use_id(Freshness),
    }
)


@automation.register_action(
    "freshness.feed", FeedAction, FRESHNESS_ID_SCHEMA, synchronous=True
)
async def freshness_feed_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_condition(
    "freshness.is_stale", FreshCondition, FRESHNESS_ID_SCHEMA
)
async def freshness_is_stale_to_code(config, condition_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, paren, True)


@automation.register_condition(
    "freshness.is_not_stale", FreshCondition, FRESHNESS_ID_SCHEMA
)
async def freshness_is_not_stale_to_code(config, condition_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, paren, False)
