#!/usr/bin/env python3
"""Exercise the demo through a synthetic controlling terminal (stdlib only).

Usage: python3 tests/test_demo_pty.py /path/to/winetk-terminal-demo
This does not render graphics. It checks negotiation, fragmented input, scoped
cleanup, recording, termios restoration, and bounded failure behavior. Actual
Kitty rendering remains a separate interactive smoke test.
"""

import errno
import fcntl
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import zlib


ESC = b"\x1b"
APC = ESC + b"_G"
ST = ESC + b"\\"
LIMIT_SECONDS = 10.0
WINDOW = (24, 80, 800, 480)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def event_name(row):
    value = row.get("event", row.get("type"))
    return value if isinstance(value, str) else ""


def input_name(row):
    name = event_name(row)
    if name != "input":
        return name
    for key in ("kind", "input_type", "type"):
        if isinstance(row.get(key), str):
            return row[key]
    return ""


def read_jsonl(path):
    require(path.exists(), f"missing JSONL log: {path}")
    rows = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        require(bool(line), f"empty JSONL row {number}")
        try:
            row = json.loads(line)
        except json.JSONDecodeError as error:
            raise AssertionError(f"invalid JSONL row {number}: {error}") from error
        require(isinstance(row, dict), f"JSONL row {number} is not an object")
        rows.append(row)
    return rows


def validate_png(path):
    """Check PNG framing/CRC and decompressed scanline size, without Pillow."""
    data = path.read_bytes()
    require(data[:8] == b"\x89PNG\r\n\x1a\n", f"invalid PNG signature: {path}")
    offset = 8
    width = height = channels = None
    compressed = bytearray()
    ended = False
    while offset < len(data):
        require(offset + 12 <= len(data), f"truncated PNG chunk: {path}")
        length = struct.unpack_from("!I", data, offset)[0]
        kind = data[offset + 4:offset + 8]
        end = offset + 8 + length
        require(end + 4 <= len(data), f"truncated PNG payload: {path}")
        payload = data[offset + 8:end]
        expected_crc = struct.unpack_from("!I", data, end)[0]
        require(zlib.crc32(kind + payload) & 0xffffffff == expected_crc,
                f"bad PNG {kind!r} CRC: {path}")
        if kind == b"IHDR":
            require(offset == 8 and length == 13, f"invalid PNG IHDR: {path}")
            width, height, depth, color, compression, filtering, interlace = \
                struct.unpack("!IIBBBBB", payload)
            require(0 < width <= 16384 and 0 < height <= 16384,
                    f"invalid PNG dimensions: {path}")
            require(depth == 8 and color in (2, 6), f"expected RGB(A)8 PNG: {path}")
            require(compression == filtering == interlace == 0,
                    f"unsupported PNG encoding: {path}")
            channels = 4 if color == 6 else 3
        elif kind == b"IDAT":
            require(channels is not None, f"PNG data before IHDR: {path}")
            compressed.extend(payload)
        elif kind == b"IEND":
            require(length == 0 and end + 4 == len(data), f"invalid PNG IEND: {path}")
            ended = True
        offset = end + 4
    require(ended and compressed and channels is not None, f"incomplete PNG: {path}")
    try:
        pixels = zlib.decompress(compressed)
    except zlib.error as error:
        raise AssertionError(f"invalid compressed PNG data: {path}") from error
    stride = width * channels + 1
    require(len(pixels) == stride * height, f"wrong PNG scanline extent: {path}")
    require(all(pixels[row * stride] <= 4 for row in range(height)),
            f"invalid PNG filter byte: {path}")


class TerminalRun:
    """Consume PTY output continuously; retain only bounded diagnostic tails."""

    def __init__(self, executable, directory, scenario):
        self.directory = directory
        self.scenario = scenario
        self.master, self.slave = os.openpty()
        self.selector = selectors.DefaultSelector()
        self.tail = b""
        self.stdout = b""
        self.stderr = b""
        self.query_id = None
        self.frame_time = None
        self.frame_bytes_observed = 0
        self.first_dimensions = None
        self.resized_frame_seen = False
        self.first_placement = None
        self.resized_placement_seen = False
        self.expected_title = executable.name.encode("utf-8")
        self.title_seen = False
        self.logical_size = (800, 600) if scenario == "title_row" else (640, 360)
        self.transfer_terminated = False
        self.deferred_mode_replies = []
        self.delayed_modes_sent = False
        self.pixel_mouse_enabled = self.synchronized_enabled = False
        self.outgoing = []
        self.last_outgoing_due = 0.0
        self.resized = self.acted = False
        self.first_reply = False
        self.reply_queries = set()
        self.master_closed = False
        self.log = directory / "events.jsonl"
        self.records = directory / "records"
        window = (24, 80, 0, 0) if scenario == "cell_only" else WINDOW
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", *window))
        self.saved_termios = termios.tcgetattr(self.slave)
        os.set_blocking(self.master, False)

        def controlling_tty():
            os.setsid()
            fcntl.ioctl(self.slave, termios.TIOCSCTTY, 0)

        duration = "1.8" if scenario in ("normal", "delayed_modes", "cell_only", "title_row", "zoom_in", "fit_resolution") else "7"
        self.process = subprocess.Popen(
            [str(executable), "--duration", duration, "--fps", "5",
             "--width", str(self.logical_size[0]), "--height", str(self.logical_size[1]),
             "--log", str(self.log),
             "--record-dir", str(self.records), "--record-frames", "2"]
            + (["--zoom-in"] if scenario == "zoom_in" else [])
            + (["--fit-resolution"] if scenario == "fit_resolution" else []),
            stdin=self.slave, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            close_fds=True, pass_fds=(self.slave,), preexec_fn=controlling_tty,
            env={**os.environ, "TERM": "xterm-kitty"},
        )
        if scenario == "fit_resolution":
            self.logical_size = (800, 450)
        self.selector.register(self.master, selectors.EVENT_READ, "tty")
        for name, stream in (("stdout", self.process.stdout), ("stderr", self.process.stderr)):
            os.set_blocking(stream.fileno(), False)
            self.selector.register(stream, selectors.EVENT_READ, name)

    def enqueue(self, data, delay=0.0, fragmented=False):
        # Replies/events are complete byte streams: fragmentation must not let
        # an unrelated later response land inside an APC, CSI, or UTF-8 scalar.
        now = max(time.monotonic() + delay, self.last_outgoing_due + .002)
        if not fragmented:
            self.outgoing.append((now, data))
            self.last_outgoing_due = now
            return
        # Split UTF-8, CSI parameters, and the two-byte ST terminator as well.
        cursor = 0
        sizes = (1, 2, 5, 3)
        index = 0
        while cursor < len(data):
            count = sizes[index % len(sizes)]
            self.outgoing.append((now + index * .002, data[cursor:cursor + count]))
            self.last_outgoing_due = now + index * .002
            cursor += count
            index += 1

    def observe(self, data):
        combined = self.tail + data
        self.tail = combined[-16384:]
        self.pixel_mouse_enabled |= ESC + b"[?1016h" in combined
        self.synchronized_enabled |= ESC + b"[?2026h" in combined
        # Keep this observation separately: full RGBA transfers exceed the
        # retained diagnostic tail long before the session finishes.
        title = (rb"\x1b\[1;1H(?:\x1b\[[0-9;?]*[mK])*"
                 + re.escape(self.expected_title))
        self.title_seen |= re.search(title, combined) is not None
        now = time.monotonic()
        if self.frame_time is not None:
            self.frame_bytes_observed += len(data)
        if self.frame_time is None and (APC + b"a=t," in combined or APC + b"a=T," in combined):
            self.frame_time = now
            start = combined.find(APC + b"a=t,")
            if start < 0:
                start = combined.find(APC + b"a=T,")
            self.frame_bytes_observed = len(combined) - start
            if self.scenario == "delayed_modes":
                require(len(self.deferred_mode_replies) == 2,
                        "optional mode queries were not observed before activation")
                self.enqueue(b"".join(self.deferred_mode_replies), fragmented=True)
                self.delayed_modes_sent = True
                mouse = ESC + b"[<0;24;9M" + ESC + b"[<32;30;11M" + ESC + b"[<0;30;11m"
            else:
                mouse = (ESC + b"[<0;240;180M" + ESC + b"[<32;300;210M"
                         + ESC + b"[<0;300;210m")
            events = (
                ESC + b"[97;5:1u" + ESC + b"[97;5:3u"  # Ctrl+A press/release.
                + "终端".encode("utf-8")
                + mouse + ESC + b"[O" + ESC + b"[I"
            )
            if self.scenario in ("title_row", "zoom_in", "fit_resolution"):
                # A mouse report in the reserved text row must remain outside
                # the graphics canvas instead of reaching its top pixels.
                events = ESC + b"[<35;100;10M" + events
            if self.scenario != "sigterm_transfer":
                self.enqueue(events, .06, fragmented=True)
        if (self.scenario == "sigterm_transfer" and not self.transfer_terminated
                and self.frame_bytes_observed >= 8192):
            # Reads are capped at 4096 in this case. With a >1 MB first frame
            # and a bounded PTY queue, it cannot complete before this signal.
            require(APC + b"a=p," not in self.tail,
                    "mid-transfer signal arrived after a completed placement")
            os.kill(self.process.pid, signal.SIGTERM)
            self.transfer_terminated = self.acted = True
        if self.scenario == "timeout":
            return  # A terminal that ignores every capability query.
        for match in re.finditer(rb"\x1b_G([^;\x1b]*);", combined):
            controls = dict(item.split(b"=", 1) for item in match[1].split(b",") if b"=" in item)
            if controls.get(b"a") in (b"t", b"T") and b"s" in controls and b"v" in controls:
                dimensions = int(controls[b"s"]), int(controls[b"v"])
                if self.first_dimensions is None:
                    self.first_dimensions = dimensions
                elif self.resized and dimensions != self.first_dimensions:
                    self.resized_frame_seen = True
            if not self.first_reply:
                if controls.get(b"a") == b"q" and b"i" in controls:
                    self.query_id = int(controls[b"i"])
                    self.first_reply = True
                    self.enqueue(APC + b"i=" + str(self.query_id).encode("ascii") + b";OK" + ST,
                                 fragmented=True)
                    # The primary-DA fence follows the complete graphics reply.
                    self.enqueue(ESC + b"[?1;2c", .08)
        for match in re.finditer(rb"\x1b\[(\d+);(\d+)H\x1b_Ga=p,", combined):
            position = int(match[1]), int(match[2])
            require(position[0] >= 2,
                    f"{self.scenario}: graphics placement overlaps executable title: {position!r}")
            if self.first_placement is None:
                self.first_placement = position
            elif self.resized and position != self.first_placement:
                self.resized_placement_seen = True
        queries = (
            (ESC + b"[?u", ESC + b"[?0u"),
            (ESC + b"[14t", ESC + b"[4;480;800t"),
            (ESC + b"[16t", ESC + b"[6;20;10t"),
            (ESC + b"[?1016$p", ESC + b"[?1016;2$y"),
            (ESC + b"[?2026$p", ESC + b"[?2026;2$y"),
        )
        for query, reply in queries:
            if query in self.tail and query not in self.reply_queries:
                self.reply_queries.add(query)
                if self.scenario == "cell_only" and query == ESC + b"[14t":
                    continue  # No whole-window pixel report, only cell pixels.
                if self.scenario == "delayed_modes" and b"$p" in query:
                    self.deferred_mode_replies.append(reply)
                else:
                    # Separate replies must not interleave fragmented APC.
                    self.enqueue(reply, .10)

    def flush_outgoing(self):
        self.outgoing.sort(key=lambda item: item[0])
        while self.outgoing and self.outgoing[0][0] <= time.monotonic() and not self.master_closed:
            due, data = self.outgoing[0]
            try:
                count = os.write(self.master, data)
            except BlockingIOError:
                return
            except OSError as error:
                if error.errno in (errno.EIO, errno.EBADF):
                    self.outgoing.clear()
                    return
                raise
            if count < len(data):
                self.outgoing[0] = (due, data[count:])
                return
            self.outgoing.pop(0)

    def read_ready(self, key):
        try:
            count = 4096 if key.data == "tty" and self.scenario == "sigterm_transfer" else 65536
            data = os.read(key.fd, count)
        except BlockingIOError:
            return
        except OSError as error:
            if error.errno != errno.EIO:
                raise
            data = b""
        if not data:
            self.selector.unregister(key.fileobj)
            return
        if key.data == "tty":
            self.observe(data)
        else:
            previous = getattr(self, key.data)
            require(APC not in previous[-2:] + data,
                    f"graphics protocol leaked to {key.data}")
            setattr(self, key.data, (previous + data)[-32768:])

    def close_master(self):
        if not self.master_closed:
            try:
                self.selector.unregister(self.master)
            except KeyError:
                pass
            os.close(self.master)
            self.master_closed = True

    def run(self):
        started = time.monotonic()
        try:
            while True:
                now = time.monotonic()
                require(now - started < LIMIT_SECONDS,
                        f"{self.scenario} exceeded {LIMIT_SECONDS}s; stderr={self.stderr!r}")
                self.flush_outgoing()
                if self.frame_time is not None and self.scenario != "sigterm_transfer":
                    if not self.resized and now - self.frame_time >= .25:
                        pixels = (0, 0) if self.scenario == "cell_only" else (1000, 600)
                        fcntl.ioctl(self.slave, termios.TIOCSWINSZ,
                                    struct.pack("HHHH", 30, 100, *pixels))
                        os.kill(self.process.pid, signal.SIGWINCH)
                        self.resized = True
                        if self.scenario in ("title_row", "zoom_in", "fit_resolution"):
                            self.enqueue(ESC + b"[<35;500;300M", .10, fragmented=True)
                    if not self.acted and now - self.frame_time >= .65:
                        if self.scenario == "sigterm":
                            os.kill(self.process.pid, signal.SIGTERM)
                        elif self.scenario == "hangup":
                            self.close_master()
                        if self.scenario in ("sigterm", "hangup"):
                            self.acted = True
                if (self.scenario == "ctrl_c"
                        and (self.resized_frame_seen or self.resized_placement_seen)
                        and not self.outgoing and not self.acted):
                    # A resized frame proves SIGWINCH was handled. Previous
                    # input bytes precede this enhanced Ctrl+C press in order.
                    self.enqueue(ESC + b"[99;5:1u", fragmented=True)
                    self.acted = True
                for key, _ in self.selector.select(.015):
                    self.read_ready(key)
                result = self.process.poll()
                if result is not None:
                    # Drain the final cleanup/diagnostic bytes after waitpid.
                    for _ in range(20):
                        ready = self.selector.select(.005)
                        if not ready:
                            break
                        for key, _ in ready:
                            self.read_ready(key)
                    if self.scenario != "hangup":
                        require(termios.tcgetattr(self.slave) == self.saved_termios,
                                f"{self.scenario}: termios was not restored")
                    return result, time.monotonic() - started
        finally:
            if self.process.poll() is None:
                try:
                    os.killpg(self.process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                self.process.wait(timeout=2)
            self.close_master()
            self.selector.close()
            os.close(self.slave)
            self.process.stdout.close()
            self.process.stderr.close()


def validate_terminal_cleanup(run, result):
    expected = 0 if run.scenario in ("normal", "ctrl_c", "delayed_modes", "cell_only", "title_row", "zoom_in", "fit_resolution") else 128 + signal.SIGTERM
    require(result == expected,
            f"{run.scenario}: exit {result}, expected {expected}; stderr={run.stderr!r}")
    require(run.first_reply and run.frame_time is not None,
            f"{run.scenario}: graphics negotiation/presentation did not complete")
    require(run.title_seen,
            f"{run.scenario}: executable basename was not written at terminal cell (0,0)")
    for sequence in (
        ESC + b"[<u", ESC + b"[?1049l", ESC + b"[?2026l",
        APC + b"a=d,d=I,i=" + str(run.query_id).encode("ascii") + b",q=2" + ST,
    ):
        require(sequence in run.tail,
                f"{run.scenario}: missing cleanup sequence {sequence!r}")
    required_modes = {b"25", b"1000", b"1002", b"1003", b"1004", b"1006",
                      b"1016", b"2004", b"2026"}
    restored_modes = re.findall(rb"\x1b\[\?([0-9;]+)r", run.tail)
    require(any(required_modes <= set(modes.split(b";")) for modes in restored_modes),
            f"{run.scenario}: private terminal modes were not restored")


def validate_active_run(run, result):
    validate_terminal_cleanup(run, result)
    rows = read_jsonl(run.log)
    names = [event_name(row) for row in rows]
    for name in ("capabilities", "resize", "frame", "summary"):
        require(name in names, f"{run.scenario}: missing {name} log event: {names!r}")
    require(names.count("resize") >= 2, f"{run.scenario}: SIGWINCH did not produce resize log")
    for row in rows:
        if event_name(row) != "resize":
            continue
        cell_height = row["pixel_height"] // row["rows"]
        require(row["y"] >= cell_height,
                f"{run.scenario}: canvas overlaps title row after resize: {row!r}")
        require(row["y"] + row["height"] <= row["pixel_height"],
                f"{run.scenario}: title reservation pushed canvas below terminal: {row!r}")
    require(names[-1] == "summary", f"{run.scenario}: final log row is not summary")
    inputs = [input_name(row) for row in rows]
    for name, minimum in (("key", 2), ("text", 2), ("mouse", 3), ("focus", 2)):
        require(inputs.count(name) >= minimum,
                f"{run.scenario}: missing fragmented {name} inputs: {inputs!r}")
    if run.scenario == "ctrl_c":
        require(any(input_name(row) == "key" and row.get("codepoint") == 99
                    and row.get("action") == 1 and row.get("modifiers", 0) & 4
                    for row in rows), "safe quit did not consume enhanced Ctrl+C")
    if run.scenario == "delayed_modes":
        capabilities = next(row for row in rows if event_name(row) == "capabilities")
        require(run.delayed_modes_sent and capabilities.get("pixel_mouse") is False
                and capabilities.get("synchronized") is False,
                "late optional reports changed the negotiated capabilities")
        require(not run.pixel_mouse_enabled and not run.synchronized_enabled,
                "late optional reports enabled modes after activation")
        mouse = next(row for row in rows if input_name(row) == "mouse")
        # Cell coordinates 24,9 refer to a point well inside the canvas. The
        # same integers misread as pixels map near its upper-left edge.
        require(mouse.get("inside") is True and mouse.get("x", 0) >= 100
                and mouse.get("y", 0) >= 70,
                f"late optional report changed cell mouse mapping: {mouse!r}")
    if run.scenario == "cell_only":
        sizes = [(row.get("pixel_width"), row.get("pixel_height"))
                 for row in rows if event_name(row) == "resize"]
        require((800, 480) in sizes and (1000, 600) in sizes,
                f"cell-pixel fallback was not recomputed after SIGWINCH: {sizes!r}")
    if run.scenario in ("title_row", "zoom_in", "fit_resolution"):
        outside = next((row for row in rows if input_name(row) == "mouse"
                        and row.get("raw_x") == 100 and row.get("raw_y") == 10), None)
        require(outside is not None and outside.get("inside") is False,
                f"reserved title row was mapped inside the graphics canvas: {outside!r}")
        # Check the public coordinate contract before and after a resize. Cell
        # and pixel terminal extents must still describe the full terminal,
        # even though the graphics viewport reserves one text row.
        geometry = None
        checked = set()
        for row in rows:
            if event_name(row) == "resize":
                geometry = row
            elif input_name(row) == "mouse" and (row.get("raw_x"), row.get("raw_y")) in ((240, 180), (500, 300)):
                require(geometry is not None, "mouse event arrived before canvas geometry")
                px, py = row["raw_x"] - 1, row["raw_y"] - 1
                require(geometry["x"] <= px < geometry["x"] + geometry["width"]
                        and geometry["y"] <= py < geometry["y"] + geometry["height"],
                        f"test mouse point unexpectedly outside canvas: {row!r}")
                expected = ((px - geometry["x"]) * run.logical_size[0] // geometry["width"],
                            (py - geometry["y"]) * run.logical_size[1] // geometry["height"])
                require(row.get("inside") is True and (row["x"], row["y"]) == expected,
                        f"title row changed mouse coordinate mapping: {row!r}, expected {expected!r}")
                checked.add((geometry["pixel_width"], geometry["pixel_height"]))
        require({(800, 480), (1000, 600)} <= checked,
                f"mouse mapping was not checked on both terminal sizes: {checked!r}")
    if run.scenario in ("normal", "zoom_in", "fit_resolution"):
        geometry = [r for r in rows if event_name(r) == "resize"]
        if run.scenario in ("zoom_in", "fit_resolution"):
            require(run.first_dimensions == (800, 450),
                    f"zoom did not enlarge the transmitted frame: {run.first_dimensions!r}")
            require(any((r["width"], r["height"]) == (1000, 562) for r in geometry),
                    "zoom did not refit after terminal resize")
            for r in geometry:
                require(abs(r["width"] * 360 - r["height"] * 640) < 640,
                        f"zoom changed aspect ratio: {r!r}")
        else:
            require(all((r["width"], r["height"]) == (640, 360) for r in geometry),
                    "default mode unexpectedly enlarged the canvas")
        images = sorted(run.records.glob("frame-*.png"))
        require([path.name for path in images] == ["frame-000001.png", "frame-000002.png"],
                f"record-frames bound or naming incorrect: {images!r}")
        for image in images:
            validate_png(image)


def validate_transfer_interrupt(run, result):
    validate_terminal_cleanup(run, result)
    require(run.transfer_terminated, "no signal was delivered during the first transfer")
    require(APC + b"m=0,q=2;" + ST in run.tail,
            "interrupted multi-chunk transfer was not terminated before cleanup")
    names = [event_name(row) for row in read_jsonl(run.log)]
    require("capabilities" in names and names[-1] == "summary",
            "interrupted transfer did not produce a complete diagnostic log")
    require("frame" not in names,
            "first-frame interrupt test unexpectedly completed a frame")


def main():
    require(len(sys.argv) == 2, "usage: test_demo_pty.py /path/to/winetk-terminal-demo")
    executable = Path(sys.argv[1]).resolve()
    require(executable.is_file(), f"missing executable: {executable}")
    directory = Path(tempfile.mkdtemp(prefix="winetk-pty-test-"))
    try:
        for scenario in ("normal", "ctrl_c", "sigterm", "sigterm_transfer", "title_row", "zoom_in", "fit_resolution",
                         "delayed_modes", "cell_only", "timeout", "hangup"):
            case = directory / scenario
            case.mkdir()
            run = TerminalRun(executable, case, scenario)
            result, elapsed = run.run()
            if scenario in ("normal", "ctrl_c", "sigterm", "delayed_modes", "cell_only", "title_row", "zoom_in", "fit_resolution"):
                validate_active_run(run, result)
            elif scenario == "sigterm_transfer":
                validate_transfer_interrupt(run, result)
            else:
                require(result != 0, f"{scenario}: expected failure, got success")
                if scenario == "timeout":
                    require(run.frame_time is None, "unconfirmed graphics terminal was activated")
                    require(not re.search(rb"\x1b\[>\d+u", run.tail)
                            and ESC + b"[<u" not in run.tail,
                            "failed probe changed/popped the caller's keyboard stack")
                    require(ESC + b"[?1049h" not in run.tail and ESC + b"[?1049l" not in run.tail,
                            "failed probe changed the caller's alternate screen")
                else:
                    require(run.frame_time is not None, "hangup occurred before a frame")
            print(f"PTY {scenario}: passed ({elapsed:.2f}s)")
    finally:
        if os.environ.get("WINETK_PTY_TEST_KEEP"):
            print(f"PTY artifacts retained: {directory}")
        else:
            shutil.rmtree(directory)


if __name__ == "__main__":
    main()
