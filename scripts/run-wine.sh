#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
wine=${WINETK_WINE_LOADER:-"$root/build/wine-gdi/wine"}
prefix="$root/prefixes/gdi"
fps=60
cwd=""
options=()
while [[ $# -gt 0 && $1 != -- ]]; do
    case "$1" in
        --zoom-in|--fit-resolution) options+=("$1"); shift ;;
        --fps) fps=${2:?missing fps}; shift 2 ;;
        --prefix) prefix=${2:?missing prefix}; shift 2 ;;
        --cwd) cwd=${2:?missing working directory}; shift 2 ;;
        --duration|--record-dir|--record-frames|--kitty-transfer|--title|--width|--height) options+=("$1" "${2:?missing value}"); shift 2 ;;
        --help) printf 'Usage: run-wine.sh [--fps N] [--zoom-in|--fit-resolution] [--width W --height H] [--prefix PATH] [--cwd DIR] [--duration S] [--kitty-transfer auto|direct] [--record-dir NEWDIR] -- [PROGRAM [ARGS...]]\n'; exit 0 ;;
        *) break ;;
    esac
done
if [[ ! $fps =~ ^[0-9]+$ ]] || (( fps<1 || fps>240 )); then
    printf 'FPS must be 1..240.\n' >&2; exit 2
fi
prefix=$(realpath -m -- "$prefix")
mkdir -p -- "$(dirname -- "$prefix")"
if [[ -n "$cwd" ]]; then
    cwd=$(realpath -e -- "$cwd")
    options+=(--wine-cwd "$cwd")
fi
if [[ ! -x "$wine" || ! -f "$root/build/wine-gdi/dlls/wineterm.drv/wineterm.so" ]]; then
    printf '请先运行 ./scripts/build-wine.sh 构建含终端驱动的 Wine。\n' >&2
    exit 1
fi
mkdir -p "$root/logs" "$root/prefixes"
# A prefix's explorer must belong to the same terminal session as its apps.
exec 9>"$prefix.session.lock"
if ! flock -n 9; then
    printf '项目 Wine 前缀已被另一个终端会话使用，请先退出该会话。\n' >&2
    exit 1
fi
cleanup() {
    # The lock and dedicated prefix make this kill specific to our session.
    env WINEPREFIX="$prefix" "$root/build/wine-gdi/server/wineserver" -k 9>&- 2>/dev/null || true
}
trap cleanup EXIT
cmake -S "$root" -B "$root/build/terminal" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$root/build/terminal"
if [[ ! -f "$prefix/.winetk-term-v1" ]]; then
    env -u DISPLAY -u WAYLAND_DISPLAY WINEPREFIX="$prefix" WINEDLLOVERRIDES=winemenubuilder.exe=d \
        "$wine" reg add 'HKCU\Software\Wine\Drivers' /v Graphics /t REG_SZ /d term /f \
        >"$root/logs/wine-prefix.log" 2>&1 9>&-
    touch "$prefix/.winetk-term-v1"
    # Bootstrap explorer may have cached the null display driver before the
    # registry value was written. Start the real session with a fresh server.
    env WINEPREFIX="$prefix" "$root/build/wine-gdi/server/wineserver" -k 9>&-
    env WINEPREFIX="$prefix" "$root/build/wine-gdi/server/wineserver" -w 9>&-
fi
if [[ ${1:-} == -- ]]; then shift; fi
if [[ $# == 0 ]]; then
    "$root/scripts/build-smoke.sh"
    set -- "$root/out/smoke/gdi-window-x86_64.exe"
fi
session_log="$root/logs/wine-session.jsonl"
application_log="$root/logs/wine-application.log"
if [[ $prefix != "$root/prefixes/gdi" ]]; then
    label=$(printf '%s' "$(basename -- "$prefix")" | LC_ALL=C tr -c 'A-Za-z0-9._-' '_')
    session_log="$root/logs/wine-$label-session.jsonl"
    application_log="$root/logs/wine-$label-application.log"
fi
"$root/build/terminal/winetk-terminal-demo" --fps "$fps" "${options[@]}" \
    --log "$session_log" --wine "$wine" --wine-prefix "$prefix" \
    --wine-log "$application_log" -- "$@" 9>&-
