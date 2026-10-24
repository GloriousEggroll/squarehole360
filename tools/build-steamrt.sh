#!/usr/bin/env bash
# Builds the game against the Steam Linux Runtime 3 "sniper" into build-sniper/
# (see cmake/toolchains/steamrt-sniper.cmake). Run tools/build-linux.sh once
# first: it generates the recompiled sources this build reuses.
# Then: tools/package-linux.sh --steamrt [--with-assets]
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
jobs="${GOW3_BUILD_JOBS:-8}"
mkdir -p logs
test -f generated/default/sources.cmake ||
  { echo 'Generate the sources first with tools/build-linux.sh.' >&2; exit 1; }
test -d vendor-sdks/sniper-sysroot/usr/lib/gcc-14 || tools/steamrt-sysroot.sh
cmake -S . -B build-sniper -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/steamrt-sniper.cmake \
  -DREXGLUE_BUILD_TESTS=OFF 2>&1 | tee logs/configure-sniper.log
cmake --build build-sniper --target gow3 gow3-hdr-probe gow3-vrr-probe --parallel "$jobs" 2>&1 |
  tee logs/build-sniper.log
# Libraries the runtime and newer distributions lack, next to the binaries.
for soname in libjemalloc.so.2 libcrypto.so.1.1; do
  cp -L "vendor-sdks/sniper-sysroot/usr/lib/x86_64-linux-gnu/$soname" "build-sniper/bin/$soname"
done
