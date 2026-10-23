#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.."
./tools/run-linux.sh --fullscreen=false --window_width=1280 --window_height=900 \
  --log_file=reports/rendering/native-hdr/game-menu.log --log_max_file_size_mb=30 \
  > reports/rendering/native-hdr/console-menu.log 2>&1 &
for frame in 1 2 3 4 5 6; do
  sleep 0.5
  grim -o DP-2 -t jpeg "reports/rendering/native-hdr/shader-bar-$frame.jpg"
done
wait
