#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$project_dir"
wlr-randr --output HEADLESS-1 --custom-mode 2560x1440@60 --scale "${GOW3_WINDOW_TEST_SCALE:-1}"
exec env GDK_BACKEND="${GOW3_WINDOW_TEST_BACKEND:-wayland}" build/bin/gow3-window-size-test
