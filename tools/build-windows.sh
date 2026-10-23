#!/usr/bin/env bash
# Cross-compile the Windows build (build-windows/bin/gow3.exe) on Linux with
# clang-cl and an xwin sysroot. Code generation runs on the Linux host, so the
# recompiled sources come from tools/build-linux.sh; run that first.
#
# Sysroot (accepts Microsoft's license):
#   xwin --accept-license --arch x86_64 splat --use-winsysroot-style \
#     --preserve-ms-arch-notation --output ~/.xwin
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
jobs="${GOW3_BUILD_JOBS:-8}"
export XWIN_ROOT="${XWIN_ROOT:-$HOME/.xwin}"
if [[ ! -d "$XWIN_ROOT/VC/Tools/MSVC" ]]; then
  echo "No xwin sysroot at $XWIN_ROOT (set XWIN_ROOT); see the comment in this script." >&2
  exit 1
fi
if [[ ! -f generated/default/sources.cmake || ! -f generated/native-getdata/getdata_leaf_body.inc ]]; then
  echo 'Generated sources are missing; run tools/build-linux.sh first.' >&2
  exit 1
fi
mkdir -p logs
cmake -S . -B build-windows -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=vendor/rexglue-sdk/cmake/toolchains/xwin-clang-cl.cmake \
  -DREXGLUE_USE_D3D12=OFF -DREXGLUE_USE_VULKAN=ON 2>&1 | tee logs/configure-windows.log
cmake --build build-windows --target gow3 --parallel "$jobs" 2>&1 | tee logs/build-windows.log
echo "Built build-windows/bin/gow3.exe; on Windows run tools\\run-windows.bat"
