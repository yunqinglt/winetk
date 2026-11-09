#!/usr/bin/env python3
"""Launcher discovery/config tests and real curses interaction through a PTY."""
from dataclasses import asdict
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/"src"))
from tui.config import ConfigError, Settings, load, save, terminal_options, validate
from tui.launch import plan
from tui.steam import Game, VDFError, parse_vdf, scan


def pe(path, machine=0x8664):
    path.parent.mkdir(parents=True, exist_ok=True)
    header = bytearray(88)
    header[:2] = b"MZ"
    struct.pack_into("<I", header, 60, 64)
    header[64:68] = b"PE\0\0"
    struct.pack_into("<H", header, 68, machine)
    struct.pack_into("<H", header, 86, 0x0002)
    path.write_bytes(header)


def manifest(library, appid, folder, name=None, flags=4):
    apps = library/"steamapps"
    apps.mkdir(parents=True, exist_ok=True)
    path = apps/f"appmanifest_{appid}.acf"
    path.write_text('"AppState" { '+" ".join(json.dumps(k)+" "+json.dumps(v) for k, v in
                    {"appid": str(appid), "name": name or folder, "installdir": folder,
                     "StateFlags": str(flags)}.items())+' }\n')
    return path


class Terminal:
    def __init__(self, arguments, env):
        self.master, self.slave = os.openpty()
        self.resize(24, 100, notify=False)
        self.saved = termios.tcgetattr(self.slave)
        self.output = b""

        def controlling():
            os.setsid()
            fcntl.ioctl(self.slave, termios.TIOCSCTTY, 0)

        self.process = subprocess.Popen([str(ROOT/"winetk"), *map(str, arguments)], cwd=ROOT,
                                        stdin=self.slave, stdout=self.slave, stderr=subprocess.PIPE,
                                        preexec_fn=controlling, env={**env, "TERM": "xterm-256color"})

    def resize(self, rows, columns, notify=True):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, columns*10, rows*20))
        if notify:
            os.kill(self.process.pid, signal.SIGWINCH)

    def send(self, data):
        os.write(self.master, data)

    def wait(self, text=None, condition=None):
        deadline = time.monotonic()+5
        while time.monotonic() < deadline:
            if select.select([self.master], [], [], .03)[0]:
                self.output += os.read(self.master, 65536)
            if text is not None and text in self.output or condition is not None and condition():
                return
            if self.process.poll() is not None:
                raise AssertionError(f"TUI exited early: {self.process.stderr.read()!r}")
        raise AssertionError(f"TUI timeout waiting for {text!r}; tail={self.output[-1000:]!r}")

    def close(self):
        try:
            if self.process.poll() is None:
                self.send(b"q")
            self.process.wait(timeout=3)
            if self.process.returncode != 0:
                raise AssertionError(f"TUI exit {self.process.returncode}: {self.process.stderr.read()!r}")
            if termios.tcgetattr(self.slave) != self.saved:
                raise AssertionError("curses did not restore terminal attributes")
        finally:
            if self.process.poll() is None:
                self.process.kill()
                self.process.wait(timeout=2)
            os.close(self.master)
            os.close(self.slave)
            self.process.stderr.close()


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="winetk-tui-test-")
        self.addCleanup(self.temporary.cleanup)
        self.home = Path(self.temporary.name)
        self.steam = self.home/".local/share/Steam"
        self.path = self.home/"config/config.json"
        self.env = {**os.environ, "HOME": str(self.home), "XDG_CONFIG_HOME": str(self.home/"config"),
                    "XDG_DATA_HOME": str(self.home/".local/share"), "WINETK_STEAM_DIR": ""}

    def test_config_roundtrip_defaults_and_permissions(self):
        self.assertEqual(load(self.path), Settings())
        self.assertFalse(self.path.parent.exists())
        settings = Settings(fps=144, scaling="zoom-in", steam_roots=["~/Steam library"], proton_dir="~/Proton")
        save(self.path, settings)
        self.assertEqual(load(self.path), settings)
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(terminal_options(settings), ["--fps", "144", "--kitty-transfer", "auto", "--zoom-in"])

    def test_config_validation_and_atomic_failure(self):
        for invalid in ({"fps": True}, {"fps": 241}, {"fps": "60"}, {"version": 2}, {"scaling": "stretch"},
                        {"kitty_transfer": "socket"}, {"steam_roots": "~/Steam"}, {"proton_dir": None}, {"unknown": 1}):
            with self.subTest(invalid=invalid), self.assertRaises(ConfigError):
                validate(invalid)
        save(self.path, Settings(fps=30))
        original = self.path.read_bytes()
        with patch("tui.config.os.replace", side_effect=OSError("disk failure")):
            with self.assertRaises(OSError):
                save(self.path, Settings(fps=120))
        self.assertEqual(self.path.read_bytes(), original)
        self.assertFalse(list(self.path.parent.glob(".config-*")))
        self.path.write_text('{"fps":')
        with self.assertRaises(ConfigError):
            load(self.path)

    def test_keyvalues_grammar(self):
        data = parse_vdf('// a comment\n"LibraryFolders" { "0" "/mnt/Steam" "1" { "path" "C:\\\\Games" } }')
        self.assertEqual(data["libraryfolders"]["1"]["path"], r"C:\Games")
        self.assertEqual(parse_vdf('key "a \\"quote\\""')["key"], 'a "quote"')
        for text in ('"key" {', 'key', '}', '"unterminated'):
            with self.subTest(text=text), self.assertRaises(VDFError):
                parse_vdf(text)

    def make_library(self):
        self.steam.joinpath("steamapps").mkdir(parents=True)
        external = self.home/"External Steam library"
        external.joinpath("steamapps").mkdir(parents=True)
        (self.steam/"steamapps/libraryfolders.vdf").write_text('"libraryfolders" { "0" { "path" '+json.dumps(str(self.steam))+
                ' } "1" { "path" '+json.dumps(str(external))+' } }')
        return external

    def test_scan_external_libraries_filters_and_deduplicates(self):
        external = self.make_library()
        manifest(self.steam, 10, "Windows Game")
        pe(self.steam/"steamapps/common/Windows Game/Game.exe", 0x14c)
        pe(self.steam/"steamapps/common/Windows Game/UnityCrashHandler64.exe")
        manifest(external, 20, "Candy Crash")
        pe(external/"steamapps/common/Candy Crash/Binaries/Win64/CandyCrash.EXE")
        manifest(self.steam, 30, "Linux Game")
        native = self.steam/"steamapps/common/Linux Game"
        native.mkdir(parents=True)
        (native/"Game.x86_64").write_bytes(b"\x7fELF")
        pe(native/"_CommonRedist/setup.exe")
        manifest(self.steam, 40, "Not installed", flags=0)
        pe(self.steam/"steamapps/common/Not installed/Game.exe")
        manifest(self.steam, 50, "A compatibility tool")
        tool = self.steam/"steamapps/common/A compatibility tool"
        pe(tool/"tool.exe")
        (tool/"compatibilitytool.vdf").touch()
        alias = self.home/"Steam"
        alias.symlink_to(self.steam, target_is_directory=True)
        with patch.dict(os.environ, self.env, clear=True):
            result = scan([str(external), str(alias)], home=self.home)
        self.assertEqual([game.appid for game in result.games], ["20", "10"])
        self.assertEqual(result.games[1].executables, [("Game.exe", "x86")])
        self.assertFalse(result.warnings)

    def test_bad_manifest_does_not_hide_valid_game_or_escape_install(self):
        self.make_library()
        pe(self.steam/"steamapps/common/Good/Game.exe")
        manifest(self.steam, 10, "Good")
        (self.steam/"steamapps/appmanifest_99.acf").write_text('"AppState" { broken')
        manifest(self.steam, 11, "../../outside")
        pe(self.steam/"outside/Outside.exe")
        pe(self.home/"Outside.exe")
        (self.steam/"steamapps/common/Good/Linked.exe").symlink_to(self.home/"Outside.exe")
        with patch.dict(os.environ, self.env, clear=True):
            result = scan(home=self.home)
        self.assertEqual([game.appid for game in result.games], ["10"])
        self.assertEqual(len(result.games[0].executables), 1)
        self.assertEqual(len(result.warnings), 1)

    def test_launch_plan_presets_and_literal_arguments(self):
        game = Game("1973530", "Limbus", self.home/"Limbus Company", self.steam,
                    self.home/"Other library", [("LimbusCompany.exe", "x64")])
        command, env = plan(game, "LimbusCompany.exe", Settings(fps=90))
        self.assertEqual(command[0:2], [str(ROOT/"scripts/run-unity.sh"), "limbus"])
        self.assertIn("--fit-resolution", command)
        self.assertEqual(env["WINETK_STEAM_LIBRARY"], str(game.library))
        game.appid = "42"
        game.executables = [('Game $(touch unwanted).exe', 'x64')]
        command, env = plan(game, game.executables[0][0], Settings())
        self.assertIn('Game $(touch unwanted).exe', command)
        self.assertIn(str(ROOT/"scripts/run-steam.py"), command)
        with self.assertRaises(ValueError):
            plan(game, "outside.exe", Settings())

    def test_generic_unity_launch_uses_bridge_copy_and_configured_options(self):
        spec = importlib.util.spec_from_file_location("run_steam_test", ROOT/"scripts/run-steam.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        game, prefix, bridge = self.home/"project game", self.home/"prefix", self.home/"bridge"
        (game/"Game_Data").mkdir(parents=True)
        prepared = SimpleNamespace(prepare_game=lambda *args: (game, prefix, bridge))
        loader = SimpleNamespace(exec_module=lambda module: setattr(module, "prepare_game", prepared.prepare_game))
        stub = SimpleNamespace(loader=loader)
        with patch.dict(os.environ, {**self.env, "WINETK_STEAM_DIR": str(self.steam)}, clear=True), \
             patch.object(sys, "argv", ["run-steam.py", "--appid", "42", "--directory", str(self.home/"installed"),
                                        "--executable", "Game.exe", "--", "--fps", "90", "--fit-resolution"]), \
             patch.object(module.importlib.util, "spec_from_file_location", return_value=stub), \
             patch.object(module.importlib.util, "module_from_spec", return_value=SimpleNamespace()), \
             patch.object(module.os, "execve") as execute:
            module.main()
        executable, argv, env = execute.call_args.args
        self.assertEqual(executable, str(ROOT/"scripts/run-wine.sh"))
        self.assertIn(str(bridge/"steam.exe"), argv)
        self.assertIn("@WINETK_WIDTH@", argv)
        self.assertEqual(argv[argv.index("--fps")+1], "90")
        self.assertEqual(env["SteamAppId"], "42")
        self.assertEqual(env["WINETK_STEAM_BRIDGE"], "1")

    def test_cli_help_readonly_and_headless_modes(self):
        def run(*args):
            return subprocess.run([str(ROOT/"winetk"), *map(str,args)], env=self.env, capture_output=True, text=True)
        result = run()
        self.assertEqual(result.returncode, 0)
        self.assertIn("--settings", result.stdout)
        self.assertFalse(self.path.parent.exists())
        result = run("--steam", "--list")
        self.assertEqual(json.loads(result.stdout)["games"], [])
        self.assertNotEqual(run("--settings").returncode, 0)
        self.assertNotEqual(run("--list").returncode, 0)

    def test_settings_pty_save_reload_discard_and_resize(self):
        terminal = Terminal(["--settings", "--config", self.path], self.env)
        try:
            terminal.wait(b"Submission FPS")
            terminal.send(b"\n")
            terminal.wait(b"Ctrl+U")
            terminal.send(b"\x1525\n")
            terminal.wait(b"Changes pending")
            terminal.send(b"j s")  # Scaling: fit-resolution -> native; then save.
            terminal.wait(condition=lambda: self.path.exists() and load(self.path).fps == 25)
            self.assertEqual(load(self.path).scaling, "native")
            terminal.resize(10, 30)
            terminal.wait(b"Resize terminal")
            terminal.resize(24, 100)
            terminal.send(b"q")
        finally:
            terminal.close()
        terminal = Terminal(["--settings", "--config", self.path], self.env)
        try:
            terminal.wait(b"Submission FPS")
            self.assertIn(b"25", terminal.output)
            terminal.send(b"\n")
            terminal.wait(b"Ctrl+U")
            terminal.send(b"\x1599\n")
            terminal.wait(b"Changes pending")
            terminal.send(b"\x1b")
        finally:
            terminal.close()
        self.assertEqual(load(self.path).fps, 25)

    def test_steam_pty_list_details_and_exit(self):
        self.make_library()
        manifest(self.steam, 42, "Fixture Windows")
        pe(self.steam/"steamapps/common/Fixture Windows/Game.exe")
        terminal = Terminal(["--steam", "--config", self.path], self.env)
        try:
            terminal.wait(b"Fixture Windows")
            terminal.send(b"\n")
            terminal.wait(b"Enter: Launch")
            terminal.wait(b"Game.exe")
            terminal.send(b"\x1b")
            time.sleep(.08)
            terminal.send(b"q")
        finally:
            terminal.close()
        self.assertFalse(self.path.exists())


if __name__ == "__main__":
    unittest.main()
