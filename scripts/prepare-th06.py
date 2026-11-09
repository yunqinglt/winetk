#!/usr/bin/env python3
"""Prepare a private Touhou 6 test copy; never modify the supplied game tree."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import tempfile


ROOT = Path(__file__).resolve().parent.parent
EXECUTABLE = "東方紅魔郷.exe"
CONFIG = "東方紅魔郷.cfg"
ARCHIVES = tuple(f"紅魔郷{part}.DAT" for part in ("CM", "ED", "IN", "MD", "ST", "TL"))
REQUIRED = (EXECUTABLE, CONFIG, *ARCHIVES)
MARKER = ".winetk-th06.json"


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def target_name(name: str) -> str:
    """Only repair names expected by the executable, using reversible conversion."""
    if name in REQUIRED:
        return name
    try:
        repaired = name.encode("gbk").decode("cp932")
    except UnicodeError:
        return name
    if repaired in REQUIRED and repaired.encode("cp932").decode("gbk") == name:
        return repaired
    return name


def prepare(source: Path, destination: Path, no_directinput: bool, mute: bool) -> dict:
    if no_directinput:
        raise ValueError(
            "--no-directinput is unsupported for the supplied Touhou 6 1.02f: "
            "config bit 11 is only honored by 1.02g and later; leave DirectInput enabled"
        )
    if not source.is_dir():
        raise ValueError(f"Game directory does not exist: {source}")
    if destination == source or source in destination.parents or destination in source.parents:
        raise ValueError("Source and destination must be separate, non-overlapping directories")
    if destination.exists():
        raise ValueError(f"Destination already exists; choose a fresh --destination: {destination}")
    for path in source.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"Refusing to copy a symlink in the supplied game tree: {path}")
        if not path.is_file() and not path.is_dir():
            raise ValueError(f"Refusing non-regular game entry: {path}")
    names = {target_name(path.name): path for path in source.iterdir()}
    missing = [name for name in REQUIRED if name not in names]
    if missing:
        raise ValueError("Missing Touhou 6 files: " + ", ".join(missing))
    if len(names) != len(list(source.iterdir())):
        raise ValueError("Repaired filenames would collide")
    original_config = names[CONFIG].read_bytes()
    if len(original_config) != 0x38 or struct.unpack_from("<I", original_config, 0x14)[0] != 0x102:
        raise ValueError("Unsupported Touhou 6 config; expected 56 bytes and version 0x102")
    modified_config = bytearray(original_config)
    modified_config[0x1E] = 1  # GameConfiguration.windowed
    modified_config[0x1F] = 0  # GameConfiguration.frameskipConfig: draw every frame
    if mute:
        modified_config[0x1B] = 0  # musicMode = OFF
        modified_config[0x1C] = 0  # playSounds = false
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=".th06-preparing-", dir=destination.parent))
    try:
        manifest = {
            "format": 1,
            "source": str(source),
            "executable": EXECUTABLE,
            "logical_resolution": [640, 480],
            "windowed": True,
            "frameskip": 0,
            "directinput_disabled": no_directinput,
            "muted": mute,
            "config_before_hex": original_config.hex(),
            "config_after_hex": modified_config.hex(),
            "files": [],
        }
        for repaired, original in sorted(names.items()):
            copied = temporary / repaired
            if original.is_dir():
                shutil.copytree(original, copied, copy_function=shutil.copy2)
            else:
                shutil.copy2(original, copied)
            if original.is_file():
                manifest["files"].append({
                    "source": original.name,
                    "copy": repaired,
                    "sha256": digest(original),
                    "size": original.stat().st_size,
                })
        (temporary / CONFIG).write_bytes(modified_config)
        (temporary / MARKER).write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
        temporary.rename(destination)
        return manifest
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "game")
    parser.add_argument("--destination", type=Path, default=ROOT / "out" / "game-th06")
    parser.add_argument("--no-directinput", action="store_true",
                        help="Unsupported for the supplied 1.02f; exits without creating a copy")
    parser.add_argument("--mute", action="store_true", help="Disable game music and effects in the copy")
    args = parser.parse_args()
    source, destination = args.source.resolve(), args.destination.resolve()
    try:
        manifest = prepare(source, destination, args.no_directinput, args.mute)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Prepared: {destination}")
    print(f"Executable: {destination / manifest['executable']}")
    print(f"Manifest: {destination / MARKER}")


if __name__ == "__main__":
    main()
