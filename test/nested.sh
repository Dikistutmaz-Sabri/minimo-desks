#!/bin/sh
# Starts a nested test Hyprland (a window inside Plasma) and prints its instance signature.
# Every later hyprctl call must use: hyprctl --instance "$SIG" ...
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
[ -z "${HYPRLAND_INSTANCE_SIGNATURE:-}" ] || { echo "refusing: already inside Hyprland" >&2; exit 1; }
before=$(ls "$XDG_RUNTIME_DIR/hypr" 2>/dev/null || true)
HYPRLAND_NO_SD_VARS=1 XDG_CURRENT_DESKTOP=Hyprland setsid Hyprland --config "$HERE/hyprland-test.lua" >"$HERE/nested.log" 2>&1 &
sleep 4
for s in $(ls "$XDG_RUNTIME_DIR/hypr"); do echo "$before" | grep -qx "$s" || echo "$s"; done
