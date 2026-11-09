#!/usr/bin/env bash
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cmake -S "$root" -B "$root/build/terminal" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$root/build/terminal"
mkdir -p "$root/logs"
exec "$root/build/terminal/winetk-terminal-demo" --log "$root/logs/terminal-demo.jsonl" "$@"
