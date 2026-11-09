"""Validated, versioned JSON settings with atomic replacement."""
from dataclasses import asdict, dataclass, field
import json
import os
from pathlib import Path
import tempfile


class ConfigError(ValueError):
    pass


@dataclass
class Settings:
    version: int = 1
    fps: int = 60
    scaling: str = "fit-resolution"
    kitty_transfer: str = "auto"
    steam_roots: list[str] = field(default_factory=list)
    proton_dir: str = ""


def config_path():
    base = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home()/".config").expanduser()
    return base/"winetk/config.json"


def validate(data):
    if not isinstance(data, dict):
        raise ConfigError("configuration must be a JSON object")
    unknown = data.keys() - asdict(Settings()).keys()
    if unknown:
        raise ConfigError("unknown settings: " + ", ".join(sorted(unknown)))
    value = {**asdict(Settings()), **data}
    if type(value["version"]) is not int or value["version"] != 1:
        raise ConfigError("unsupported configuration version (expected 1)")
    if type(value["fps"]) is not int or not 1 <= value["fps"] <= 240:
        raise ConfigError("fps must be an integer from 1 to 240")
    if value["scaling"] not in ("native", "zoom-in", "fit-resolution"):
        raise ConfigError("scaling must be native, zoom-in or fit-resolution")
    if value["kitty_transfer"] not in ("auto", "direct"):
        raise ConfigError("kitty_transfer must be auto or direct")
    roots = value["steam_roots"]
    if not isinstance(roots, list) or len(roots) > 32 or any(
            not isinstance(p, str) or not p.strip() or any(ord(c) < 32 for c in p) for p in roots):
        raise ConfigError("steam_roots must contain at most 32 nonempty directory paths")
    if not isinstance(value["proton_dir"], str) or any(ord(c) < 32 for c in value["proton_dir"]):
        raise ConfigError("proton_dir must be a directory path or an empty string")
    return Settings(**value)


def load(path):
    try:
        with Path(path).open("r", encoding="utf-8") as stream:
            raw = stream.read(65537)
        if len(raw) > 65536:
            raise ConfigError("configuration exceeds 64 KiB")
        return validate(json.loads(raw))
    except FileNotFoundError:
        return Settings()
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ConfigError(f"cannot read {path}: {error}") from error


def save(path, settings):
    checked = validate(asdict(settings))
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                         prefix=".config-", delete=False) as stream:
            temporary = Path(stream.name)
            os.chmod(temporary, 0o600)
            json.dump(asdict(checked), stream, ensure_ascii=False, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def terminal_options(settings):
    result = ["--fps", str(settings.fps), "--kitty-transfer", settings.kitty_transfer]
    if settings.scaling != "native":
        result.append("--" + settings.scaling)
    return result
