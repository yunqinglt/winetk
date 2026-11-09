#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
output_dir=${WINETK_SMOKE_OUTPUT_DIR:-"$root/out/smoke"}
mkdir -p -- "$output_dir"
for target in x86_64 i686; do
    compiler="$target-w64-mingw32-gcc"
    if ! command -v "$compiler" >/dev/null; then
        printf 'Missing compiler: %s\n' "$compiler" >&2
        exit 1
    fi
    architecture=$target
    if [[ $target == i686 ]]; then architecture=i386; fi
    "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror -municode -mconsole \
        "$root/tests/smoke/gdi-window.c" -o "$output_dir/gdi-window-$architecture.exe" \
        -lgdi32 -luser32
    printf 'Built %s\n' "$output_dir/gdi-window-$architecture.exe"
    "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror -municode -mconsole \
        "$root/tests/smoke/wgl-window.c" -o "$output_dir/wgl-window-$architecture.exe" \
        -lopengl32 -lgdi32 -luser32
    printf 'Built %s\n' "$output_dir/wgl-window-$architecture.exe"
    "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror -municode -mconsole \
        "$root/tests/smoke/d3d8-window.c" -o "$output_dir/d3d8-window-$architecture.exe" \
        -luser32
    printf 'Built %s\n' "$output_dir/d3d8-window-$architecture.exe"
    "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror -municode -mconsole \
        "$root/tests/smoke/dinput8-window.c" -o "$output_dir/dinput8-window-$architecture.exe" \
        -ldinput8 -ldxguid -lgdi32 -luser32
    printf 'Built %s\n' "$output_dir/dinput8-window-$architecture.exe"
    "$compiler" -std=c11 -O2 -g -Wall -Wextra -Werror -municode -mconsole \
        "$root/tests/smoke/d3d11-window.c" -o "$output_dir/d3d11-window-$architecture.exe" \
        -ld3d11 -ld3dcompiler -ldxguid -luser32
    printf 'Built %s\n' "$output_dir/d3d11-window-$architecture.exe"
done
