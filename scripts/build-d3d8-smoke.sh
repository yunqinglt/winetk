#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
output_dir=${WINETK_SMOKE_OUTPUT_DIR:-"$root/out/smoke"}
mkdir -p -- "$output_dir"
for target in x86_64 i686; do
    compiler="$target-w64-mingw32-gcc"
    architecture=$target
    if [[ $target == i686 ]]; then architecture=i386; fi
    "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror -municode -mconsole \
        "$root/tests/smoke/d3d8-window.c" -o "$output_dir/d3d8-window-$architecture.exe" \
        -luser32
    printf 'Built %s\n' "$output_dir/d3d8-window-$architecture.exe"
done
