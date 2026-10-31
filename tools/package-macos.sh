#!/usr/bin/env bash
# Packages the macOS build as dist/squarehole360-macos-arm64.tar.gz. Run on
# the Mac after tools/build-macos.sh and, once, tools/fetch-runtime-deps.sh macos.
#
#   tools/package-macos.sh                 # program only
#   tools/package-macos.sh --with-assets   # plus game/ and the shader cache
#
# Like the Linux package, it carries what a Mac lacks: the Vulkan loader,
# MoltenVK (Vulkan on Metal) and OpenSSL in build/lib, and a private Python
# (python/) with PySide6 and tomli-w for the setup wizard, the asset importer
# and the settings app. Players need no Homebrew. Saves, user/identity.toml
# and personal settings are never included.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"

with_assets=false
for arg in "$@"; do
  case "$arg" in
    --with-assets) with_assets=true ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "Unknown option: $arg" >&2; exit 2 ;;
  esac
done
[[ "$(uname -s)" == Darwin ]] || { echo 'Run this on the Mac.' >&2; exit 1; }
test -x build/bin/gow3 || { echo 'Build first: tools/build-macos.sh' >&2; exit 1; }
runtime=vendor-sdks/runtime/macos
test -d "$runtime/python" || { echo 'Fetch the runtime first: tools/fetch-runtime-deps.sh macos' >&2; exit 1; }

stage="dist/macos/squarehole360"
rm -rf "$stage"
mkdir -p "$stage/build/bin" "$stage/build/lib/vulkan/icd.d" "$stage/config" "$stage/docs" "$stage/tools"

for file in gow3 librexruntime.dylib libdxcompiler.dylib squarehole-lobby; do
  cp -L "build/bin/$file" "$stage/build/bin/"
done

# Homebrew libraries the game links (OpenSSL) move next to it under their
# file names; the Vulkan loader and MoltenVK are opened at run time by name.
bundle() {
  cp -L "$1" "$stage/build/lib/"
  chmod u+w "$stage/build/lib/${1##*/}"
  install_name_tool -id "@rpath/${1##*/}" "$stage/build/lib/${1##*/}"
}
while read -r dependency; do
  bundle "$dependency"
  install_name_tool -change "$dependency" "@executable_path/../lib/${dependency##*/}" "$stage/build/bin/gow3"
done < <(otool -L build/bin/gow3 | awk 'NR > 1 && ($1 ~ "^/opt/homebrew/" || $1 ~ "^/usr/local/") {print $1}')
bundle "$(brew --prefix vulkan-loader)/lib/libvulkan.1.dylib"
bundle "$(brew --prefix molten-vk)/lib/libMoltenVK.dylib"
cat > "$stage/build/lib/vulkan/icd.d/MoltenVK_icd.json" <<'EOF'
{
    "file_format_version" : "1.0.0",
    "ICD": {
        "library_path": "../../libMoltenVK.dylib",
        "api_version" : "1.4.0",
        "is_portability_driver" : true
    }
}
EOF
# Every bundled library must need nothing else from Homebrew.
for file in "$stage"/build/bin/* "$stage"/build/lib/*.dylib; do
  if otool -L "$file" | awk 'NR > 1 {print $1}' | grep -E '^(/opt/homebrew|/usr/local)/'; then
    echo "$file still links Homebrew libraries" >&2
    exit 1
  fi
done
# Editing load commands invalidates the signatures; sign again (ad hoc).
for file in "$stage"/build/bin/* "$stage"/build/lib/*.dylib; do
  codesign --force --sign - "$file"
done
cp -R "$runtime/python" "$stage/python"

# Documentation, default configuration and tools.
cp README.md LICENSE THIRD_PARTY_NOTICES.md "$stage/"
cp docs/gears3.md docs/multiplayer.md "$stage/docs/"
if git rev-parse --git-dir >/dev/null 2>&1; then
  git show HEAD:config/gow3.toml > "$stage/config/gow3.toml"
else
  cp config/gow3.toml "$stage/config/gow3.toml"
fi
# No FidelityFX (FSR 3.1, frame generation) on macOS: default to FSR 1.
sed -i '' -e 's/^present_effect = "fsr3"/present_effect = "fsr"/' \
  -e 's/^present_frame_generation = true/present_frame_generation = false/' "$stage/config/gow3.toml"
cp tools/run-macos.sh tools/settings-macos.sh tools/import-assets.py tools/setup-wizard.py "$stage/tools/"
cp -R tools/settings tools/assets "$stage/tools/"
# The importer reads the global shader cache with this scanner.
mkdir -p "$stage/tools/rendering"
cp tools/rendering/extract-shaders.py "$stage/tools/rendering/"
find "$stage/tools" -name __pycache__ -type d -prune -exec rm -rf {} +
cat > "$stage/README-MACOS.txt" <<'EOF'
squarehole360 - Gears of War 3, macOS build (Apple silicon, Vulkan on Metal)

0. The game is not signed by Apple. After extracting, allow it once in
   Terminal:  xattr -dr com.apple.quarantine squarehole360
1. Run tools/run-macos.sh. Without the full package, the first start opens a
   setup wizard: choose your own Xbox 360 disc image (ISO) or extracted game
   folder, then optionally your DLC (folders or .zip/.tar.gz/.tar.xz
   archives). Add DLC later with
   python/bin/python3 tools/setup-wizard.py --dlc
   (the full package already contains game/ and a shader cache).
2. Settings: tools/settings-macos.sh, config/gow3.toml, or F4 / L3+R3 in game.

Everything needed is included: MoltenVK and the Vulkan loader, OpenSSL, and
Python with PySide6 in python/ for the wizard and settings. Needs macOS 14 or
newer on an Apple silicon Mac. FSR 3.1 frame generation, DLSS and XeSS are
not available on macOS; FSR 1 and CAS are.

Multiplayer (System Link and internet play): see docs/multiplayer.md.
build/bin/squarehole-lobby is the lobby server.

build/lib holds MoltenVK and the Vulkan loader (Apache 2.0) and OpenSSL
(Apache 2.0). python/ is CPython from python-build-standalone (PSF License)
with PySide6 and shiboken6 (LGPLv3) and tomli-w (MIT).

If game/ is included, it is Microsoft's copyrighted content: only use this
package with your own copy of the game.
EOF

name="squarehole360-macos-arm64"
sources=(-C dist/macos squarehole360)
if [[ "$with_assets" == true ]]; then
  "$stage/python/bin/python3" tools/import-assets.py check --output game
  name+="-full"
  sources+=(-C "$project_dir" -s ',^game,squarehole360/game,' -s ',^user,squarehole360/user,' game)
  for cache in native-shaders native-pipeline-recipes native-fsi-state-recipes native-fsi-state-shaders shaders; do
    test -d "user/cache/$cache" && sources+=("user/cache/$cache")
  done
fi
tar -czf "dist/$name.tar.gz" "${sources[@]}"
echo "dist/$name.tar.gz"
shasum -a 256 "dist/$name.tar.gz"
