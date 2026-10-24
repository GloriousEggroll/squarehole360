#!/usr/bin/env bash
# Creates vendor-sdks/sniper-sysroot from Valve's Steam Runtime 3 "sniper" SDK
# image (podman or docker), for cmake/toolchains/steamrt-sniper.cmake.
#   tools/steamrt-sysroot.sh [image]   (default: the latest sniper SDK)
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
image="${1:-registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest}"
sysroot="$project_dir/vendor-sdks/sniper-sysroot"
engine="$(command -v podman || command -v docker || true)"
test -n "$engine" || { echo 'Install podman or docker first.' >&2; exit 1; }

"$engine" pull "$image"
staging="$sysroot.new"
rm -rf -- "$staging"
mkdir -p -- "$staging"
container="$("$engine" create "$image")"
trap '"$engine" rm -f "$container" >/dev/null 2>&1 || true' EXIT
"$engine" export "$container" | tar -C "$staging" -xf - --exclude='dev/*'

# Absolute symlinks would point into the host inside a sysroot.
python3 - "$staging" <<'EOF'
import os, sys
root = sys.argv[1]
for directory, dirs, files in os.walk(root):
    for name in dirs + files:
        path = os.path.join(directory, name)
        if os.path.islink(path) and os.readlink(path).startswith('/'):
            target = os.path.join(root, os.readlink(path).lstrip('/'))
            os.unlink(path)
            os.symlink(os.path.relpath(target, directory), path)
EOF

# jemalloc is not part of the SDK; Debian 11's build matches its glibc.
debs="$project_dir/vendor-sdks/debs"
mkdir -p "$debs"
for package in libjemalloc2_5.2.1-3_amd64.deb libjemalloc-dev_5.2.1-3_amd64.deb; do
  test -f "$debs/$package" ||
    curl -fL -o "$debs/$package" "https://deb.debian.org/debian/pool/main/j/jemalloc/$package"
  unpack="$(mktemp -d)"
  (cd "$unpack" && ar x "$debs/$package" && tar -xf data.tar.* -C "$staging")
  rm -rf -- "$unpack"
done

rm -rf -- "$sysroot"
mv -- "$staging" "$sysroot"
grep BUILD_ID "$sysroot/usr/lib/os-release"
