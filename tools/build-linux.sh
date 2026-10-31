#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
jobs="${GOW3_BUILD_JOBS:-8}"
mkdir -p logs
python3 tools/import-assets.py check
python3 tools/audit.py > logs/input-check.json
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DREXGLUE_BUILD_TESTS=ON 2>&1 | tee logs/configure.log
cmake --build build --target rexglue --parallel "$jobs" 2>&1 | tee logs/build-tool.log
LD_LIBRARY_PATH="$project_dir/build/bin${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  build/bin/rexglue codegen gow3_manifest.toml 2>&1 | tee logs/codegen.log
python3 tools/audit.py --require-clean-codegen
# XDK-derived GetData inputs are generated from the user's own executable.
cmake --build build --target gow3-xex-image-dump --parallel "$jobs" 2>&1 | tee logs/build-image-dump.log
python3 tools/porting/generate_getdata.py
cmake -S . -B build 2>&1 | tee logs/configure-game.log
cmake --build build --target gow3 gow3-shader-precompile squarehole-lobby --parallel "$jobs" 2>&1 | tee logs/build-game.log
