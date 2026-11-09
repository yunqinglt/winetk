#!/usr/bin/env bash
# Export tracked and new driver files without changing Wine's real Git index.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/config/wine-source.env"
source_dir=${WINETK_WINE_SOURCE_DIR:-"$root/sources/wine"}
patch=${1:-"$root/patches/wine/0001-wineterm-driver.patch"}
if [[ ${1:-} == --help ]]; then
    printf 'Usage: %s [OUTPUT_PATCH]\n' "$0"
    printf 'WINETK_WINE_SOURCE_DIR overrides sources/wine. The real Git index is preserved.\n'
    exit 0
fi
if (( $# > 1 )); then
    printf 'Usage: %s [OUTPUT_PATCH]\n' "$0" >&2
    exit 2
fi
actual=$(git -C "$source_dir" rev-parse HEAD)
if [[ $actual != "$WINE_COMMIT" ]]; then
    printf 'Wine checkout HEAD %s does not match locked baseline %s.\n' "$actual" "$WINE_COMMIT" >&2
    exit 1
fi
temporary=$(mktemp -d "${TMPDIR:-/tmp}/winetk-wine-export.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT
export GIT_INDEX_FILE="$temporary/index"
git -C "$source_dir" read-tree "$WINE_COMMIT"
paths=(configure configure.ac include/Makefile.in include/wine/winetk_protocol.h dlls/wineterm.drv dlls/win32u/dce.c dlls/win32u/window.c dlls/ntdll/loader.c)
git -C "$source_dir" add -A -- "${paths[@]}"
if git -C "$source_dir" diff --cached --quiet "$WINE_COMMIT" -- "${paths[@]}"; then
    printf 'No terminal driver changes to export.\n' >&2
    exit 1
fi
{
    printf 'WineTk terminal GDI, surfaceless OpenGL and opt-in Steam bridge\n\n'
    printf 'Base-Wine-Version: %s\nBase-Wine-Commit: %s\n\n' "$WINE_VERSION" "$WINE_COMMIT"
    git -C "$source_dir" -c core.quotePath=true diff --cached --binary --full-index \
        --no-ext-diff "$WINE_COMMIT" -- "${paths[@]}"
} > "$temporary/driver.patch"
mkdir -p -- "$(dirname -- "$patch")"
# Copy to the destination filesystem before the atomic rename.
output=$(mktemp "$(dirname -- "$patch")/.winetk-patch.XXXXXX")
trap 'rm -rf -- "$temporary"; rm -f -- "${output:-}"' EXIT
cat "$temporary/driver.patch" > "$output"
chmod 644 "$output"
mv -f -- "$output" "$patch"
printf 'Exported Wine %s driver patch: %s\n' "$WINE_VERSION" "$patch"
