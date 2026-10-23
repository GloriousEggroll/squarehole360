#!/usr/bin/env bash
# Download the proprietary upscaler SDKs used by the Windows build into
# vendor-sdks/ (gitignored). Nothing here is redistributed by this project:
# each archive comes from its vendor's official GitHub release, pinned by
# version and SHA-256, under the vendor's own license.
#
#   NVIDIA Streamline (DLSS Super Resolution and Frame Generation)
#     https://github.com/NVIDIA-RTX/Streamline  (NVIDIA RTX SDKs license)
#   Intel XeSS (Super Resolution)
#     https://github.com/intel/xess             (Intel Simplified Software License)
#
# Usage: tools/fetch-vendor-sdks.sh --accept-licenses [streamline] [xess]
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
dest="$project_dir/vendor-sdks"

declare -A url sha dir
url[streamline]=https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip
sha[streamline]=32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b
dir[streamline]=streamline-v2.14.16
url[xess]=https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip
sha[xess]=88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990
dir[xess]=xess-v3.0.2

accept=0
wanted=()
for arg in "$@"; do
  case "$arg" in
    --accept-licenses) accept=1 ;;
    streamline|xess) wanted+=("$arg") ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done
[ ${#wanted[@]} -eq 0 ] && wanted=(streamline xess)
if [ "$accept" != 1 ]; then
  echo "These SDKs are licensed by NVIDIA and Intel, not by this project." >&2
  echo "Read the licenses linked at the top of this script, then rerun with --accept-licenses." >&2
  exit 1
fi

mkdir -p "$dest"
for name in "${wanted[@]}"; do
  target="$dest/${dir[$name]}"
  if [ -f "$target/.complete" ]; then echo "$name: present ($target)"; continue; fi
  archive="$dest/$(basename "${url[$name]}")"
  if [ ! -f "$archive" ] || ! echo "${sha[$name]}  $archive" | sha256sum -c --status; then
    echo "$name: downloading ${url[$name]}"
    curl -fL --retry 3 -o "$archive.part" "${url[$name]}"
    mv "$archive.part" "$archive"
  fi
  echo "${sha[$name]}  $archive" | sha256sum -c --quiet
  rm -rf "$target.part"; mkdir -p "$target.part"
  # Python's zipfile refuses absolute paths and '..' members.
  python3 -I -m zipfile -e "$archive" "$target.part"
  rm -rf "$target"; mv "$target.part" "$target"
  touch "$target/.complete"
  echo "$name: extracted to $target"
done
