#!/usr/bin/env bash
# Build the macOS (Apple silicon) version: build/bin/gow3. Code generation
# runs on Linux (tools/build-linux.sh), so the recompiled sources in
# generated/ come from there.
#
# Needs the Xcode command-line tools and, from Homebrew: cmake ninja llvm
# molten-vk vulkan-loader openssl glslang. The packaged game carries its Vulkan loader,
# MoltenVK and OpenSSL; see tools/package-macos.sh.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
jobs="${GOW3_BUILD_JOBS:-$(sysctl -n hw.ncpu)}"
if [[ ! -f generated/default/sources.cmake || ! -f generated/native-getdata/getdata_leaf_body.inc ]]; then
  echo 'Generated sources are missing; run tools/build-linux.sh on Linux and copy generated/.' >&2
  exit 1
fi
command -v brew >/dev/null || { echo 'Homebrew is needed: https://brew.sh' >&2; exit 1; }
llvm="$(brew --prefix llvm)"
mkdir -p logs
# FidelityFX's shader compiler is a Windows program; macOS builds go without
# FSR 3.1 and frame generation.
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$llvm/bin/clang" -DCMAKE_CXX_COMPILER="$llvm/bin/clang++" \
  -DGOW3_ENABLE_FIDELITYFX=OFF -DGOW3_XENIA_RENDERER=OFF 2>&1 | tee logs/configure-macos.log
cmake --build build --target gow3 gow3-shader-precompile squarehole-lobby --parallel "$jobs" 2>&1 | tee logs/build-macos.log
echo "Built build/bin/gow3; run tools/run-macos.sh"
