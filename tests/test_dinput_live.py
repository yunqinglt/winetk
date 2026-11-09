#!/usr/bin/env python3
"""Opt-in real Kitty -> Wine -> DirectInput 8 enhanced-arrow smoke test."""

import argparse
import base64
from datetime import datetime, timezone
import fcntl
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time

from test_wine_live import ROOT, command, jsonl, kill_prefix, require, status, wait_for


def kitty_key_event(address, match, codepoint, action):
    # Send a WindowSystemKeyEvent to Kitty, which then applies its actual
    # negotiated keyboard encoder. This supports separately timed holds;
    # `kitten @ send-key` always synthesizes an immediate press+release pair.
    event = f"\x1b[{codepoint};1:{action}u".encode("ascii")
    request = {"cmd": "send-text", "version": [0, 49, 2], "no_response": True,
               "payload": {"match": match, "data": "kitty-key:" + base64.b64encode(event).decode("ascii"),
                           "bracketed_paste": "disable"}}
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.connect(address.removeprefix("unix:"))
        connection.sendall(b"\x1bP@kitty-cmd" + json.dumps(request).encode("utf-8") + b"\x1b\\")


def exercise(architecture, output, prefix, wine, wineserver, kitty_config, key_source):
    directory = output / architecture
    directory.mkdir()
    application_status = directory / "status.txt"
    events = directory / "session.jsonl"
    executable = ROOT / "out/smoke" / f"dinput8-window-{architecture}.exe"
    require(executable.is_file(), "Build smoke programs first")
    kill_prefix(prefix, wineserver)
    with tempfile.TemporaryDirectory(prefix="wtk-dinput-", dir="/tmp") as temporary:
        address = f"unix:{temporary}/kitty.sock"

        def remote(action, *args, input=None):
            return command(["/usr/bin/kitty", "@", "--to", address, "--use-password", "never",
                            action, *map(str, args)], input=input)

        config = str(Path.home() / ".config/kitty/kitty.conf") if kitty_config == "user" else "NONE"
        args = ["/usr/bin/kitty", "--config", config, "--listen-on", address,
                "--override", "allow_remote_control=socket-only", "--title", "WineTK DirectInput smoke",
                str(ROOT / "build/terminal/winetk-terminal-demo"), "--fps", "60", "--duration", "35",
                "--log", str(events), "--wine", str(wine), "--wine-prefix", str(prefix),
                "--wine-log", str(directory / "application.log"), "--", str(executable),
                "--duration", "15", "--status", "Z:" + str(application_status).replace("/", "\\")]
        with (directory / "kitty.log").open("wb") as log:
            process = subprocess.Popen(args, cwd=ROOT, stdout=log, stderr=log, close_fds=True)
            try:
                def window_id():
                    try:
                        windows = json.loads(remote("ls"))
                    except (RuntimeError, json.JSONDecodeError, subprocess.TimeoutExpired):
                        return None
                    # Never save or print environments present in Kitty ls.
                    return next((window["id"] for item in windows for tab in item.get("tabs", [])
                                 for window in tab.get("windows", [])), None)

                window = wait_for("Kitty window", window_id, process=process)
                match = f"id:{window}"
                remote("focus-window", "--match", match)
                wait_for("successful DirectInput keyboard polling", lambda:
                         int(status(application_status).get("successful_polls", "0")) >= 3,
                         timeout=20, process=process)
                sources = ("kitty", "functional", "raw-csi-u") if key_source == "all" else (key_source,)
                for source in sources:
                    for name, codepoint, trailer in (("up", 57352, "A"), ("down", 57353, "B"),
                                                     ("left", 57350, "D"), ("right", 57351, "C")):
                        before = status(application_status)
                        old_count = int(before.get(f"{name}_count", "0"))
                        old_samples = int(before.get(f"{name}_samples", "0"))

                        def send(action):
                            if source == "kitty":
                                kitty_key_event(address, match, codepoint, action)
                            else:
                                encoded = (f"\x1b[{codepoint};1:{action}u" if source == "raw-csi-u" else
                                           f"\x1b[{trailer}" if action == 1 else f"\x1b[1;1:{action}{trailer}")
                                remote("send-text", "--match", match, "--stdin", input=encoded)

                        send(1)
                        time.sleep(.11)
                        wait_for(f"{source} DirectInput {name} held", lambda: (
                            int(status(application_status).get(f"{name}_samples", "0")) >= old_samples + 3
                            and status(application_status).get(f"{name}_held") == "1"),
                            timeout=2, process=process)
                        send(2)
                        time.sleep(.14)
                        send(3)
                        wait_for(f"{source} DirectInput {name} down and release", lambda: (
                            int(status(application_status).get(f"{name}_count", "0")) == old_count + 1
                            and int(status(application_status).get(f"{name}_samples", "0")) >= old_samples + 3
                            and status(application_status).get(f"{name}_held") == "0"), process=process)
                remote("screenshot", "--match", match, directory / "kitty.png")
                wait_for("DirectInput application exit", lambda:
                         status(application_status).get("phase") == "complete", timeout=18)
                wait_for("terminal summary", lambda: any(row.get("event") == "summary"
                         for row in jsonl(events)), timeout=4)
                process.wait(timeout=4)
                require(process.returncode == 0, f"Kitty failed: {process.returncode}")
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=4)
                kill_prefix(prefix, wineserver)
    final = status(application_status)
    for name in ("numpad8", "numpad2", "numpad4", "numpad6"):
        require(final.get(f"{name}_count") == "0", f"Arrow incorrectly generated {name}: {final}")
    require(final.get("phase") == "complete", f"Incomplete DirectInput run: {final}")
    final["kitty_config"] = kitty_config
    final["key_sources"] = list(sources)
    (directory / "result.json").write_text(json.dumps(final, indent=2) + "\n")
    print(f"DIRECTINPUT {architecture}: config={kitty_config} sources={','.join(sources)}; arrows held/released, no numpad events", flush=True)
    return final


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--arch", choices=("i386", "x86_64", "both"), default="both")
    parser.add_argument("--kitty-config", choices=("NONE", "user"), default="NONE")
    parser.add_argument("--key-source", choices=("all", "kitty", "functional", "raw-csi-u"), default="all")
    args = parser.parse_args()
    output = (args.output_dir or ROOT / "logs" / ("dinput-live-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))).resolve()
    output.mkdir(parents=True, exist_ok=False)
    prefix = ROOT / "prefixes/gdi"
    with (ROOT / "prefixes/gdi.session.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        results = [exercise(architecture, output, prefix, ROOT / "build/wine-gdi/wine",
                            ROOT / "build/wine-gdi/server/wineserver", args.kitty_config, args.key_source) for architecture in
                   (("i386", "x86_64") if args.arch == "both" else (args.arch,))]
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    main()
