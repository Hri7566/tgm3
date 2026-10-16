#!/usr/bin/env bash
# Run TGM3 on Linux: native jvsd provides the JVS I/O board, Wine runs game.exe with jvsbridge.dll.
#
#   TGM3_DIR      game directory          (default: ../Tetris The Grand Master 3  (PC))
#   JVSD_PORT     loopback port           (default: 26100)
#   JVSD_CONFIG   bindings file           (default: bindings.conf next to this script)
#   TGM3_DESKTOP  e.g. 1280x960 to run inside a Wine virtual desktop (contains mode switches)
#   JVSD_VERBOSE  set to 1 to log JVS traffic
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
build=$here/build
game_dir=${TGM3_DIR:-"$here/../Tetris The Grand Master 3  (PC)"}
port=${JVSD_PORT:-26100}

for f in jvsd jvsbridge.dll tgm3inject.exe; do
    [[ -e $build/$f ]] || { echo "missing $build/$f - run 'make' in $here" >&2; exit 1; }
done
[[ -f $game_dir/game.exe ]] || { echo "game.exe not found in $game_dir (set TGM3_DIR)" >&2; exit 1; }

jvsd_args=(-p "$port" -c "${JVSD_CONFIG:-$here/bindings.conf}")
[[ ${JVSD_VERBOSE:-0} == 1 ]] && jvsd_args+=(-v)
"$build/jvsd" "${jvsd_args[@]}" &
jvsd_pid=$!
trap 'kill "$jvsd_pid" 2>/dev/null || true; wait "$jvsd_pid" 2>/dev/null || true' EXIT

# Saves go to .\TGM3\sramdata relative to the working directory (game.exe is patched from D:\).
cd "$game_dir"
export JVSD_PORT=$port
bridge=$(winepath -w "$build/jvsbridge.dll")

if [[ -n ${TGM3_DESKTOP:-} ]]; then
    wine explorer "/desktop=tgm3,$TGM3_DESKTOP" "$(winepath -w "$build/tgm3inject.exe")" game.exe "$bridge"
else
    wine "$build/tgm3inject.exe" game.exe "$bridge"
fi
