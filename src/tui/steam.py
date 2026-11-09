"""Read Steam's local library manifests and identify installed Windows PE games."""
from dataclasses import dataclass, field
import os
from pathlib import Path
import re
import struct


class VDFError(ValueError):
    pass


def parse_vdf(text):
    # Valve KeyValues, including comments, legacy unquoted keys and escaped paths.
    pattern = re.compile(r'\s+|//[^\n]*|"(?:\\.|[^"\\])*"|[{}]|[^\s{}"]+')
    tokens = []
    offset = 0
    while offset < len(text):
        match = pattern.match(text, offset)
        if not match:
            raise VDFError(f"invalid KeyValues token at {offset}")
        token = match.group()
        offset = match.end()
        if token.isspace() or token.startswith("//"):
            continue
        quoted = token.startswith('"')
        if quoted:
            token = re.sub(r'\\([\\"])', r'\1', token[1:-1])
        tokens.append((token, quoted))
    cursor = 0

    def object_value(depth=0, nested=False):
        nonlocal cursor
        if depth > 32:
            raise VDFError("KeyValues nesting is too deep")
        result = {}
        while cursor < len(tokens):
            key, quoted = tokens[cursor]
            cursor += 1
            if key == "}" and not quoted:
                if not nested:
                    raise VDFError("unexpected closing brace")
                return result
            if key == "{" and not quoted or cursor == len(tokens):
                raise VDFError("missing KeyValues key/value")
            value, quoted = tokens[cursor]
            cursor += 1
            if value == "{" and not quoted:
                value = object_value(depth+1, True)
            elif value == "}" and not quoted:
                raise VDFError("missing value before closing brace")
            result[key.casefold()] = value
        if nested:
            raise VDFError("unclosed KeyValues object")
        return result

    return object_value()


def read_vdf(path):
    with path.open("r", encoding="utf-8-sig", errors="strict") as stream:
        text = stream.read(8*1024*1024+1)
    if len(text) > 8*1024*1024:
        raise VDFError("manifest exceeds 8 MiB")
    return parse_vdf(text)


def steam_roots(extra=(), home=None):
    home = Path(home) if home else Path.home()
    candidates = [os.environ.get("WINETK_STEAM_DIR", ""), *extra,
                  home/".local/share/Steam", home/".steam/steam", home/".steam/root",
                  home/"Steam", home/".steam", home/".var/app/com.valvesoftware.Steam/.local/share/Steam"]
    if os.environ.get("XDG_DATA_HOME"):
        candidates.append(Path(os.environ["XDG_DATA_HOME"])/"Steam")
    roots = []
    for candidate in candidates:
        if not candidate:
            continue
        try:
            path = Path(candidate).expanduser().resolve()
            if path.name.casefold() == "steamapps":
                path = path.parent
            if (path/"steamapps").is_dir() and path not in roots:
                roots.append(path)
        except (OSError, RuntimeError):
            continue
    return roots


def pe_architecture(path):
    try:
        with path.open("rb") as stream:
            header = stream.read(64)
            if len(header) < 64 or header[:2] != b"MZ":
                return None
            offset = struct.unpack_from("<I", header, 60)[0]
            if offset < 64 or offset > 16*1024*1024:
                return None
            stream.seek(offset)
            header = stream.read(24)
            if len(header) != 24 or header[:4] != b"PE\0\0":
                return None
            machine, = struct.unpack_from("<H", header, 4)
            flags, = struct.unpack_from("<H", header, 22)
            if flags & 0x2000 or not flags & 0x0002:
                return None
            return {0x14c: "x86", 0x8664: "x64", 0xaa64: "arm64"}.get(machine)
    except OSError:
        return None


SKIP_DIRS = {"_commonredist", "redist", "redistributables", "directx", "dotnet", "vcredist",
             "windows", "steamworks shared", "thirdparty", "third_party", "files", "dist"}
SKIP_EXE = re.compile(r"(?:unins\d*|uninstall.*|CrashReportClient.*|CrashHandler.*|.*CrashReporter|dxsetup|setup|"
                      r"vcredist.*|vc_redist.*|dotnet.*|ue[45]prereq.*|UnityCrashHandler.*)\.exe$", re.I)


def executables(directory):
    found = []
    visited = 0
    for current, dirs, files in os.walk(directory, followlinks=False):
        relative = Path(current).relative_to(directory)
        dirs[:] = sorted(d for d in dirs if len(relative.parts) < 4 and not d.startswith(".")
                         and d.casefold() not in SKIP_DIRS and not d.casefold().endswith("_data"))
        for name in sorted(files):
            visited += 1
            if visited > 50000:
                return found
            if not name.casefold().endswith(".exe") or SKIP_EXE.fullmatch(name):
                continue
            path = Path(current)/name
            try:
                if not path.resolve().is_relative_to(directory.resolve()):
                    continue
            except (OSError, RuntimeError):
                continue
            arch = pe_architecture(path)
            if arch:
                found.append((str(path.relative_to(directory)), arch))
    return sorted(found, key=lambda entry: (len(Path(entry[0]).parts), entry[0].casefold()))


@dataclass
class Game:
    appid: str
    name: str
    directory: Path
    steam_root: Path
    library: Path
    executables: list[tuple[str, str]]

    def json(self):
        return {"appid": self.appid, "name": self.name, "directory": str(self.directory),
                "steam_root": str(self.steam_root), "library": str(self.library),
                "executables": [{"path": path, "architecture": arch} for path, arch in self.executables]}


@dataclass
class Scan:
    games: list[Game] = field(default_factory=list)
    roots: list[Path] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)


def scan(extra=(), home=None, progress=None):
    result = Scan(roots=steam_roots(extra, home))
    client_root = next((root for root in result.roots if (root/"steamclient64.dll").is_file()), None)
    seen_libraries, seen_games = set(), set()
    for root in result.roots:
        libraries = [root]
        try:
            data = read_vdf(root/"steamapps/libraryfolders.vdf").get("libraryfolders", {})
            if not isinstance(data, dict):
                raise VDFError("libraryfolders is not an object")
            for key, value in data.items():
                if key.isdigit():
                    path = value.get("path") if isinstance(value, dict) else value
                    if isinstance(path, str) and path:
                        libraries.append(Path(path).expanduser())
        except FileNotFoundError:
            pass
        except (OSError, ValueError) as error:
            result.warnings.append(f"{root}: {error}")
        for library in libraries:
            try:
                library = library.resolve()
                if library in seen_libraries:
                    continue
                seen_libraries.add(library)
                manifests = sorted((library/"steamapps").glob("appmanifest_*.acf"))
            except (OSError, RuntimeError) as error:
                result.warnings.append(f"{library}: {error}")
                continue
            for manifest in manifests:
                try:
                    data = read_vdf(manifest).get("appstate", {})
                    if not isinstance(data, dict):
                        raise VDFError("AppState is not an object")
                    appid, name, folder = (data.get(key, "") for key in ("appid", "name", "installdir"))
                    if not all(isinstance(s, str) for s in (appid, name, folder)):
                        raise VDFError("invalid app identity")
                    if not appid.isdigit() or not name or not folder or not int(data.get("stateflags", "0")) & 4:
                        continue
                    common = (library/"steamapps/common").resolve()
                    directory = (common/folder).resolve()
                    if not directory.is_relative_to(common) or not directory.is_dir():
                        continue
                    if (directory/"compatibilitytool.vdf").exists() or name.casefold().startswith("steam linux runtime "):
                        continue
                    identity = (appid, directory)
                    if identity in seen_games:
                        continue
                    seen_games.add(identity)
                    if progress:
                        progress(name)
                    choices = executables(directory)
                    if choices:
                        result.games.append(Game(appid, name, directory,
                                                 root if (root/"steamclient64.dll").is_file() else client_root or root,
                                                 library, choices))
                except (OSError, ValueError, TypeError, RuntimeError) as error:
                    result.warnings.append(f"{manifest.name}: {error}")
    result.games.sort(key=lambda game: (game.name.casefold(), game.appid))
    return result
