#!/usr/bin/env bash
# Private fullscreen texture-decoder trial; copied saves and caches.
# MangoHud only, with FPS and frame-time graph; no profiling or video capture.
set -euo pipefail
gow3_test_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd -- "$gow3_test_root"
gow3_test_stage="$gow3_test_root/reports/rendering/native-renderer/performance-60fps/room-fix-20261006-02/combat-gpu-after-learned-20261011/texture-decode-blocks"
exec python3 "$gow3_test_stage/fullscreen-01/launch.py" --launch "$@"
