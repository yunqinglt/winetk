#!/usr/bin/env python3
"""Launch a scanned Windows Steam game using a project copy and Steam bridge."""
import argparse
import importlib.util
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--appid", required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--executable", required=True)
    parser.add_argument("options", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not args.appid.isdigit():
        parser.error("appid must be numeric")
    steam = Path(os.environ["WINETK_STEAM_DIR"]).resolve()
    proton = Path(os.environ.get("WINETK_PROTON_DIR", steam/"steamapps/common/Proton - Experimental")).resolve()
    spec = importlib.util.spec_from_file_location("prepare_unity", ROOT/"scripts/prepare-unity.py")
    prepare = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(prepare)
    game, prefix, bridge = prepare.prepare_game(steam, proton, args.directory, args.executable, "steam-"+args.appid)
    executable = game/args.executable
    unity = executable.with_name(executable.stem+"_Data").is_dir()
    options = args.options[1:] if args.options[:1] == ["--"] else args.options
    env = dict(os.environ, WINETK_STEAM_BRIDGE="1", SteamAppId=args.appid, SteamGameId=args.appid,
               SteamClientLaunch="1", SteamEnv="1",
               WINEDLLPATH=str(bridge)+(":"+os.environ["WINEDLLPATH"] if os.environ.get("WINEDLLPATH") else ""))
    command = [str(ROOT/"scripts/run-wine.sh"), "--prefix", str(prefix), "--cwd", str(executable.parent),
               "--title", executable.name, *(["--width", "800", "--height", "450"] if unity else []),
               *options, "--", str(bridge/"steam.exe"), "Z:"+str(executable).replace("/", "\\")]
    if unity:
        width, height = (("@WINETK_WIDTH@", "@WINETK_HEIGHT@") if "--fit-resolution" in options else ("800", "450"))
        command += ["-force-d3d11", "-screen-fullscreen", "0", "-screen-width", width, "-screen-height", height,
                    "-logFile", "Z:"+str(ROOT/"logs"/("steam-"+args.appid+"-Player.log")).replace("/", "\\")]
    os.execve(command[0], command, env)


if __name__ == "__main__":
    main()
