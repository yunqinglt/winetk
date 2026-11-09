#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source_dir=${WINETK_WINE_SOURCE_DIR:-"$root/sources/wine"}
build_dir=${WINETK_WINE_BUILD_DIR:-"$root/build/wine-gdi"}
install_prefix=${WINETK_WINE_INSTALL_PREFIX:-"$root/out/wine-gdi"}
jobs=${WINETK_BUILD_JOBS:-8}
if [[ ${1:-} == --help ]]; then
    printf 'Usage: %s [MAKE_TARGET ...]\n' "$0"
    printf 'Applies the locked Wine patch, configures without X11/Wayland, builds Wine and smoke PEs.\n'
    printf 'WINETK_BUILD_JOBS: parallel make jobs (default 8).\n'
    printf 'WINETK_WINE_SOURCE_DIR, WINETK_WINE_BUILD_DIR, WINETK_WINE_INSTALL_PREFIX override paths.\n'
    exit 0
fi
if [[ ! $jobs =~ ^[1-9][0-9]*$ ]]; then
    printf 'WINETK_BUILD_JOBS must be a positive integer.\n' >&2
    exit 2
fi
if [[ ! -x $source_dir/configure ]]; then
    printf 'Missing Wine source; run scripts/fetch-wine.sh first.\n' >&2
    exit 1
fi
source_dir=$(realpath -- "$source_dir")
WINETK_WINE_SOURCE_DIR="$source_dir" "$root/scripts/apply-wine-patch.sh"
"$root/scripts/check-environment.sh"
mkdir -p -- "$build_dir" "$install_prefix" "$root/logs"
build_dir=$(realpath -- "$build_dir")
install_prefix=$(realpath -- "$install_prefix")
(
    cd -- "$build_dir"
    "$source_dir/configure" \
        --prefix="$install_prefix" \
        --enable-archs=i386,x86_64 \
        --without-x --without-wayland \
        --with-opengl --with-vulkan --without-sdl --without-va \
        --disable-tests
) 2>&1 | tee "$root/logs/configure-wine-gdi.log"
make -C "$build_dir" -j "$jobs" "$@" 2>&1 | tee "$root/logs/build-wine.log"
"$root/scripts/build-smoke.sh"
printf '\nWine terminal driver build complete: %s/wine\n' "$build_dir"
printf 'GDI smoke executables: %s\n' "${WINETK_SMOKE_OUTPUT_DIR:-"$root/out/smoke"}"
