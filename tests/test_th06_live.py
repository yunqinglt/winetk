#!/usr/bin/env python3
"""Opt-in Touhou 6 end-to-end performance run in an independent Kitty/prefix.

Requires the user's game/ and the project's completed Wine build. This reports
terminal submissions, not display scanout or game simulation FPS. No assets are
distributed, and the original game and normal th06 prefix are left untouched.
"""

import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import struct
import tempfile
import time

from test_wine_live import ROOT, command, jsonl, require, wait_for
from test_dinput_live import kitty_key_event


def exercise(mode, output, duration, kitty_config):
    directory = output / mode
    directory.mkdir()
    prefix = ROOT / "prefixes/th06-performance"
    game = ROOT / "out/game-th06-performance"
    if not game.exists():
        command([str(ROOT / "scripts/prepare-th06.py"), "--destination", str(game)])
    config = (game / "東方紅魔郷.cfg").read_bytes()
    game_options = struct.unpack_from("<I", config, 0x34)[0]
    events = ROOT / "logs/wine-th06-performance-session.jsonl"
    offset = len(jsonl(events)) if events.exists() else 0

    def rows():
        return jsonl(events)[offset:] if events.exists() else []

    with tempfile.TemporaryDirectory(prefix="wtk-th06-perf-", dir="/tmp") as temporary:
        socket = f"unix:{temporary}/kitty.sock"

        def remote(action, *args, input=None):
            return command(["/usr/bin/kitten", "@", "--to", socket, action,
                            *map(str, args)], input=input)

        config_path = str(Path.home() / ".config/kitty/kitty.conf") if kitty_config == "user" else "NONE"
        args = ["/usr/bin/kitty", "--config", config_path, "--listen-on", socket,
                "--override", "allow_remote_control=socket-only", "--title", f"WineTK TH06 {mode}",
                str(ROOT / "scripts/run-th06.sh"), "--prefix", str(prefix),
                "--kitty-transfer", mode, "--duration", str(duration)]
        with (directory / "kitty.log").open("wb") as log:
            process = subprocess.Popen(args, cwd=ROOT, stdout=log, stderr=log, close_fds=True,
                                       env=dict(os.environ, WINETK_TH06_DIR=str(game)))
            try:
                def window_id():
                    try:
                        inventory = json.loads(remote("ls"))
                    except (RuntimeError, json.JSONDecodeError, subprocess.TimeoutExpired):
                        return None
                    # Remote ls includes environments: retain only the ID.
                    return next((window["id"] for item in inventory for tab in item.get("tabs", [])
                                 for window in tab.get("windows", [])), None)

                window = wait_for("Kitty window", window_id, timeout=25, process=process)
                match = f"id:{window}"
                remote("focus-window", "--match", match)
                wait_for("640x480 game canvas", lambda: any(row.get("event") == "wine_canvas"
                         and row.get("width") == 640 and row.get("height") == 480 for row in rows()),
                         timeout=35, process=process)
                # The first Wine frame is the animated loading splash, not
                # yet the menu. Its DirectInput acquisition comes later.
                time.sleep(8)
                remote("screenshot", "--match", match, directory / "menu.png")
                # Start -> difficulty -> character -> shot type -> stage.
                # Extra confirms allow for menu transition debounce/loading.
                for _ in range(7):
                    kitty_key_event(socket, match, 122, 1)
                    time.sleep(.15)
                    kitty_key_event(socket, match, 122, 3)
                    time.sleep(1.8)
                # A held arrow exercises the game's DirectInput state path.
                remote("screenshot", "--match", match, directory / "before-right.png")
                kitty_key_event(socket, match, 57351, 1)
                time.sleep(.11)
                kitty_key_event(socket, match, 57351, 2)
                time.sleep(.19)
                kitty_key_event(socket, match, 57351, 3)
                remote("screenshot", "--match", match, directory / "after-right.png")
                time.sleep(4)
                remote("screenshot", "--match", match, directory / "interaction.png")
                process.wait(timeout=duration+10)
                require(process.returncode == 0, f"Kitty/session failed: {process.returncode}")
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
    session = rows()
    (directory / "session.jsonl").write_text("".join(json.dumps(row)+"\n" for row in session))
    frames = [row for row in session if row.get("event") == "frame"]
    require(len(frames) > 60, f"Not enough frames: {len(frames)}")
    end = frames[-1]["time_ms"]
    frames = [row for row in frames if row["time_ms"] >= end-10000]
    intervals = sorted(b["time_ms"]-a["time_ms"] for a, b in zip(frames, frames[1:]))
    capabilities = next(row for row in session if row.get("event") == "capabilities")
    summary = next(row for row in reversed(session) if row.get("event") == "summary")
    require(summary["exit_status"] == 0, f"Session error: {summary}")
    result = {"requested_transfer": mode, "actual_transfer": capabilities.get("kitty_transfer"),
              "game_options": game_options, "frameskip": config[0x1f],
              "kitty_config": kitty_config, "key_source": "kitty-native-encoder",
              "wined3d_config": os.environ.get("WINE_D3D_CONFIG", ""),
              "sample_ms": end-frames[0]["time_ms"], "sample_frames": len(frames),
              "submission_fps": (len(frames)-1)*1000/(end-frames[0]["time_ms"]),
              "mean_present_ms": sum(row["present_ms"] for row in frames)/len(frames),
              "mean_tty_bytes_per_frame": sum(row["bytes"] for row in frames)/len(frames),
              "p95_submission_interval_ms": intervals[int((len(intervals)-1)*.95)],
              "summary": summary}
    (directory / "result.json").write_text(json.dumps(result, indent=2)+"\n")
    print(json.dumps(result), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--mode", choices=("auto", "direct", "both"), default="both")
    parser.add_argument("--kitty-config", choices=("NONE", "user"), default="NONE")
    parser.add_argument("--duration", type=int, default=45)
    args = parser.parse_args()
    require(35 <= args.duration <= 120, "Duration must be 35..120 seconds")
    output = (args.output_dir or ROOT / "logs" / ("th06-live-" +
              datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))).resolve()
    output.mkdir(parents=True, exist_ok=False)
    results = [exercise(mode, output, args.duration, args.kitty_config) for mode in
               (("direct", "auto") if args.mode == "both" else (args.mode,))]
    (output / "results.json").write_text(json.dumps(results, indent=2)+"\n")


if __name__ == "__main__":
    main()
