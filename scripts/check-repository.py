#!/usr/bin/env python3
"""Check publishable files, documentation links and the exported protocol header."""
from pathlib import Path
import re
import subprocess
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
LOCAL = {"sources", "downloads", "build", "out", "prefixes", "logs", "game", ".venv"}


def main():
    paths = subprocess.check_output(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=ROOT)
    errors = []
    files = sorted({Path(p.decode()) for p in paths.split(b"\0") if p})
    for relative in files:
        path = ROOT/relative
        if relative.parts[0] in LOCAL or relative.parts[0].startswith("build-") or "__pycache__" in relative.parts:
            errors.append(f"local-only file in publishable tree: {relative}")
        if not path.is_file():
            errors.append(f"missing file: {relative}")
            continue
        if path.stat().st_size > 2*1024*1024:
            errors.append(f"unexpected file over 2 MiB: {relative}")
        if relative.suffix.lower() in (".exe", ".dll", ".so", ".pyc", ".pfx"):
            errors.append(f"generated/third-party binary in publishable tree: {relative}")
        if (relative == Path("winetk") or relative.suffix == ".sh") and not path.stat().st_mode & 0o111:
            errors.append(f"entry point is not executable: {relative}")
        if relative.suffix == ".md":
            for url in re.findall(r'\[[^\]]*\]\(([^)]+)\)', path.read_text()):
                if re.match(r"[a-zA-Z][a-zA-Z0-9+.-]*:", url) or url.startswith("#"):
                    continue
                target = (path.parent/unquote(url.split("#", 1)[0])).resolve()
                if not target.is_relative_to(ROOT) or not target.exists():
                    errors.append(f"broken local documentation link in {relative}: {url}")
    wine_header = ROOT/"sources/wine/include/wine/winetk_protocol.h"
    if wine_header.exists() and wine_header.read_bytes() != (ROOT/"include/winetk/wine_protocol.h").read_bytes():
        errors.append("project and Wine IPC headers differ; synchronize before exporting the patch")
    for error in errors:
        print(error)
    if errors:
        return 1
    print(f"Repository checks passed ({len(files)} publishable files).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
