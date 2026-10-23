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
exec build/bin/gow3 "${args[@]}" "$@"
