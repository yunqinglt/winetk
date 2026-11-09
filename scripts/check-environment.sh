#!/usr/bin/env bash
set -euo pipefail
failed=0
for tool in git curl xz gcc clang make pkg-config flex bison autoconf python3 \
            x86_64-w64-mingw32-gcc i686-w64-mingw32-gcc; do
    if ! command -v "$tool" >/dev/null; then
        printf '缺少命令：%s\n' "$tool" >&2
        failed=1
    fi
done
for library in freetype2 fontconfig gnutls libpng libjpeg zlib libunwind alsa libpulse egl gl vulkan; do
    if pkg-config --exists "$library"; then
        printf '%-14s %s\n' "$library" "$(pkg-config --modversion "$library")"
    else
        printf '缺少开发库：%s\n' "$library" >&2
        failed=1
    fi
done
if (( failed )); then exit 1; fi
printf '基础构建依赖检查通过（不代表终端协议或 Wine 图形功能已实现）。\n'
