#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
game_dir=${WINETK_TH06_DIR:-"$root/out/game-th06"}
game_dir=$(realpath -m -- "$game_dir")
locale_dir="$root/out/locales"
if [[ ! -d "$game_dir" ]]; then
    python3 "$root/scripts/prepare-th06.py" --destination "$game_dir"
fi
if [[ ! -f "$game_dir/.winetk-th06.json" || ! -f "$game_dir/東方紅魔郷.exe" ]]; then
    printf 'Invalid prepared game directory: %s\n' "$game_dir" >&2
    exit 1
fi
if [[ ! -f "$locale_dir/ja_JP.UTF-8/LC_CTYPE" ]]; then
    mkdir -p -- "$locale_dir"
    # A private glibc locale gives Wine CP932 without changing system locales.
    localedef --no-archive -i ja_JP -f UTF-8 "$locale_dir/ja_JP.UTF-8"
fi
export LOCPATH="$locale_dir"
export LC_ALL=ja_JP.UTF-8
exec "$root/scripts/run-wine.sh" --prefix "$root/prefixes/th06" --cwd "$game_dir" \
    "$@" -- "$game_dir/東方紅魔郷.exe"
