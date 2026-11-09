#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source_dir="$root/sources/wine"
build_dir="$root/build/wine-gdi"
if [[ ! -x "$source_dir/configure" ]]; then
    printf '请先运行 scripts/fetch-wine.sh。\n' >&2
    exit 1
fi
"$root/scripts/check-environment.sh"
mkdir -p "$build_dir" "$root/logs" "$root/out"
cd "$build_dir"
"$source_dir/configure" \
    --prefix="$root/out/wine-gdi" \
    --enable-archs=i386,x86_64 \
    --without-x --without-wayland \
    --with-opengl --with-vulkan --without-sdl --without-va \
    --disable-tests 2>&1 | tee "$root/logs/configure-wine-gdi.log"
printf '\n无 X11/Wayland、启用离屏 OpenGL 的配置完成。终端显示依赖 wineterm 补丁；请使用 scripts/build-wine.sh 应用补丁并构建。\n'
