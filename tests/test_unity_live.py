#!/usr/bin/env python3
"""Opt-in installed Unity games -> isolated Wine -> real Kitty acceptance run.

Requires Linux Steam signed in and the two locally installed games. Captures
screenshots, Player logs, terminal submissions and Wine connect syscalls. Does
not sign in to Limbus, download game content, or claim gameplay acceptance.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import time

from PIL import Image, ImageChops, ImageStat
from test_wine_live import ROOT, command, jsonl, require, wait_for
from test_dinput_live import kitty_key_event


def exercise(game, output, duration, zoom_in=False, fit_resolution=False):
    label = "unity-64" if game == "64" else "unity-limbus"
    expected = (640, 480) if game == "64" else (800, 450)
    name = "64.0.exe" if game == "64" else "LimbusCompany.exe"
    directory = output/game
    directory.mkdir()
    session = ROOT/"logs"/f"wine-{label}-session.jsonl"
    application = ROOT/"logs"/f"wine-{label}-application.log"
    offset = len(jsonl(session))
    app_offset = application.stat().st_size if application.exists() else 0

    def rows():
        return jsonl(session)[offset:]

    with tempfile.TemporaryDirectory(prefix="wtk-unity-", dir="/tmp") as temporary:
        address = f"unix:{temporary}/kitty.sock"
        wrapper = Path(temporary)/"wine-trace.sh"
        trace = directory/"wine-connect.log"
        wrapper.write_text("#!/bin/sh\nexec "+" ".join(shlex.quote(str(x)) for x in
                           ("/usr/bin/strace", "-D", "-f", "-e", "trace=connect", "-o", trace,
                            ROOT/"build/wine-gdi/wine"))+' "$@"\n')
        wrapper.chmod(0o700)

        def remote(action, *args, input=None):
            return command(["/usr/bin/kitten", "@", "--to", address, action, *map(str,args)], input=input)

        args = ["/usr/bin/kitty", "--config", "NONE", "--listen-on", address,
                "--override", "allow_remote_control=socket-only", "--override", "remember_window_size=no",
                "--override", "initial_window_width=1150", "--override", "initial_window_height=850",
                "--title", f"WineTK Unity {game} acceptance", str(ROOT/"scripts/run-unity.sh"),
                game, "--duration", str(duration)]
        if zoom_in:
            args.append("--zoom-in")
        if fit_resolution:
            args.append("--fit-resolution")
        env = dict(os.environ, WINETK_WINE_LOADER=str(wrapper), WINEDEBUG="-all", WINETK_GL_METRICS="1")
        with (directory/"kitty.log").open("wb") as log:
            process = subprocess.Popen(args, cwd=ROOT, env=env, stdout=log, stderr=log, close_fds=True)
            try:
                def window_id():
                    try:
                        return next((w["id"] for item in json.loads(remote("ls")) for t in item.get("tabs",[])
                                     for w in t.get("windows",[])), None)
                    except (RuntimeError, json.JSONDecodeError, subprocess.TimeoutExpired):
                        return None
                window = wait_for("Kitty window",window_id,timeout=20,process=process)
                match = f"id:{window}"
                remote("focus-window","--match",match)
                if fit_resolution:
                    planned = wait_for("render resolution",lambda: next((r for r in rows()
                                       if r.get("event")=="render_resolution"),None),timeout=20,process=process)
                    expected = planned["width"],planned["height"]
                wait_for("Unity GPU canvas",lambda: any(r.get("event")=="wine_canvas" and
                         (r.get("width"),r.get("height"))==expected for r in rows()),
                         timeout=35,process=process)
                time.sleep(10 if game=="64" else 18)
                remote("screenshot","--match",match,directory/"before-input.png")
                require(remote("get-text","--match",match).splitlines()[0].startswith(name),
                        "executable title is the Steam helper")
                if game=="64":
                    kitty_key_event(address,match,13,1)
                    time.sleep(.18)
                    kitty_key_event(address,match,13,3)
                    time.sleep(2)
                    remote("screenshot","--match",match,directory/"menu.png")
                    kitty_key_event(address,match,13,1)
                    time.sleep(.18)
                    kitty_key_event(address,match,13,3)
                    time.sleep(2)
                    kitty_key_event(address,match,57351,1)
                    time.sleep(.25)
                    kitty_key_event(address,match,57351,3)
                else:
                    geometry = [r for r in rows() if r.get("event")=="resize"][-1]
                    x = geometry["x"]+geometry["width"]//2+1
                    y = geometry["y"]+geometry["height"]*3//4+1
                    remote("send-text","--match",match,"--stdin",input=f"\x1b[<35;{x};{y}M")
                    remote("send-text","--match",match,"--stdin",input=f"\x1b[<0;{x};{y}M")
                    time.sleep(.16)
                    remote("send-text","--match",match,"--stdin",input=f"\x1b[<0;{x};{y}m")
                    time.sleep(1)
                    remote("send-key","--match",match,"escape")
                time.sleep(3)
                remote("screenshot","--match",match,directory/"after-input.png")
                process.wait(timeout=duration+15)
                require(process.returncode==0,f"Unity terminal session exited {process.returncode}")
            finally:
                if process.poll() is None:
                    # Closing Kitty's child shell runs run-wine's prefix cleanup.
                    process.terminate()
                    process.wait(timeout=10)
    captured = rows()
    (directory/"session.jsonl").write_text("".join(json.dumps(r)+"\n" for r in captured))
    (directory/"application.log").write_bytes(application.read_bytes()[app_offset:])
    player = ROOT/"logs"/f"{label}-Player.log"
    (directory/"Player.log").write_bytes(player.read_bytes())
    text = player.read_text(errors="replace")
    require("Forcing GfxDevice: Direct3D 11" in text and "<RI> Input initialized." in text,
            "Unity did not initialize D3D11/input")
    require("SteamAPI_Init() failed" not in text,"Steam bridge did not initialize")
    trace_text = trace.read_text(errors="replace")
    require("display.sock" in trace_text,"Wine IPC was not traced")
    require(not re.search(r"(?:\.X11-unix|wayland-[0-9]|/tmp/\.XIM-unix)",trace_text),
            "Wine attempted a host display connection")
    frames = [r for r in captured if r.get("event")=="frame"]
    geometry = [r for r in captured if r.get("event")=="resize"][-1]
    if fit_resolution:
        require((geometry.get("source_width"),geometry.get("source_height"))==expected,
                "Unity did not render at the planned terminal resolution")
        require((geometry["width"],geometry["height"])==expected,
                "expected a native-pixel fit at this terminal size")
    if zoom_in or fit_resolution:
        require(geometry.get("zoom_in") is True and geometry["width"]>=expected[0],
                "terminal did not enlarge the Unity canvas")
        require(abs(geometry["width"]*expected[1]-geometry["height"]*expected[0])<max(expected),
                "zoom changed Unity aspect ratio")
        require(geometry["y"]>=geometry["pixel_height"]//geometry["rows"] and
                geometry["y"]+geometry["height"]<=geometry["pixel_height"],
                "zoom overlaps the title or terminal boundary")
    require(len(frames)>200,"too few terminal frames")
    end = frames[-1]["time_ms"]
    sample = [r for r in frames if r["time_ms"]>=end-10000]
    before, after = (Image.open(directory/f"{phase}-input.png").convert("RGB") for phase in ("before","after"))
    difference = sum(ImageStat.Stat(ImageChops.difference(before,after)).mean)
    # Account state can change the response from a large login dialog to the
    # small CONNECTING label. Compare inside the canvas; letterboxing must not
    # dilute the response just because the terminal is tall.
    box = (geometry["x"],geometry["y"],geometry["x"]+geometry["width"],geometry["y"]+geometry["height"])
    canvas_difference = sum(ImageStat.Stat(ImageChops.difference(before.crop(box),after.crop(box))).mean)
    require(canvas_difference>1,"no visible response inside the game canvas after input")
    result = {"game":game,"api":"D3D11 / WineD3D / surfaceless EGL","canvas":expected,
              "viewport":geometry,"frames":len(frames),"last_10s_submission_fps":(len(sample)-1)*1000/(end-sample[0]["time_ms"]),
              "screenshot_mean_difference":difference,"canvas_mean_difference":canvas_difference,"no_x11_wayland_connect":True,
              "summary":next(r for r in reversed(captured) if r.get("event")=="summary")}
    (directory/"result.json").write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(result),flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game",choices=("64","limbus","both"),default="both")
    parser.add_argument("--output",type=Path)
    parser.add_argument("--duration",type=int,default=45)
    parser.add_argument("--zoom-in",action="store_true")
    parser.add_argument("--fit-resolution",action="store_true")
    args = parser.parse_args()
    require(40<=args.duration<=120,"duration must be 40..120 seconds")
    output = (args.output or ROOT/"logs"/("unity-live-"+datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))).resolve()
    output.mkdir(parents=True,exist_ok=False)
    results = [exercise(game,output,args.duration,args.zoom_in,args.fit_resolution) for game in (("64","limbus") if args.game=="both" else (args.game,))]
    (output/"results.json").write_text(json.dumps(results,indent=2)+"\n")


if __name__=="__main__":
    main()
