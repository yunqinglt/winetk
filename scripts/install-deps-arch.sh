#!/usr/bin/env bash
set -euo pipefail

# 不刷新包索引，不进行整个系统升级；使用主机现有的 Arch 包数据库。
# base-devel 包含 make/gcc 等；现代 WoW64 使用 64 位 Unix 库和双架构 PE 工具链。
exec pkexec pacman -S --needed --noconfirm \
    base-devel git curl xz autoconf clang lld mingw-w64-gcc \
    cmake ninja pkgconf flex bison python python-pillow \
    freetype2 fontconfig gnutls libpng libjpeg-turbo zlib libunwind \
    alsa-lib libpulse libglvnd vulkan-headers vulkan-icd-loader strace
