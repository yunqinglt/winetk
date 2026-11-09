"""Small curses TUI: common layout, keyboard navigation and validated forms."""
from dataclasses import asdict
import curses
import json
import unicodedata
from .config import ConfigError, Settings, save, validate
from .steam import scan


def clean(text):
    return "".join(c if unicodedata.category(c)[0] != "C" else " " for c in str(text))


def clipped(text, width):
    result, columns = [], 0
    for char in clean(text):
        count = 0 if unicodedata.combining(char) else 2 if unicodedata.east_asian_width(char) in "WF" else 1
        if columns + count > width:
            break
        result.append(char)
        columns += count
    return "".join(result)


class Application:
    def __init__(self, settings, path):
        self.settings, self.path = settings, path
        self.status = ""

    def initialize(self, screen):
        screen.keypad(True)
        curses.set_escdelay(25)
        curses.noecho()
        try:
            curses.curs_set(0)
        except curses.error:
            pass

    def put(self, screen, y, x, text, style=0):
        rows, cols = screen.getmaxyx()
        if y < 0 or y >= rows or x < 0 or x >= cols-1:
            return
        try:
            screen.addstr(y, x, clipped(text, cols-x-1), style)
        except curses.error:
            pass

    def frame(self, screen, title, footer):
        screen.erase()
        rows, cols = screen.getmaxyx()
        self.put(screen, 0, 1, "WineTK | "+title, curses.A_BOLD)
        if rows < 14 or cols < 48:
            self.put(screen, 2, 0, "Resize terminal to at least 48 x 14. Q/Esc: Back")
            screen.refresh()
            return False
        self.put(screen, 1, 1, "Config: "+str(self.path), curses.A_DIM)
        self.put(screen, rows-3, 1, self.status)
        self.put(screen, rows-1, 1, footer, curses.A_REVERSE)
        return True

    def edit(self, screen, label, value):
        text = str(value)
        while True:
            okay = self.frame(screen, "Edit "+label, "Enter: Apply   Esc: Cancel   Ctrl+U: Clear")
            if okay:
                self.put(screen, 4, 2, label, curses.A_BOLD)
                self.put(screen, 6, 2, text or "(empty)", curses.A_REVERSE)
                self.put(screen, 8, 2, "Text is applied to the form; S saves the configuration.", curses.A_DIM)
            screen.refresh()
            key = screen.get_wch()
            if key == "\x1b":
                return None
            if not okay:
                continue
            if key in ("\n", "\r", curses.KEY_ENTER):
                return text
            if key in (curses.KEY_BACKSPACE, "\x7f", "\b"):
                text = text[:-1]
            elif key == "\x15":
                text = ""
            elif isinstance(key, str) and key.isprintable() and len(text) < 8192:
                text += key

    def settings_page(self, screen):
        self.initialize(screen)
        draft = Settings(**asdict(self.settings))
        selected = 0
        fields = (("fps", "Submission FPS (1..240)"), ("scaling", "Scaling"),
                  ("kitty_transfer", "Kitty transfer"), ("steam_roots", "Extra Steam roots (JSON array)"),
                  ("proton_dir", "Proton directory (empty: auto)"))
        choices = {"scaling": ("native", "zoom-in", "fit-resolution"), "kitty_transfer": ("auto", "direct")}
        self.status = "Edit settings, then S to save. Esc discards unsaved edits."
        while True:
            dirty = asdict(draft) != asdict(self.settings)
            okay = self.frame(screen, "Settings"+(" *" if dirty else ""),
                              "Up/Down: Select   Enter: Edit   Left/Right: Change   S: Save   Esc: Back")
            if okay:
                rows, _ = screen.getmaxyx()
                visible = max(1, rows-8)
                start = max(0, selected-visible+1)
                for index, (name, label) in enumerate(fields[start:start+visible], start):
                    value = getattr(draft, name)
                    value = json.dumps(value, ensure_ascii=False) if isinstance(value, list) else value
                    style = curses.A_REVERSE if index == selected else 0
                    self.put(screen, 3+index-start, 2, f"{label}: {value}", style)
                if rows >= 18:
                    self.put(screen, rows-6, 2, "native: keep source pixels | zoom-in: smooth enlargement", curses.A_DIM)
                    self.put(screen, rows-5, 2, "fit-resolution: Unity renders to startup terminal size (max 1920x1080)", curses.A_DIM)
            screen.refresh()
            key = screen.get_wch()
            if key in ("q", "Q", "\x1b"):
                return
            if not okay:
                continue
            if key in (curses.KEY_UP, "k"):
                selected = (selected-1) % len(fields)
            elif key in (curses.KEY_DOWN, "j", "\t"):
                selected = (selected+1) % len(fields)
            elif key in ("s", "S", "\x13"):
                try:
                    save(self.path, draft)
                    self.settings = Settings(**asdict(draft))
                    self.status = "Saved configuration: "+str(self.path)
                except (OSError, ConfigError) as error:
                    self.status = "Save failed: "+str(error)
            elif key in ("\n", "\r", curses.KEY_ENTER, curses.KEY_LEFT, curses.KEY_RIGHT, " "):
                name, label = fields[selected]
                if name in choices:
                    values = choices[name]
                    step = -1 if key == curses.KEY_LEFT else 1
                    setattr(draft, name, values[(values.index(getattr(draft, name))+step) % len(values)])
                    continue
                if key in (curses.KEY_LEFT, curses.KEY_RIGHT, " "):
                    continue
                current = getattr(draft, name)
                value = self.edit(screen, label, json.dumps(current, ensure_ascii=False) if name == "steam_roots" else current)
                if value is not None:
                    try:
                        updated = asdict(draft)
                        updated[name] = int(value) if name == "fps" else json.loads(value) if name == "steam_roots" else value
                        draft = validate(updated)
                        self.status = "Changes pending. S to save."
                    except (ValueError, ConfigError) as error:
                        self.status = "Invalid value: "+str(error)

    def details(self, screen, game):
        selected = 0
        while True:
            okay = self.frame(screen, game.name, "Up/Down: Executable   Enter: Launch   Esc: Back")
            if okay:
                rows, _ = screen.getmaxyx()
                self.put(screen, 3, 2, "Steam AppID: "+game.appid)
                self.put(screen, 4, 2, "Installed: "+str(game.directory), curses.A_DIM)
                self.put(screen, 5, 2, "Launch uses an isolated project copy / Wine prefix.", curses.A_DIM)
                visible = max(1, rows-11)
                start = max(0, selected-visible+1)
                for i, (executable, arch) in enumerate(game.executables[start:start+visible], start):
                    self.put(screen, 7+i-start, 2, f"[{arch}] {executable}", curses.A_REVERSE if i == selected else 0)
            screen.refresh()
            key = screen.get_wch()
            if key in ("q", "Q", "\x1b"):
                return None
            if not okay:
                continue
            if key in (curses.KEY_UP, "k"):
                selected = (selected-1) % len(game.executables)
            elif key in (curses.KEY_DOWN, "j"):
                selected = (selected+1) % len(game.executables)
            elif key in ("\n", "\r", curses.KEY_ENTER):
                return game.executables[selected][0]

    def steam_page(self, screen):
        self.initialize(screen)
        games, selected, query, refresh = [], 0, "", True
        notice = self.status
        while True:
            if refresh:
                def progress(name):
                    self.frame(screen, "Steam | Scanning", "Reading installed game manifests...")
                    self.put(screen, 4, 2, name)
                    screen.refresh()
                scanned = scan(self.settings.steam_roots, progress=progress)
                games, refresh = scanned.games, False
                self.status = notice or (f"{len(games)} Windows games, {len(scanned.roots)} Steam roots. "
                               + (scanned.warnings[0] if scanned.warnings else ""))
                notice = ""
            filtered = [g for g in games if query.casefold() in g.name.casefold() or query in g.appid]
            selected = max(0, min(selected, len(filtered)-1))
            okay = self.frame(screen, "Steam | Windows games", "Up/Down: Select   Enter: Details   /: Search   R: Rescan   S: Settings   Q: Exit")
            if okay:
                rows, _ = screen.getmaxyx()
                self.put(screen, 3, 2, "Search: "+(query or "(all)")+f"   {len(filtered)} games", curses.A_DIM)
                visible = max(1, rows-9)
                start = max(0, selected-visible+1)
                for index, game in enumerate(filtered[start:start+visible], start):
                    arches = "/".join(sorted({arch for _, arch in game.executables}))
                    self.put(screen, 5+index-start, 2, f"{game.name}  [{arches}]  #{game.appid}",
                             curses.A_REVERSE if selected == index else 0)
                if not filtered:
                    self.put(screen, 5, 2, "No installed Windows games found.")
                    self.put(screen, 7, 2, "R rescans. S opens settings for additional Steam directories.", curses.A_DIM)
            screen.refresh()
            key = screen.get_wch()
            if key in ("q", "Q", "\x1b"):
                return None
            if not okay:
                continue
            if key in (curses.KEY_UP, "k"):
                selected = max(0, selected-1)
            elif key in (curses.KEY_DOWN, "j"):
                selected = min(len(filtered)-1, selected+1)
            elif key == curses.KEY_NPAGE:
                selected += visible
            elif key == curses.KEY_PPAGE:
                selected -= visible
            elif key in ("r", "R"):
                refresh = True
            elif key in ("s", "S"):
                self.settings_page(screen)
                refresh = True
            elif key == "/":
                edited = self.edit(screen, "Search games", query)
                if edited is not None:
                    query, selected = edited, 0
            elif key in ("\n", "\r", curses.KEY_ENTER) and filtered:
                game = filtered[selected]
                executable = self.details(screen, game)
                if executable:
                    return game, executable
