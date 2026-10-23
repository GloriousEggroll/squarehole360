#!/usr/bin/env bash
# Download the proprietary upscaler SDKs used by the Windows build into
# vendor-sdks/ (gitignored). Nothing here is redistributed by this project:
# each archive comes from its vendor's official GitHub release, pinned by
# version and SHA-256, under the vendor's own license.
#
#   NVIDIA DLSS SDK (NGX: DLSS Super Resolution on Windows and Linux)
#     https://github.com/NVIDIA/DLSS            (NVIDIA RTX SDKs license)
#   NVIDIA Streamline (DLSS Frame Generation and Reflex, Windows)
#     https://github.com/NVIDIA-RTX/Streamline  (NVIDIA RTX SDKs license)
#   Intel XeSS (Super Resolution)
#     https://github.com/intel/xess             (Intel Simplified Software License)
#
# Usage: tools/fetch-vendor-sdks.sh --accept-licenses [dlss] [streamline] [xess]
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
dest="$project_dir/vendor-sdks"

declare -A url sha dir
# A source archive of the release tag; its libraries are regular files.
url[dlss]=https://codeload.github.com/NVIDIA/DLSS/tar.gz/refs/tags/v310.9.16
sha[dlss]=f0de83ec7045778bc9ed456229379caaae8bc81ae3b0d5fd8ec9645c71df489d
dir[dlss]=dlss-v310.9.16
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
    dlss|streamline|xess) wanted+=("$arg") ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done
[ ${#wanted[@]} -eq 0 ] && wanted=(dlss streamline xess)
if [ "$accept" != 1 ]; then
  echo "These SDKs are licensed by NVIDIA and Intel, not by this project." >&2
  echo "Read the licenses linked at the top of this script, then rerun with --accept-licenses." >&2
  exit 1
fi

mkdir -p "$dest"
for name in "${wanted[@]}"; do
  target="$dest/${dir[$name]}"
  if [ -f "$target/.complete" ]; then echo "$name: present ($target)"; continue; fi
  archive="$dest/${dir[$name]}.${url[$name]##*.}"
  [ "$name" = dlss ] && archive="$dest/${dir[$name]}.tar.gz"
  if [ ! -f "$archive" ] || ! echo "${sha[$name]}  $archive" | sha256sum -c --status; then
    echo "$name: downloading ${url[$name]}"
    curl -fL --retry 3 -o "$archive.part" "${url[$name]}"
    mv "$archive.part" "$archive"
  fi
  echo "${sha[$name]}  $archive" | sha256sum -c --quiet
  rm -rf "$target.part"; mkdir -p "$target.part"
  # Python refuses absolute paths and '..' members in both formats.
  if [[ "$archive" == *.tar.gz ]]; then
    python3 -I -c 'import sys,tarfile; tarfile.open(sys.argv[1]).extractall(sys.argv[2], filter="data")' \
      "$archive" "$target.part"
    # GitHub source archives have one top-level directory.
    inner=("$target.part"/*)
    if [ ${#inner[@]} -eq 1 ] && [ -d "${inner[0]}" ]; then
      mv "${inner[0]}" "$target.flat" && rm -rf "$target.part" && mv "$target.flat" "$target.part"
    fi
  else
    python3 -I -m zipfile -e "$archive" "$target.part"
  fi
  rm -rf "$target"; mv "$target.part" "$target"
  touch "$target/.complete"
  echo "$name: extracted to $target"
done
