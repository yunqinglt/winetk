#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
game=${1:-}
case "$game" in
    64) label=unity-64; executable=64.0.exe; appid=578850; width=640; height=480 ;;
    limbus) label=unity-limbus; executable=LimbusCompany.exe; appid=1973530; width=800; height=450 ;;
    *) printf 'Usage: run-unity.sh 64|limbus [run-wine options] [-- Unity arguments]\n'; exit 2 ;;
esac
shift
options=()
fit_resolution=0
while (( $# )) && [[ $1 != -- ]]; do
    case "$1" in
        --zoom-in) options+=("$1"); shift ;;
        --fit-resolution) options+=("$1"); fit_resolution=1; shift ;;
        --fps|--duration|--record-dir|--record-frames|--kitty-transfer)
            options+=("$1" "${2:?missing value}"); shift 2 ;;
        *) printf 'Unknown terminal option: %s\n' "$1" >&2; exit 2 ;;
    esac
done
if [[ ${1:-} == -- ]]; then shift; fi
python3 "$root/scripts/prepare-unity.py" "$game"
export WINETK_STEAM_BRIDGE=1
export WINEDLLPATH="$root/out/steam-bridge${WINEDLLPATH:+:$WINEDLLPATH}"
export SteamAppId=$appid SteamGameId=$appid SteamClientLaunch=1 SteamEnv=1
player_log="$root/logs/$label-Player.log"
windows_exe="Z:${root//\//\\}\\out\\$label\\$executable"
windows_log="Z:${player_log//\//\\}"
render_width=$width
render_height=$height
if (( fit_resolution )); then
    render_width=@WINETK_WIDTH@
    render_height=@WINETK_HEIGHT@
fi
exec "$root/scripts/run-wine.sh" --prefix "$root/prefixes/$label" --cwd "$root/out/$label" \
    --title "$executable" --width "$width" --height "$height" "${options[@]}" -- "$root/out/steam-bridge/steam.exe" "$windows_exe" \
    -force-d3d11 -screen-fullscreen 0 -screen-width "$render_width" -screen-height "$render_height" \
    -logFile "$windows_log" "$@"
