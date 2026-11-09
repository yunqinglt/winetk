"""Public WineTK command line; no arguments prints help without opening a TTY."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
from .config import ConfigError, config_path, load, terminal_options
from .launch import ROOT, plan
from .steam import scan


def main(argv=None):
    parser = argparse.ArgumentParser(prog="winetk", description="WineTK: Windows applications in a Kitty terminal.",
                                     epilog="Steam games use project copies/prefixes. Keep Linux Steam signed in.")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--steam", action="store_true", help="browse installed Windows Steam games")
    modes.add_argument("--settings", action="store_true", help="edit and save local settings in the TUI")
    modes.add_argument("--demo", action="store_true", help="run the graphics demo using saved settings")
    modes.add_argument("--run", type=Path, metavar="EXE", help="run a Windows executable using saved settings")
    parser.add_argument("--config", type=Path, default=config_path(), metavar="PATH", help="local JSON configuration path")
    parser.add_argument("--list", action="store_true", help="with --steam: print scanned games as JSON without a TUI")
    parser.add_argument("arguments", nargs=argparse.REMAINDER, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)
    if args.list and not args.steam:
        parser.error("--list requires --steam")
    if not (args.steam or args.settings or args.demo or args.run):
        parser.print_help()
        return 0
    if args.arguments and not (args.demo or args.run):
        parser.error("additional arguments require --demo or --run")
    try:
        settings = load(args.config.expanduser())
        if args.list:
            result = scan(settings.steam_roots)
            print(json.dumps({"games": [g.json() for g in result.games], "roots": list(map(str, result.roots)),
                              "warnings": result.warnings}, ensure_ascii=False, indent=2))
            return 0
        extra = args.arguments[1:] if args.arguments[:1] == ["--"] else args.arguments
        if args.demo or args.run:
            if args.run:
                command = [str(ROOT/"scripts/run-wine.sh"), *terminal_options(settings), "--", str(args.run.expanduser().resolve()), *extra]
            else:
                command = [str(ROOT/"scripts/run-terminal-demo.sh"), *terminal_options(settings), *extra]
            return subprocess.call(command, cwd=ROOT)
        if not sys.stdin.isatty() or not sys.stdout.isatty():
            parser.error("the TUI needs an interactive terminal; use --steam --list for JSON output")
        import curses
        from .ui import Application
        def interface(page):
            try:
                return curses.wrapper(page)
            except curses.error as error:
                raise ConfigError(f"terminal interface unavailable: {error}") from error
        app = Application(settings, args.config.expanduser().resolve())
        if args.settings:
            interface(app.settings_page)
            return 0
        while True:
            selected = interface(app.steam_page)
            if not selected:
                return 0
            try:
                command, env = plan(*selected, app.settings)
                status = subprocess.call(command, cwd=ROOT, env=env)
                app.status = f"Game exited with status {status}."
            except (OSError, ValueError) as error:
                app.status = "Launch failed: "+str(error)
    except (ConfigError, OSError) as error:
        print(f"winetk: {error}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 130
