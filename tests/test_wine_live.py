#!/usr/bin/env python3
"""Opt-in real Kitty/Wine GDI+input smoke; never registered with CTest.

Requires the built project Wine, initialized project prefix and a local graphical
session for Kitty. The Windows processes themselves have no X11/Wayland display.
"""

import argparse
from datetime import datetime, timezone
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def command(args, *, input=None, timeout=4):
    result = subprocess.run(args, input=input, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=timeout, close_fds=True)
    require(result.returncode == 0,
            f"command {Path(args[0]).name} {args[1]} failed: {result.stderr.strip()[:800]}")
    return result.stdout


def jsonl(path):
    if not path.exists():
        return []
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            pass  # A concurrently written final row can be incomplete.
    return rows


def status(path):
    if not path.exists():
        return {}
    return dict(line.split("=", 1) for line in path.read_text().splitlines() if "=" in line)


def wait_for(description, predicate, *, timeout=6, process=None):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        if process is not None:
            require(process.poll() is None, f"Kitty exited before {description}; inspect kitty.log")
        time.sleep(.05)
    raise RuntimeError(f"timed out waiting for {description}")


def kill_prefix(prefix, wineserver):
    # Caller owns the same exclusive prefix lock as scripts/run-wine.sh.
    subprocess.run([str(wineserver), "-k"],
                   env={**os.environ, "WINEPREFIX": str(prefix)},
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=4, close_fds=True)
    subprocess.run([str(wineserver), "-w"],
                   env={**os.environ, "WINEPREFIX": str(prefix)},
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=4, close_fds=True)


def exercise(architecture, output, kitty, strace, terminal, wine, prefix, wineserver):
    client_width, client_height = (960, 720) if architecture == "x86_64" else (640, 400)
    directory = output / architecture
    directory.mkdir()
    events = directory / "session.jsonl"
    app_log = directory / "application.log"
    app_status = directory / "status.txt"
    trace = directory / "wine-connect.log"
    executable = ROOT / "out/smoke" / f"gdi-window-{architecture}.exe"
    require(executable.is_file(), f"missing {executable}; run scripts/build-smoke.sh")
    kill_prefix(prefix, wineserver)
    with tempfile.TemporaryDirectory(prefix="wtk-live-", dir="/tmp") as temporary:
        runtime = Path(temporary)
        socket = f"unix:{runtime / 'kitty.sock'}"
        wrapper = runtime / "wine-trace.sh"
        wrapper.write_text("#!/bin/sh\nexec " + " ".join(shlex.quote(str(value)) for value in
                           (strace, "-D", "-f", "-e", "trace=connect", "-o", trace, wine)) + ' "$@"\n')
        wrapper.chmod(0o700)

        def remote(action, *args, input=None):
            return command([str(kitty), "@", "--to", socket, "--use-password", "never",
                            action, *map(str, args)], input=input)

        # /usr/bin/kitty avoids user wrappers; no inherited remote-control socket
        # is used. The prefix lock descriptor is close-on-exec and not passed.
        argv = [str(kitty), "--config", "NONE", "--listen-on", socket,
                "--override", "allow_remote_control=socket-only",
                "--override", "remember_window_size=no",
                "--override", "initial_window_width=1100",
                "--override", "initial_window_height=820",
                "--override", "background=#2e3440",
                "--override", "font_size=12",
                "--title", f"WineTk live smoke {architecture}",
                str(terminal), "--width", "800", "--height", "600", "--fps", "60",
                "--duration", "35", "--log", str(events), "--wine", str(wrapper),
                "--wine-prefix", str(prefix), "--wine-log", str(app_log), "--",
                str(executable), "--duration", "8", "--width", str(client_width),
                "--height", str(client_height), "--status",
                "Z:" + str(app_status).replace("/", "\\")]
        with (directory / "kitty.log").open("wb") as log:
            process = subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=log,
                                       close_fds=True, start_new_session=True)
            try:
                def window_id():
                    try:
                        windows = json.loads(remote("ls"))
                    except (RuntimeError, json.JSONDecodeError, subprocess.TimeoutExpired):
                        return None
                    # Deliberately extract only the numeric ID. Kitty's full ls
                    # response contains process environments and is never saved.
                    for os_window in windows:
                        for tab in os_window.get("tabs", []):
                            for window in tab.get("windows", []):
                                return window["id"]
                    return None

                window = wait_for("private Kitty window", window_id, process=process)
                match = f"id:{window}"
                remote("focus-window", "--match", match)
                wait_for("real Wine GDI frames", lambda: (
                    int(status(app_status).get("paint_count", "0")) >= 2
                    and any(row.get("event") == "wine_update" and row.get("windows", 0) > 0
                            for row in jsonl(events))), timeout=20, process=process)
                wait_for("terminal frame submission", lambda: any(
                    row.get("event") == "frame" for row in jsonl(events)), process=process)
                canvas = wait_for("reported client-sized Wine canvas", lambda: next((
                    row for row in reversed(jsonl(events)) if row.get("event") == "wine_canvas"
                    and row.get("width") == client_width and row.get("height") == client_height), None),
                    process=process)
                caps = next(row for row in jsonl(events) if row.get("event") == "capabilities")
                require(caps.get("pixel_mouse") is True, "test requires Kitty pixel mouse reporting")
                # Keyboard delivery is checked before any synthetic mouse click.
                remote("send-key", "--match", match, "a", "shift+b", "right")
                wait_for("keyboard before first mouse click", lambda: (
                    int(status(app_status).get("key_down_count", "0")) >= 3
                    and int(status(app_status).get("char_count", "0")) >= 2), process=process)
                remote("send-text", "--match", match, "--stdin", "--bracketed-paste", "enable", input="中文")
                wait_for("Chinese bracketed paste", lambda: (
                    status(app_status).get("last_char") == "U+6587"
                    and int(status(app_status).get("char_count", "0")) >= 4), process=process)
                geometry = [row for row in jsonl(events) if row.get("event") == "resize"][-1]
                require(geometry["y"] >= geometry["pixel_height"] // geometry["rows"],
                        "Wine image overlaps the native executable title row")
                x = geometry["x"] + 200 * geometry["width"] // canvas["width"] + 1
                y = geometry["y"] + 200 * geometry["height"] // canvas["height"] + 1
                mouse = "".join(f"\x1b[<{button};{x};{y}{ending}" for button, ending in
                                ((35, "M"), (0, "M"), (0, "m"), (2, "M"), (2, "m"), (64, "M")))
                remote("send-text", "--match", match, "--stdin", input=mouse)
                wait_for("left/right mouse clicks and wheel", lambda: (
                    int(status(app_status).get("left_click_count", "0")) >= 1
                    and int(status(app_status).get("right_click_count", "0")) >= 1
                    and int(status(app_status).get("wheel_delta", "0")) == 120), process=process)
                mouse_frame = max(row.get("number", 0) for row in jsonl(events)
                                  if row.get("event") == "frame")
                wait_for("visible mouse counter repaint", lambda: any(
                    row.get("event") == "frame" and row.get("number", 0) >= mouse_frame + 2
                    for row in jsonl(events)), process=process)
                screen_text = remote("get-text", "--match", match)
                require(screen_text.splitlines()[0].startswith(executable.name),
                        "native terminal row zero does not contain the executable basename")
                remote("screenshot", "--match", match, directory / "kitty.png")
                # The GDI application paints these uniform client regions. A
                # nonclient titlebar shifts the header sample, while clipping
                # to the old virtual desktop loses the large-window corner.
                image = Image.open(directory / "kitty.png").convert("RGB")
                require(image.size == (geometry["pixel_width"], geometry["pixel_height"]),
                        "Kitty screenshot dimensions differ from reported terminal pixels")
                for label, source_x, source_y, expected in (
                    ("client header", 8, 8, (24, 40, 58)),
                    ("client body", 8, 72, (16, 29, 37)),
                    ("unclipped lower-right client corner", client_width - 8, client_height - 8,
                     (24, 40, 58)),
                ):
                    sample = (geometry["x"] + source_x * geometry["width"] // canvas["width"],
                              geometry["y"] + source_y * geometry["height"] // canvas["height"])
                    actual = image.getpixel(sample)
                    require(all(abs(a - b) <= 2 for a, b in zip(actual, expected)),
                            f"{label} pixel {sample}: expected {expected}, got {actual}")
                wait_for("application duration shutdown", lambda: status(app_status).get("phase") == "complete",
                         timeout=10, process=process)
                wait_for("terminal session summary", lambda: any(
                    row.get("event") == "summary" for row in jsonl(events)), timeout=3)
                process.wait(timeout=4)
                require(process.returncode == 0, f"Kitty exited with {process.returncode}")
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
                kill_prefix(prefix, wineserver)

    final = status(app_status)
    require(final.get("phase") == "complete" and final.get("architecture") == architecture,
            f"incomplete/wrong-architecture application status: {final}")
    for key, minimum in (("paint_count", 2), ("timer_ticks", 2), ("key_down_count", 3),
                         ("char_count", 4), ("mouse_move_count", 1),
                         ("left_click_count", 1), ("right_click_count", 1)):
        require(int(final.get(key, "0")) >= minimum, f"insufficient {key}: {final.get(key)}")
    require(int(final.get("wheel_delta", "0")) == 120, "wheel event was not delivered")
    require((int(final.get("client_width", "0")), int(final.get("client_height", "0"))) ==
            (client_width, client_height), "Win32 client dimensions differ from the requested size")
    summary = [row for row in jsonl(events) if row.get("event") == "summary"][-1]
    require(summary.get("exit_status") == 0, f"terminal session failed: {summary}")
    trace_text = trace.read_text(encoding="utf-8", errors="replace")
    require("display.sock" in trace_text, "strace did not capture the terminal driver connection")
    forbidden = [line for line in trace_text.splitlines()
                 if re.search(r"(?:\.X11-unix|wayland-[0-9]|/tmp/\.XIM-unix)", line)]
    require(not forbidden, f"Wine attempted a desktop display connection: {forbidden[:3]}")
    screenshot = directory / "kitty.png"
    require(screenshot.read_bytes().startswith(b"\x89PNG\r\n\x1a\n"), "invalid Kitty screenshot")
    result = {"architecture": architecture, "status": final, "session_summary": summary,
              "keyboard_before_click": True, "native_title_row": True,
              "client_canvas": [client_width, client_height], "nonclient_hidden": True,
              "unclipped_client_pixels": True,
              "no_x11_wayland_connect": True, "screenshot": str(screenshot)}
    (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"LIVE {architecture}: passed; screenshot {screenshot}", flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, help="new result directory (default logs/wine-live-TIMESTAMP)")
    parser.add_argument("--arch", choices=("both", "x86_64", "i386"), default="both")
    parser.add_argument("--kitty", type=Path, default=Path("/usr/bin/kitty"))
    args = parser.parse_args()
    output = (args.output_dir or ROOT / "logs" / (
        "wine-live-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))).resolve()
    terminal = ROOT / "build/terminal/winetk-terminal-demo"
    wine = ROOT / "build/wine-gdi/wine"
    wineserver = ROOT / "build/wine-gdi/server/wineserver"
    prefix = ROOT / "prefixes/gdi"
    for executable in (args.kitty, terminal, wine, wineserver):
        require(os.access(executable, os.X_OK), f"missing executable: {executable}")
    require((prefix / ".winetk-term-v1").is_file(),
            "project prefix is not initialized; run scripts/run-wine.sh once first")
    strace = shutil.which("strace")
    require(strace is not None, "strace is required for actual Wine display-connection checks")
    lock = os.open(ROOT / "prefixes/gdi.session.lock", os.O_WRONLY | os.O_CREAT | os.O_CLOEXEC, 0o600)
    try:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise RuntimeError("project Wine prefix is already in use by another terminal session") from None
        output.mkdir(parents=True, exist_ok=False)
        print(f"Live test artifacts: {output}", flush=True)
        results = [exercise(arch, output, args.kitty, strace, terminal, wine, prefix, wineserver)
                   for arch in (("x86_64", "i386") if args.arch == "both" else (args.arch,))]
        (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    finally:
        os.close(lock)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        print(f"LIVE FAILED: {error}", file=sys.stderr)
        sys.exit(1)
