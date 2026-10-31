#!/usr/bin/env bash
# Download the runtime pieces the Windows and macOS packages carry, so that
# players install nothing else: a private Python with PySide6 and tomli-w for
# the setup wizard, asset importer and settings app, and on Windows the
# Visual C++ runtime DLLs. Everything goes to vendor-sdks/runtime/ (gitignored),
# pinned by version and SHA-256, from the official sources:
#
#   Python 3.13 embeddable package (Windows)   python.org, PSF License
#   python-build-standalone 3.13 (macOS)       github.com/astral-sh, PSF License
#   PySide6-Essentials, shiboken6, tomli-w     PyPI; LGPLv3 and MIT
#   Visual C++ 2015-2022 Redistributable x64   Microsoft; its license permits
#                                              app-local copies of these DLLs
#
# Usage: tools/fetch-runtime-deps.sh [windows] [macos]
# Extracting the Visual C++ DLLs needs cabextract.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
dest="$project_dir/vendor-sdks/runtime"
downloads="$dest/downloads"

sha256() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

# fetch NAME SHA256 URL: download into $downloads once and verify.
fetch() {
  local file="$downloads/$1"
  mkdir -p "$downloads"
  if [[ ! -f "$file" || "$(sha256 "$file")" != "$2" ]]; then
    echo "downloading $1" >&2
    curl -fL --retry 3 -o "$file.part" "$3"
    mv "$file.part" "$file"
  fi
  if [[ "$(sha256 "$file")" != "$2" ]]; then
    echo "$1: checksum mismatch" >&2
    exit 1
  fi
  echo "$file"
}

pypi=https://files.pythonhosted.org/packages
wheel_tomli_w() {
  fetch tomli_w-1.2.0-py3-none-any.whl 188306098d013b691fcadc011abd66727d3c414c571bb01b1a174ba8c983cf90 \
    "$pypi/c7/18/c86eb8e0202e32dd3df50d43d7ff9854f8e0603945ff398974c1d91ac1ef/tomli_w-1.2.0-py3-none-any.whl"
}

# Wheels need no installer: their contents go into site-packages as they are.
install_wheels() {
  local site="$1"; shift
  mkdir -p "$site"
  for wheel in "$@"; do
    python3 -I -m zipfile -e "$wheel" "$site"
  done
}

windows() {
  local out="$dest/windows" python vcredist
  rm -rf "$out.part"; mkdir -p "$out.part/python" "$out.part/vcruntime"
  python="$(fetch python-3.13.17-embed-amd64.zip 9e35319bffedab8b36ad44d8f377cb4f1733bcec257a0b6d04d94f01486ddb38 \
    https://www.python.org/ftp/python/3.13.17/python-3.13.17-embed-amd64.zip)"
  python3 -I -m zipfile -e "$python" "$out.part/python"
  # 6.11, not 6.12: Qt 6.12's Windows platform plugin calls
  # mscms WcsCreateIccProfile, which Wine does not implement, so every window
  # aborts under Proton.
  install_wheels "$out.part/python/Lib/site-packages" \
    "$(fetch pyside6_essentials-6.11.3-cp310-abi3-win_amd64.whl ea61687d2c27f8cc7a851c259481be1001dbffa3bebacea4491d9905e7a5d0cf \
       "$pypi/10/02/24eeb84caf10de98e366a2e536abf8e66b23a90c72553ccb3d77acef2dfd/pyside6_essentials-6.11.3-cp310-abi3-win_amd64.whl")" \
    "$(fetch shiboken6-6.11.3-cp310-abi3-win_amd64.whl 1d38919492d08417b7579ad077bc81110d05183d21eb21d66316bc556140a8a3 \
       "$pypi/ec/f7/db1cb2ce128d5b7d5397cafdf666520db4466cf41d9c04aa7cb299341051/shiboken6-6.11.3-cp310-abi3-win_amd64.whl")" \
    "$(wheel_tomli_w)"
  # The embeddable package only searches the paths in its ._pth file.
  printf 'Lib\\site-packages\n' >> "$out.part/python/python313._pth"

  vcredist="$(fetch VC_redist.x64.exe cc0ff0eb1dc3f5188ae6300faef32bf5beeba4bdd6e8e445a9184072096b713b \
    https://download.visualstudio.microsoft.com/download/pr/990c720a-46ce-4dc7-87b9-c2eb615d34b9/CC0FF0EB1DC3F5188AE6300FAEF32BF5BEEBA4BDD6E8E445A9184072096B713B/VC_redist.x64.exe)"
  command -v cabextract >/dev/null || { echo 'cabextract is needed for the Visual C++ DLLs' >&2; exit 1; }
  local work="$out.part/vcredist"
  mkdir -p "$work"
  # The bundle carries its packages as cabinets; nested cabinets hold the DLLs
  # under names with an architecture suffix.
  python3 -I - "$vcredist" "$work" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
offset, count = 0, 0
while (offset := data.find(b'MSCF\0\0\0\0', offset)) >= 0:
    size = struct.unpack_from('<I', data, offset + 8)[0]
    if 0 < size <= len(data) - offset:
        open(f'{sys.argv[2]}/bundle{count}.cab', 'wb').write(data[offset:offset + size])
        count += 1
    offset += 4
PY
  for cab in "$work"/bundle*.cab; do cabextract -q -d "$work/${cab##*/}.d" "$cab" 2>/dev/null || true; done
  while IFS= read -r -d '' file; do
    cabextract -q -d "$work/nested" "$file" 2>/dev/null || true
  done < <(find "$work" -path '*.cab.d/*' -type f -print0)
  for dll in msvcp140.dll msvcp140_atomic_wait.dll vcruntime140.dll vcruntime140_1.dll; do
    test -f "$work/nested/${dll}_amd64" || { echo "Visual C++ redistributable: $dll not found" >&2; exit 1; }
    cp "$work/nested/${dll}_amd64" "$out.part/vcruntime/$dll"
  done
  rm -rf "$work"
  rm -rf "$out"; mv "$out.part" "$out"
  echo "windows: $out"
}

macos() {
  local out="$dest/macos" python
  rm -rf "$out.part"; mkdir -p "$out.part"
  python="$(fetch cpython-3.13.17-aarch64-apple-darwin-install_only.tar.gz 9c985cbf327425873469896ce748c7134b6deca8edb81e93c589ad2fc5aff445 \
    "https://github.com/astral-sh/python-build-standalone/releases/download/20261021/cpython-3.13.17%2B20261021-aarch64-apple-darwin-install_only.tar.gz")"
  # The archive's top-level directory is python/. Symbolic links (bin/python3)
  # are kept; the data filter refuses links leaving the tree.
  python3 -I -c 'import sys,tarfile; tarfile.open(sys.argv[1]).extractall(sys.argv[2], filter="data")' \
    "$python" "$out.part"
  install_wheels "$out.part/python/lib/python3.13/site-packages" \
    "$(fetch pyside6_essentials-6.12.0-cp310-abi3-macosx_14_0_universal2.whl 0867b709a724db28b161227c7d6ecb5ed10cc09226a53542417ac6dee1308239 \
       "$pypi/e1/90/d528f8dd3f9f20712bc719e10c698d27c998e1dd76ed3f33b6fb606d0972/pyside6_essentials-6.12.0-cp310-abi3-macosx_14_0_universal2.whl")" \
    "$(fetch shiboken6-6.12.0-cp310-abi3-macosx_14_0_universal2.whl 47105e05baf57d35453e07d240bbe59cb4b8d9565377ea5134ff9fb37d4f5c02 \
       "$pypi/b5/45/11536fba5f50a57671eb14441c6098096f3e57074e54c9d7d66039a5831e/shiboken6-6.12.0-cp310-abi3-macosx_14_0_universal2.whl")" \
    "$(wheel_tomli_w)"
  rm -rf "$out"; mv "$out.part" "$out"
  echo "macos: $out"
}

wanted=("$@")
[[ ${#wanted[@]} -eq 0 ]] && wanted=(windows macos)
for target in "${wanted[@]}"; do
  case "$target" in
    windows) windows ;;
    macos) macos ;;
    *) echo "unknown target: $target" >&2; exit 2 ;;
  esac
done
