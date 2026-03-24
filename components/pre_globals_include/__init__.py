from pathlib import Path

from esphome import codegen as cg, config_validation as cv
from esphome.const import CONF_PATH
from esphome.core import CORE, CoroPriority, coroutine_with_priority
from esphome.helpers import copy_file_if_changed

CONF_C_HEADER = "c_header"

VALID_HEADER_EXTENSIONS = {".h", ".hpp", ".tcc"}

MULTI_CONF = True


def validate_header_file(value: object) -> Path:
    path = cv.file_(value)
    if path.suffix not in VALID_HEADER_EXTENSIONS:
        raise cv.Invalid(
            "Header file has invalid extension "
            f"{path.suffix} - valid extensions are {', '.join(sorted(VALID_HEADER_EXTENSIONS))}"
        )
    return path


CONFIG_SCHEMA = cv.maybe_simple_value(
    cv.Schema(
        {
            cv.Required(CONF_PATH): validate_header_file,
            cv.Optional(CONF_C_HEADER, default=False): cv.boolean,
        }
    ),
    key=CONF_PATH,
)


def _relative_include_path(source_path: Path) -> Path:
    try:
        return source_path.relative_to(CORE.config_dir)
    except ValueError:
        return Path(source_path.name)


@coroutine_with_priority(CoroPriority.COMPONENT)
async def to_code(config):
    source_path = config[CONF_PATH]
    include_path = _relative_include_path(source_path)
    copy_file_if_changed(source_path, CORE.relative_src_path(include_path))

    include_text = include_path.as_posix()
    if config[CONF_C_HEADER]:
        cg.add_global(
            cg.RawStatement(f'extern "C" {{\n  #include "{include_text}"\n}}')
        )
    else:
        cg.add_global(cg.RawStatement(f'#include "{include_text}"'))
