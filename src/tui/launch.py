"""Build argument vectors; never execute Steam metadata as shell text."""
import os
from pathlib import Path
import sys
from .config import terminal_options

ROOT = Path(__file__).resolve().parents[2]
PRESETS = {"578850": ("64", "64.0", "64.0.exe"),
           "1973530": ("limbus", "Limbus Company", "LimbusCompany.exe")}


def plan(game, executable, settings):
    if (executable, "arm64") in game.executables:
        raise ValueError("the current Wine build supports x86/x64 games only")
    if executable not in {path for path, _ in game.executables}:
        raise ValueError("executable is not part of the scanned game")
    env = dict(os.environ, WINETK_STEAM_DIR=str(game.steam_root), WINETK_STEAM_LIBRARY=str(game.library))
    if settings.proton_dir:
        env["WINETK_PROTON_DIR"] = str(Path(settings.proton_dir).expanduser().resolve())
    preset = PRESETS.get(game.appid)
    if preset and game.directory.name == preset[1] and executable == preset[2]:
        command = [str(ROOT/"scripts/run-unity.sh"), preset[0], *terminal_options(settings)]
    else:
        command = [sys.executable, str(ROOT/"scripts/run-steam.py"), "--appid", game.appid,
                   "--directory", str(game.directory), "--executable", executable,
                   "--", *terminal_options(settings)]
    return command, env
