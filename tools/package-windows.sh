#!/usr/bin/env bash
# Packages the Windows build as dist/squarehole360-windows-x64.zip.
#
#   tools/package-windows.sh                 # program only
#   tools/package-windows.sh --with-assets   # plus game/ and the shader cache
#
# Build first with tools/build-windows.sh, and fetch the runtime pieces once
# with tools/fetch-runtime-deps.sh windows. The package carries the Visual C++
# runtime DLLs next to the game and a private Python (python/) with PySide6
# and tomli-w for the setup wizard, the asset importer and the settings app, so
# players install nothing but a Vulkan GPU driver. Saves, user/identity.toml
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

build=build-windows/bin
test -f "$build/gow3.exe" || { echo 'Build first: tools/build-windows.sh' >&2; exit 1; }
runtime=vendor-sdks/runtime/windows
test -d "$runtime/python" || { echo 'Fetch the runtime first: tools/fetch-runtime-deps.sh windows' >&2; exit 1; }

stage="dist/windows/squarehole360"
rm -rf "$stage"
mkdir -p "$stage/build-windows/bin" "$stage/config" "$stage/docs" "$stage/tools"

# Program files. DLSS and XeSS are the vendors' release binaries, present when
# their SDKs were fetched.
for file in gow3.exe gow3-shader-precompile.exe gow3-vrr-probe.exe rexruntime.dll mimalloc.dll amd_fidelityfx_vk.dll dxcompiler.dll squarehole-lobby.exe; do
  cp "$build/$file" "$stage/build-windows/bin/"
done
# Double-click launchers at the package root.
cp "$build/squarehole360-launcher.exe" "$stage/squarehole360.exe"
cp "$build/squarehole360-settings.exe" "$stage/Settings.exe"
for file in nvngx_dlss.dll libxess.dll; do
  test -f "$build/$file" && cp "$build/$file" "$stage/build-windows/bin/"
done
cp "$runtime"/vcruntime/*.dll "$stage/build-windows/bin/"
cp -r "$runtime/python" "$stage/python"
# The embeddable Python runs isolated and imports only from its ._pth paths,
# not from a script's folder; the tools import their sibling modules.
printf '..\\tools\n..\\tools\\settings\n' >> "$stage/python/python313._pth"

# Documentation, default configuration and tools.
cp README.md LICENSE THIRD_PARTY_NOTICES.md "$stage/"
cp docs/gears3.md docs/multiplayer.md "$stage/docs/"
git show HEAD:config/gow3.toml > "$stage/config/gow3.toml"
cp tools/run-windows.bat tools/run-windows.ps1 tools/settings-windows.bat tools/import-assets.py \
   tools/setup-wizard.py "$stage/tools/"
cp -r tools/settings tools/assets "$stage/tools/"
# The importer reads the global shader cache with this scanner.
mkdir -p "$stage/tools/rendering"
cp tools/rendering/extract-shaders.py "$stage/tools/rendering/"
find "$stage/tools" -name __pycache__ -type d -prune -exec rm -rf {} +
cat > "$stage/README-WINDOWS.txt" <<'EOF'
squarehole360 - Gears of War 3, Windows build (x86-64, Vulkan)

1. Double-click squarehole360.exe. Without the full package, the first
   start opens a setup wizard: choose your own Xbox 360 disc image (ISO) or
   extracted game folder, then optionally your DLC (folders or
   .zip/.tar.gz/.tar.xz archives). Add DLC later with
   python\python.exe tools\setup-wizard.py --dlc
   (the full package already contains game\ and a shader cache).
   Saves go to "user", logs to "logs".
2. Settings: Settings.exe, config\gow3.toml, or F4 / L3+R3 in game.
   tools\run-windows.bat and tools\settings-windows.bat do the same from a
   command prompt.

Everything needed is included (the Visual C++ runtime, and Python with
PySide6 in python\ for the wizard and settings); the PC needs a Vulkan 1.3
GPU driver and a CPU with FMA3 (Intel Haswell / AMD FX or newer). Also runs
under Proton or Wine, for example:
  umu-run squarehole360.exe

Upscaling (config\gow3.toml):
  present_effect = "fsr3"   # or "dlss" (NVIDIA RTX), "xess", "fsr", "cas", "bilinear"
  present_frame_generation = true   # FSR 3.1 frame generation
  gow3_low_latency = true           # game at most one frame ahead
  present_anti_lag = true           # AMD Anti-Lag where the driver has it
DLSS or XeSS fall back to FSR 3.1 when unavailable; the log says which runs.
See docs\gears3.md for details.

Multiplayer (System Link and internet play): see docs\multiplayer.md.
build-windows\bin\squarehole-lobby.exe is the lobby server.

nvngx_dlss.dll (NVIDIA DLSS) and libxess.dll (Intel XeSS) are the vendors'
official release binaries under their own licenses. python\ is CPython
(PSF License) with PySide6 and shiboken6 (LGPLv3) and tomli-w (MIT). The
Visual C++ runtime DLLs are Microsoft's redistributable files.

If game\ is included, it is Microsoft's copyrighted content: only use this
package with your own copy of the game.
EOF

name="squarehole360-windows-x64"
sources=(squarehole360)
if [[ "$with_assets" == true ]]; then
  python3 tools/import-assets.py check --output game
  name+="-full"
  for cache in native-shaders native-pipeline-recipes native-fsi-state-recipes native-fsi-state-shaders shaders; do
    test -d "user/cache/$cache" || continue
    mkdir -p "$stage/user/cache"
    cp -r "user/cache/$cache" "$stage/user/cache/"
  done
  ln -sfn "$project_dir/game" "$stage/game"
fi
rm -f "dist/$name.zip"
(cd dist/windows && python3 -I - "../$name.zip" "${sources[@]}" <<'PY'
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
    for top in sys.argv[2:]:
        for folder, directories, files in os.walk(top, followlinks=True):
            directories.sort()
            for name in sorted(files):
                path = os.path.join(folder, name)
                archive.write(path, path)
PY
)
rm -f "$stage/game"
echo "dist/$name.zip"
sha256sum "dist/$name.zip"
