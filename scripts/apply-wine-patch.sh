#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source "$root/config/wine-source.env"
source_dir=${WINETK_WINE_SOURCE_DIR:-"$root/sources/wine"}
patch=${1:-"$root/patches/wine/0001-wineterm-driver.patch"}
if [[ ${1:-} == --help ]]; then
    printf 'Usage: %s [INPUT_PATCH]\n' "$0"
    printf 'WINETK_WINE_SOURCE_DIR overrides sources/wine. Already applied patches are accepted.\n'
    exit 0
fi
if (( $# > 1 )); then
    printf 'Usage: %s [INPUT_PATCH]\n' "$0" >&2
    exit 2
fi
if [[ ! -s $patch ]]; then
    printf 'Missing Wine driver patch: %s\n' "$patch" >&2
    exit 1
fi
patch=$(realpath -- "$patch")
actual=$(git -C "$source_dir" rev-parse HEAD)
if [[ $actual != "$WINE_COMMIT" ]]; then
    printf 'Wine checkout HEAD %s does not match locked baseline %s.\n' "$actual" "$WINE_COMMIT" >&2
    exit 1
fi
if git -C "$source_dir" apply --reverse --check "$patch" 2>/dev/null; then
    printf 'Wine terminal driver patch is already applied.\n'
elif git -C "$source_dir" apply --check "$patch"; then
    git -C "$source_dir" apply "$patch"
    printf 'Applied Wine terminal driver patch: %s\n' "$patch"
else
    printf 'Patch conflicts with the Wine working tree; existing files were preserved.\n' >&2
    exit 1
fi
