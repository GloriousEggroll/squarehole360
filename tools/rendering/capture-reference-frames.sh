#!/usr/bin/env bash
set -euo pipefail
window_id="${1:?Xenia window ID required}"
for ((frame=0; frame<12; ++frame)); do
  xdotool windowactivate --sync "$window_id" keydown F12 sleep 0.3 keyup F12
  sleep 5
done
