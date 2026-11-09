#!/usr/bin/env python3
"""Prepare local Steam game copies and isolated Wine/Steam bridge prefixes."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pwd
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GAMES = {"64": ("64.0", "64.0.exe", "unity-64"),
         "limbus": ("Limbus Company", "LimbusCompany.exe", "unity-limbus")}


def copy_missing(source, destination):
    """Publish complete missing subtrees atomically; preserve existing cache data."""
    if destination.exists():
        if source.is_dir() and destination.is_dir():
            for child in source.iterdir():
                copy_missing(child, destination/child.name)
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".winetk-cache-", dir=destination.parent) as staging:
        staged = Path(staging)/"data"
        subprocess.run(["cp", "-a", "--reflink=auto", "--", str(source), str(staged)], check=True)
        os.rename(staged, destination)


def import_limbus_cache(steam, prefix, refresh=False):
    marker = prefix/".winetk-limbus-cache-v1.json"
    if marker.exists() and not refresh:
        return
    original = steam/"steamapps/compatdata/1973530/pfx/drive_c/users/steamuser/AppData/LocalLow"
    local = prefix/"drive_c/users"/pwd.getpwuid(os.getuid()).pw_name/"AppData/LocalLow"
    # Unity's bundles live outside persistentDataPath. Import both bundles and
    # Addressables catalogs, never saves, account data, logs or PlayerPrefs.
    resources = ("Unity/ProjectMoon_LimbusCompany", "ProjectMoon/LimbusCompany/com.unity.addressables")
    if not (original/resources[0]).is_dir():
        print(f"No original Limbus resource cache at {original/resources[0]}")
        return
    entries = []
    for relative in resources:
        source, destination = original/relative, local/relative
        if not source.is_dir():
            continue
        copy_missing(source, destination)
        files = [p for p in source.rglob("*") if p.is_file()]
        entries.append({"source": str(source), "destination": str(destination),
                        "source_files": len(files), "source_bytes": sum(p.stat().st_size for p in files)})
    # A failed/interrupted copy leaves no marker and is safe to resume. Existing
    # entries win on refresh as the project game may already have newer content.
    with tempfile.NamedTemporaryFile(mode="w", dir=prefix, delete=False) as temp:
        temporary = Path(temp.name)
        json.dump({"policy": "copy-missing-preserve-existing", "resources": entries}, temp, indent=2)
        temp.write("\n")
    try:
        os.replace(temporary, marker)
    finally:
        temporary.unlink(missing_ok=True)
    print(f"Imported Limbus resource cache (source {sum(e['source_bytes'] for e in entries):,} bytes); "
          f"existing project entries preserved. Details: {marker}")


def prepare_game(steam, proton, source, executable, label, limbus=False, refresh_cache=False):
    source = Path(source).resolve()
    if Path(executable).is_absolute() or not (source/executable).resolve().is_relative_to(source):
        raise ValueError("executable must be inside the installed game directory")
    if not label or any(c not in "abcdefghijklmnopqrstuvwxyz0123456789-" for c in label):
        raise ValueError("invalid project prefix label")
    game = ROOT/"out"/label
    prefix, bridge = ROOT/"prefixes"/label, ROOT/"out/steam-bridge"
    wine, server = ROOT/"build/wine-gdi/wine", ROOT/"build/wine-gdi/server/wineserver"
    if not (source/executable).is_file():
        raise SystemExit(f"Missing installed game: {source/executable}")
    libraries = [(proton/"files/lib/wine"/f"{arch}-windows/lsteamclient.dll",
                  bridge/f"{arch}-windows/lsteamclient.dll") for arch in ("x86_64", "i386")]
    libraries += [(proton/"files/lib/wine/x86_64-unix/lsteamclient.so", bridge/"x86_64-unix/lsteamclient.so")]
    for path, _ in libraries:
        if not path.is_file():
            raise SystemExit(f"Missing bridge library: {path}; set WINETK_PROTON_DIR")
    prefix.parent.mkdir(parents=True, exist_ok=True)
    with Path(str(prefix)+".session.lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        if not game.exists():
            game.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(prefix=label+"-", dir=game.parent) as staging:
                subprocess.run(["cp", "-a", "--reflink=auto", str(source)+"/.", staging], check=True)
                os.rename(staging, game)
        if not (game/executable).is_file():
            raise SystemExit(f"Incomplete game copy: {game}")
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "WINETK_SOCKET")}
        env.update(WINEPREFIX=str(prefix), WINEDLLOVERRIDES="winemenubuilder.exe=d", WINEDEBUG="-all")
        # Only the project prefix is initialized/stopped, under its session lock.
        if not (prefix/".winetk-term-v1").exists():
            (ROOT/"logs").mkdir(exist_ok=True)
            with (ROOT/"logs"/(label+"-prefix.log")).open("wb") as log:
                subprocess.run([str(wine), "reg", "add", r"HKCU\Software\Wine\Drivers", "/v", "Graphics",
                                "/t", "REG_SZ", "/d", "term", "/f"], env=env, stdout=log, stderr=log,
                               timeout=90, check=True)
            (prefix/".winetk-term-v1").touch()
            subprocess.run([str(server), "-k"], env=env, check=True, timeout=10)
            subprocess.run([str(server), "-w"], env=env, check=True, timeout=10)
        if limbus:
            import_limbus_cache(steam, prefix, refresh_cache)
        hashes = {}
        for src, dest in libraries:
            dest.parent.mkdir(parents=True, exist_ok=True)
            # Atomic replacement also handles Proton's read-only library modes.
            with tempfile.NamedTemporaryFile(dir=dest.parent, delete=False) as temp:
                temporary = Path(temp.name)
            try:
                shutil.copy2(src, temporary)
                os.replace(temporary, dest)
            finally:
                temporary.unlink(missing_ok=True)
            hashes[str(src)] = hashlib.sha256(src.read_bytes()).hexdigest()
        for arch, directory, client in (("x86_64", "system32", "steamclient64.dll"),
                                         ("i386", "syswow64", "steamclient.dll")):
            for src, name in ((bridge/f"{arch}-windows/lsteamclient.dll", "lsteamclient.dll"),
                              (steam/client, client)):
                dest = prefix/"drive_c/windows"/directory/name
                dest.unlink(missing_ok=True)
                shutil.copy2(src, dest)
                hashes[str(src)] = hashlib.sha256(src.read_bytes()).hexdigest()
        host_source, host = ROOT/"src/session/steam-host.c", bridge/"steam.exe"
        if not host.exists() or host.stat().st_mtime < host_source.stat().st_mtime:
            with tempfile.NamedTemporaryFile(dir=bridge, suffix=".exe", delete=False) as temp:
                temporary = Path(temp.name)
            try:
                subprocess.run(["x86_64-w64-mingw32-gcc", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                                "-municode", "-mconsole", str(host_source), "-o", str(temporary), "-ladvapi32"], check=True)
                os.replace(temporary, host)
            finally:
                temporary.unlink(missing_ok=True)
        version = (proton/"version").read_text().strip() if (proton/"version").is_file() else "unknown"
        (bridge/"provenance.json").write_text(json.dumps({"proton": str(proton), "version": version,
                                                        "sha256": hashes}, indent=2)+"\n")
    print(f"Prepared {game} with {prefix}")
    return game, prefix, bridge


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("game", choices=GAMES)
    parser.add_argument("--refresh-cache", action="store_true",
                        help="re-import missing Limbus resource cache entries from original Proton prefix")
    args = parser.parse_args()
    steam = Path(os.environ.get("WINETK_STEAM_DIR", Path.home()/".local/share/Steam")).resolve()
    library = Path(os.environ.get("WINETK_STEAM_LIBRARY", steam)).resolve()
    proton = Path(os.environ.get("WINETK_PROTON_DIR", steam/"steamapps/common/Proton - Experimental")).resolve()
    folder, executable, label = GAMES[args.game]
    prepare_game(steam, proton, library/"steamapps/common"/folder, executable, label,
                 args.game == "limbus", args.refresh_cache)


if __name__ == "__main__":
    main()
