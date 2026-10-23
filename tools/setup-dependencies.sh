#!/usr/bin/env bash
# Fetch the pinned third-party sources (git submodules) and apply this
# project's patches to them. Safe to run again: applied patches are skipped.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
git submodule update --init --recursive

apply() {
  local dir=$1 patch=$PWD/$2
  if git -C "$dir" apply --check -R "$patch" 2>/dev/null; then
    echo "already applied: $2"
  else
    git -C "$dir" apply "$patch"
    echo "applied: $2"
  fi
}
apply vendor/plume patches/plume.patch
apply vendor/fidelityfx-sdk patches/fidelityfx-sdk.patch
apply vendor/xenos-recomp patches/xenos-recomp.patch
apply vendor/rexglue-sdk/thirdparty/glslang patches/rexglue-sdk-thirdparty-glslang.patch
apply vendor/rexglue-sdk/thirdparty/spirv-tools patches/rexglue-sdk-thirdparty-spirv-tools.patch
