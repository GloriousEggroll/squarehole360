#!/usr/bin/env bash
# Packages the Linux build as dist/squarehole360-linux-x86_64[-full].tar.zst.
#
#   tools/package-linux.sh                 # program only
#   tools/package-linux.sh --with-assets   # plus game/ and the shader cache
#   tools/package-linux.sh --steamrt       # from build-sniper (recommended)
#
# --steamrt packages the Steam Linux Runtime (sniper) build: it needs glibc 2.30
# for our code and 2.34 for the prebuilt shader compiler, links libstdc++
# statically, and the launcher runs it inside the runtime (installed by Steam
# or downloaded once). jemalloc and OpenSSL 1.1, which the runtime and newer
# distributions lack, sit next to the binaries.
#
# Without --steamrt the host build is packaged with the runtime libraries a
# distribution may lack or ship older (libstdc++, libgcc_s, jemalloc, OpenSSL)
# in build/lib, on the library path set by the launcher.
#
# GTK, Wayland/X11, the Vulkan loader and GPU drivers come from the system or
# the runtime. Saves, user/identity.toml and personal settings are never
# included.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"

with_assets=false
steamrt=false
for arg in "$@"; do
  case "$arg" in
    --with-assets) with_assets=true ;;
    --steamrt) steamrt=true ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "Unknown option: $arg" >&2; exit 2 ;;
  esac
done

build="build"
if [[ "$steamrt" == true ]]; then
  build="build-sniper"
  test -x "$build/bin/gow3" || { echo 'Build first: tools/build-steamrt.sh' >&2; exit 1; }
else
  test -x build/bin/gow3 || { echo 'Build first: tools/build-linux.sh' >&2; exit 1; }
fi
stage="dist/linux/squarehole360"
rm -rf "$stage"
mkdir -p "$stage/build/bin" "$stage/build/lib" "$stage/config" "$stage/docs" "$stage/tools"

# Program files.
for file in gow3 gow3-hdr-probe gow3-vrr-probe librexruntime.so libamd_fidelityfx_vk.so libdxcompiler.so; do
  cp -aL "$build/bin/$file" "$stage/build/bin/"
done
cp -a build/bin/libnvidia-ngx-dlss.so.* "$stage/build/bin/" 2>/dev/null || true
# The lobby server runs outside the launcher, so it is linked statically.
clang++ -std=c++20 -O2 -static -s -Ivendor/rexglue-sdk/include tools/lobby/lobby_server.cpp \
  -o "$stage/build/bin/squarehole-lobby"

if [[ "$steamrt" == true ]]; then
  # Found through the binaries' $ORIGIN run path, inside the runtime or not.
  sysroot="vendor-sdks/sniper-sysroot"
  for soname in libjemalloc.so.2 libcrypto.so.1.1; do
    cp -L "$sysroot/usr/lib/x86_64-linux-gnu/$soname" "$stage/build/bin/$soname"
  done
  rmdir "$stage/build/lib"
  touch "$stage/build/steamrt-sniper"
else
  # Bundled runtime libraries, under their sonames.
  bundled=(libstdc++.so.6 libgcc_s.so.1 libjemalloc.so.2 libcrypto.so.3)
  for soname in "${bundled[@]}"; do
    path="$(ldd build/bin/gow3 build/bin/librexruntime.so | awk -v name="$soname" '$1 == name && !found {print $3; found = 1}')"
    test -n "$path" || { echo "Cannot locate $soname" >&2; exit 1; }
    cp -L "$path" "$stage/build/lib/$soname"
  done
fi

# Documentation, default configuration and tools.
cp README.md LICENSE THIRD_PARTY_NOTICES.md "$stage/"
cp docs/gears3.md docs/multiplayer.md "$stage/docs/"
git show HEAD:config/gow3.toml > "$stage/config/gow3.toml"
cp tools/run-linux.sh tools/settings.sh tools/import-assets.py "$stage/tools/"
cp -r tools/settings tools/assets "$stage/tools/"
find "$stage/tools" -name __pycache__ -type d -prune -exec rm -rf {} +
cat > "$stage/README-LINUX.txt" <<'EOF'
squarehole360 - Gears of War 3, Linux build (x86-64, Vulkan)

1. Without the full package: import your own disc image first,
   python3 tools/import-assets.py import "/path/to/gow3.iso"
   (the full package already contains game/ and a shader cache).
2. Run tools/run-linux.sh (for example: setsid mangohud ./tools/run-linux.sh).
3. Settings: tools/settings.sh, config/gow3.toml, or F4 / L3+R3 in game.

Multiplayer (System Link and internet play): see docs/multiplayer.md.
  - Set your gamertag in tools/settings.sh -> Online.
  - Internet play: set "Lobby server" to the server's address and use the same
    "Room" as the other players.
  - In game: main menu -> Back (Tab) -> Party Options -> System Link.
  A player ID is created in user/identity.toml on first start; never copy it
  between PCs. build/bin/squarehole-lobby is the lobby server (static binary).

REQUIREMENTS_PLACEHOLDER

If game/ is included, it is Microsoft's copyrighted content: only use this
package with your own copy of the game.
EOF

if [[ "$steamrt" == true ]]; then
  requirements="Built for the Steam Linux Runtime 3 (sniper). The launcher runs the game inside
it, using a copy installed by Steam or downloading it once (about 190 MB) into
~/.local/share/squarehole360; set GOW3_STEAM_RUNTIME=0 to run directly.
Needs a distribution with glibc 2.34 or newer (2021 on: Ubuntu 22.04, Debian 12,
Fedora 35, SteamOS 3) and a Vulkan driver. The settings app needs Python 3 with
PySide6 and tomli-w."
else
  requirements="The C++ runtime, jemalloc and OpenSSL are included in build/lib. The system
must provide glibc $(ldd --version | head -1 | awk '{print $NF}') or newer, GTK 3, Wayland or X11, the Vulkan loader and
a Vulkan driver; the launcher lists anything missing. The settings app also
needs Python 3 with PySide6 and tomli-w."
fi
python3 - "$stage/README-LINUX.txt" "$requirements" <<'PYEOF'
import sys
path, text = sys.argv[1], sys.argv[2]
content = open(path).read().replace('REQUIREMENTS_PLACEHOLDER', text)
open(path, 'w').write(content)
PYEOF

# Every library must resolve with the bundled ones on the path.
missing="$(LD_LIBRARY_PATH="$stage/build/lib" ldd "$stage/build/bin/gow3" | grep 'not found' || true)"
test -z "$missing" || { echo "Unresolved libraries: $missing" >&2; exit 1; }

name="squarehole360-linux-x86_64"
[[ "$steamrt" == true ]] && name+="-steamrt"
sources=(-C dist/linux squarehole360)
if [[ "$with_assets" == true ]]; then
  python3 tools/import-assets.py check --output game
  name+="-full"
  caches=()
  for cache in native-shaders native-pipeline-recipes native-fsi-state-recipes native-fsi-state-shaders shaders; do
    test -d "user/cache/$cache" && caches+=("user/cache/$cache")
  done
  sources+=(-C "$project_dir" --transform 's,^game,squarehole360/game,;s,^user,squarehole360/user,'
            game "${caches[@]}")
fi
tar -cf - "${sources[@]}" | zstd -T0 -6 -q -f -o "dist/$name.tar.zst"
echo "dist/$name.tar.zst"
sha256sum "dist/$name.tar.zst"
