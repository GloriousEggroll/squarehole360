#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
test -x build/bin/gow3
# Honor a custom asset directory for preflight as well as the actual mount.
game_data_root="$project_dir/game"
next_is_game_root=false
check_assets=true
for arg in "$@"; do
  if [[ "$next_is_game_root" == true ]]; then
    game_data_root="$arg"
    next_is_game_root=false
  elif [[ "$arg" == --game_data_root ]]; then
    next_is_game_root=true
  elif [[ "$arg" == --game_data_root=* ]]; then
    game_data_root="${arg#*=}"
  elif [[ "$arg" == --help || "$arg" == -h || "$arg" == --version ]]; then
    check_assets=false
  fi
done
if [[ "$next_is_game_root" == true || -z "$game_data_root" ]]; then
  echo 'The --game_data_root option requires an asset directory.' >&2
  exit 1
fi
# Steam Linux Runtime 3 "sniper": packaged builds are built against it
# (build/steamrt-sniper) and run inside it, using an installed copy from Steam
# or one downloaded once into ~/.local/share/squarehole360.
# GOW3_STEAM_RUNTIME: auto (default), 1 (require it), 0 (run directly).
runtime_mode="${GOW3_STEAM_RUNTIME:-auto}"
runtime_home="${XDG_DATA_HOME:-$HOME/.local/share}/squarehole360"
find_steam_runtime() {
  local roots=("${XDG_DATA_HOME:-$HOME/.local/share}/Steam" "$HOME/.steam/steam" "$HOME/.steam/root"
               "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam")
  local libraries=() root library path
  for root in "${roots[@]}"; do
    [[ -d "$root/steamapps" ]] || continue
    libraries+=("$root")
    if [[ -f "$root/steamapps/libraryfolders.vdf" ]]; then
      while IFS= read -r path; do libraries+=("$path"); done < <(
        sed -nE 's/^[[:space:]]*"path"[[:space:]]*"(.*)"[[:space:]]*$/\1/p' "$root/steamapps/libraryfolders.vdf")
    fi
  done
  for library in "${libraries[@]}"; do
    if [[ -x "$library/steamapps/common/SteamLinuxRuntime_sniper/run" ]]; then
      echo "$library/steamapps/common/SteamLinuxRuntime_sniper"
      return 0
    fi
  done
  if [[ -x "$runtime_home/SteamLinuxRuntime_sniper/run" ]]; then
    echo "$runtime_home/SteamLinuxRuntime_sniper"
    return 0
  fi
  return 1
}
download_steam_runtime() {
  local base=https://repo.steampowered.com/steamrt-images-sniper/snapshots/latest-container-runtime-depot
  command -v curl >/dev/null || { echo 'curl is needed to download the Steam Linux Runtime.' >&2; return 1; }
  mkdir -p "$runtime_home"
  local download
  download="$(mktemp -d "$runtime_home/download.XXXXXX")"
  echo 'Downloading the Steam Linux Runtime (sniper, about 190 MB); this happens once.' >&2
  if curl -fL --progress-bar -o "$download/SteamLinuxRuntime_sniper.tar.xz" "$base/SteamLinuxRuntime_sniper.tar.xz" &&
     curl -fsL -o "$download/SHA256SUMS" "$base/SHA256SUMS" &&
     (cd "$download" && grep ' \*SteamLinuxRuntime_sniper.tar.xz$' SHA256SUMS | sed 's/ \*/  /' | sha256sum -c --quiet -) &&
     tar -C "$download" -xJf "$download/SteamLinuxRuntime_sniper.tar.xz" &&
     [[ -x "$download/SteamLinuxRuntime_sniper/run" ]]; then
    rm -rf "$runtime_home/SteamLinuxRuntime_sniper"
    mv "$download/SteamLinuxRuntime_sniper" "$runtime_home/"
    rm -rf "$download"
    return 0
  fi
  rm -rf "$download"
  echo 'Could not download or verify the Steam Linux Runtime.' >&2
  return 1
}
runtime=""
if [[ "$runtime_mode" == 1 || ( "$runtime_mode" == auto && -f build/steamrt-sniper ) ]]; then
  runtime="$(find_steam_runtime || true)"
  if [[ -z "$runtime" ]] && download_steam_runtime; then
    runtime="$(find_steam_runtime || true)"
  fi
  if [[ -z "$runtime" ]]; then
    if [[ "$runtime_mode" == 1 ]]; then
      echo 'The Steam Linux Runtime is required (GOW3_STEAM_RUNTIME=1) but unavailable.' >&2
      exit 1
    fi
    echo 'Running without the Steam Linux Runtime.' >&2
  fi
fi
# Packaged builds carry libraries a distribution may lack or ship older in
# build/lib (host builds) or next to the binaries (Steam Runtime builds).
if [[ -d build/lib ]]; then
  export LD_LIBRARY_PATH="$project_dir/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
if [[ -z "$runtime" ]]; then
  missing="$(ldd build/bin/gow3 2>/dev/null | grep -E 'not found' | sed -E 's/^[[:space:]]*//' | sort -u || true)"
  if [[ -n "$missing" ]]; then
    echo 'gow3 cannot start: these system libraries are missing or too old:' >&2
    echo "$missing" >&2
    echo 'Install them with your distribution'"'"'s package manager (for example GTK 3 and the Vulkan loader),' >&2
    echo 'or let the launcher use the Steam Linux Runtime: GOW3_STEAM_RUNTIME=1.' >&2
    exit 1
  fi
fi
# Python is only needed to import a disc image; a packaged game/ directory
# carries its own completion marker.
if [[ "$check_assets" == true ]] && ! command -v python3 >/dev/null && [[ -d "$game_data_root/.gow3-assets" ]]; then
  check_assets=false
fi
if [[ "$check_assets" == true ]]; then
  if ! python3 tools/import-assets.py check --output "$game_data_root"; then
    echo 'Import your own ISO first: python3 tools/import-assets.py import "/path/to/gow3.iso"' >&2
    echo 'For an existing extraction, add --adopt-existing; for a custom directory, add --output "/path/to/game".' >&2
    exit 1
  fi
fi
mkdir -p user logs
# Prefer native Wayland, including on fractional-scale and HDR desktops.
# Check Vulkan WSI before GTK chooses its display; Xwayland is always SDR.
if [[ -z "${GDK_BACKEND:-}" ]]; then
  if [[ -x build/bin/gow3-hdr-probe ]] && timeout 5s build/bin/gow3-hdr-probe --wayland-supported >/dev/null 2>&1; then
    export GDK_BACKEND=wayland,x11
  else
    export GDK_BACKEND=x11
  fi
fi
defaults=(
  "--game_data_root=$game_data_root"
  "--user_data_root=$project_dir/user"
  "--log_file=$project_dir/logs/gow3.log"
  "--gow3_config=$project_dir/config/gow3.toml"
)
# CLI11 rejects repeated options. Omit a default when the caller supplies it.
args=()
for default in "${defaults[@]}"; do
  option="${default%%=*}"
  overridden=false
  for arg in "$@"; do
    if [[ "$arg" == "$option" || "$arg" == "$option="* ]]; then
      overridden=true
      break
    fi
  done
  if [[ "$overridden" == false ]]; then
    args+=("$default")
  fi
done
if [[ -n "$runtime" ]]; then
  echo "Using the Steam Linux Runtime at $runtime" >&2
  # The container sees the home directory; share the game and its data too.
  export PRESSURE_VESSEL_FILESYSTEMS_RW="$project_dir:$game_data_root${PRESSURE_VESSEL_FILESYSTEMS_RW:+:$PRESSURE_VESSEL_FILESYSTEMS_RW}"
  exec "$runtime/run" -- "$project_dir/build/bin/gow3" "${args[@]}" "$@"
fi
exec build/bin/gow3 "${args[@]}" "$@"
