#!/usr/bin/env bash
set -euo pipefail
window_id="${1:?Game window ID required}"
output="${2:?Screenshot output directory required}"
mkdir -p "$output"
frame_count="${3:-150}"
for ((frame=0; frame<frame_count; ++frame)); do
  xdotool windowactivate --sync "$window_id" windowraise "$window_id"
  if ((frame % 2 == 0)); then
    xdotool keydown F12 sleep 0.15 keyup F12
    if [[ "${4:-}" == trace ]]; then
      xdotool keydown F5 sleep 0.15 keyup F5
    fi
  fi
  import -window "$window_id" "$output/frame-$(printf '%03d' "$frame").png"
  sleep 1
done
