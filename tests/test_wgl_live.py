#!/usr/bin/env python3
"""Opt-in real Wine WGL/D3D8/D3D11 surfaceless EGL test, without a terminal or host display.

Build Wine and the smoke PEs first. This creates/reuses a dedicated project
prefix, captures the actual driver IPC, and checks native RGBA colors/origin.
"""

import argparse
from datetime import datetime, timezone
import fcntl
import json
import os
from pathlib import Path
import selectors
import socket
import struct
import subprocess
import tempfile
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
HEADER = struct.Struct("<4I")
WINDOW = struct.Struct("<QIiiIIIiiII")
MAGIC = 0x4B544957
VERSION = 2


def require(value, message):
    if not value:
        raise RuntimeError(message)


def write_png(path, width, height, pixels):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    scanlines = b"".join(b"\0" + pixels[y * width * 4:(y + 1) * width * 4] for y in range(height))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" +
                     chunk(b"IHDR", struct.pack(">2I5B", width, height, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))


def clean_prefix(prefix, env):
    subprocess.run([str(ROOT / "build/wine-gdi/server/wineserver"), "-k"], env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
    subprocess.run([str(ROOT / "build/wine-gdi/server/wineserver"), "-w"], env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)


def capture(architecture, output, prefix, env, api="wgl"):
    directory = output / architecture
    directory.mkdir()
    wine = ROOT / "build/wine-gdi/wine"
    executable = ROOT / "out/smoke" / f"{api}-window-{architecture}.exe"
    require(executable.is_file(), f"missing {executable}; run scripts/build-smoke.sh")
    clean_prefix(prefix, env)
    frames = []
    latest = None
    latest_size = None
    sizes = {}
    windows = set()
    with tempfile.TemporaryDirectory(prefix="wtk-wgl-", dir="/tmp") as temporary:
        address = str(Path(temporary) / "broker.sock")
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(address)
        server.listen()
        server.setblocking(False)
        selector = selectors.DefaultSelector()
        selector.register(server, selectors.EVENT_READ)
        clients = {}
        started = time.monotonic()
        with (directory / "application.log").open("wb") as log:
            command = [str(wine), str(executable)]
            if api == "d3d8":
                command += ["--duration", "8", "--status", "Z:" + str(directory / "status.json").replace("/", "\\")]
            process = subprocess.Popen(command, env={**env, "WINETK_SOCKET": address},
                                       stdout=log, stderr=log, close_fds=True)
            try:
                while time.monotonic() - started < 40:
                    for key, _ in selector.select(.05):
                        if key.fileobj is server:
                            client, _ = server.accept()
                            client.setblocking(False)
                            clients[client] = bytearray()
                            selector.register(client, selectors.EVENT_READ)
                            continue
                        client = key.fileobj
                        data = client.recv(2 * 1024 * 1024)
                        if not data:
                            selector.unregister(client)
                            del clients[client]
                            client.close()
                            continue
                        buffer = clients[client]
                        buffer.extend(data)
                        while len(buffer) >= HEADER.size:
                            magic, version, kind, size = HEADER.unpack_from(buffer)
                            require(magic == MAGIC and version == VERSION, "unexpected driver protocol")
                            require(size <= 48 + 1920 * 1080 * 4, "oversized driver frame")
                            if len(buffer) < HEADER.size + size:
                                break
                            payload = bytes(buffer[HEADER.size:HEADER.size + size])
                            del buffer[:HEADER.size + size]
                            if kind != 1:
                                continue
                            require(len(payload) >= WINDOW.size, "short frame metadata")
                            hwnd, pid, x, y, width, height, flags, cx, cy, cw, ch = WINDOW.unpack_from(payload)
                            pixels = payload[WINDOW.size:]
                            require(len(pixels) == width * height * 4, "bad RGBA frame length")
                            if (width, height) not in ((640, 480), (800, 600)):
                                continue
                            # Ignore GDI's initial blank client before WGL creates its drawable.
                            if api == "wgl":
                                red_pos = ((height // 8) * width + width // 8) * 4
                                blue_pos = ((height * 7 // 8) * width + width * 7 // 8) * 4
                                red = pixels[red_pos:red_pos + 4]
                                blue = pixels[blue_pos:blue_pos + 4]
                                if abs(red[0] - 230) > 2 or abs(blue[2] - 204) > 2:
                                    continue
                                require(all(abs(a - b) <= 2 for a, b in zip(red, (230, 26, 51, 255))),
                                        f"wrong top-left RGBA {list(red)}")
                                require(all(abs(a - b) <= 2 for a, b in zip(blue, (26, 51, 204, 255))),
                                        f"wrong lower-right RGBA {list(blue)}")
                            else:
                                if api == "d3d8" and (width, height) != (640, 480):
                                    continue
                                background = pixels[(50 * width + 50) * 4:(50 * width + 50) * 4 + 4]
                                center_pos = ((height // 2) * width + width // 2) * 4
                                center = pixels[center_pos:center_pos + 4]
                                if not all(abs(a - b) <= 2 for a, b in zip(background, (12, 24, 48, 255))):
                                    continue
                                if api == "d3d11":
                                    require(all(abs(a-b)<=2 for a,b in zip(center,(204,153,102,255))),
                                            f"wrong D3D11 shader RGBA {list(center)}")
                                require(all(channel >= 60 for channel in center[:3]) and center[3] == 255,
                                        f"missing {api} triangle pixels at center: {list(center)}")
                            require((cw, ch) == (width, height), "client dimensions include decoration")
                            require((x, y) == (cx, cy), "GPU frame origin differs from client origin")
                            windows.add((pid, hwnd))
                            frames.append(time.monotonic())
                            sizes[(width, height)] = sizes.get((width, height), 0) + 1
                            latest = pixels
                            latest_size = (width, height)
                    if process.poll() is not None:
                        break
                require(process.poll() is not None, f"{api} smoke did not exit within 40 seconds")
                require(process.returncode == 0, f"{api} smoke exited {process.returncode}; inspect application.log")
            finally:
                clean_prefix(prefix, env)
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)
                for client in clients:
                    client.close()
                server.close()
                selector.close()
    require(len(frames) >= 60, f"only {len(frames)} correctly colored GPU frames")
    expected_sizes = ((640, 480), (800, 600)) if api in ("wgl", "d3d11") else ((640, 480),)
    require(all(sizes.get(size, 0) >= 60 for size in expected_sizes),
            f"resized drawable missing correct pixels: {sizes}")
    span = frames[-1] - frames[0]
    rate = (len(frames) - 1) / span
    require(rate >= 20, f"GPU frame cadence only {rate:.1f} fps")
    text = (directory / "application.log").read_text(errors="replace")
    if api == "wgl":
        require("GL_RENDERER=" in text and "frames=" in text, "missing actual OpenGL renderer/cadence report")
    elif api == "d3d11":
        require("D3D11CreateDeviceAndSwapChain=0x00000000" in text and "exit=0" in text,
                "missing successful D3D11 device/present report")
    else:
        require("CreateDevice=0x00000000" in text and "D3D8_SMOKE frames=" in text,
                "missing actual D3D8 device/present report")
        status = json.loads((directory / "status.json").read_text())
        require(status["exit"] == 0 and status["frames"] >= 60, "D3D8 present loop failed")
    write_png(directory / "frame.png", *latest_size, latest)
    result = {"api": api, "architecture": architecture, "sizes": [{"width": w, "height": h, "frames": n} for (w, h), n in sizes.items()],
              "matching_frames": len(frames), "capture_fps": round(rate, 2), "windows": len(windows)}
    (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result), flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--prefix", type=Path, default=ROOT / "prefixes/gpu-test")
    parser.add_argument("--api", choices=("wgl", "d3d8", "d3d11"), default="wgl")
    args = parser.parse_args()
    output = (args.output or ROOT / "logs" / ("wgl-live-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))).resolve()
    output.mkdir(parents=True, exist_ok=False)
    prefix = args.prefix.resolve()
    prefix.parent.mkdir(parents=True, exist_ok=True)
    env = {key: value for key, value in os.environ.items() if key not in ("DISPLAY", "WAYLAND_DISPLAY", "WINETK_SOCKET")}
    env.update(WINEPREFIX=str(prefix), WINEDLLOVERRIDES="winemenubuilder.exe=d",
               WINEDEBUG=os.environ.get("WINETK_TEST_WINEDEBUG", "-all"), WINETK_FPS="120")
    with prefix.with_suffix(".session.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        clean_prefix(prefix, env)
        with (output / "prefix.log").open("wb") as log:
            result = subprocess.run([str(ROOT / "build/wine-gdi/wine"), "reg", "add", r"HKCU\Software\Wine\Drivers",
                                     "/v", "Graphics", "/t", "REG_SZ", "/d", "term", "/f"],
                                    env=env, stdout=log, stderr=log, timeout=60, close_fds=True)
            require(result.returncode == 0, "failed to initialize project prefix; inspect prefix.log")
        clean_prefix(prefix, env)
        # Upstream Wine builds its legacy D3D8 implementation for i386 only.
        architectures = ("i386",) if args.api == "d3d8" else ("x86_64", "i386")
        results = [capture(architecture, output, prefix, env, args.api) for architecture in architectures]
    (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"Verified real surfaceless {args.api} output: {output}")


if __name__ == "__main__":
    main()
