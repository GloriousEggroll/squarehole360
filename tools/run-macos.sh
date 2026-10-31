#!/usr/bin/env bash
# macOS counterpart of tools/run-linux.sh: asset preflight (the setup wizard on
# first start), default paths inside the install, then the game. Extra
# arguments go to gow3.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
test -x build/bin/gow3 || { echo 'Missing build/bin/gow3; build it with tools/build-macos.sh.' >&2; exit 1; }

# Packages carry their own Python (python/) with PySide6 and tomli-w.
python=""
if [[ -x python/bin/python3 ]]; then
  python="$project_dir/python/bin/python3"
elif command -v python3 >/dev/null && python3 -c 'import sys; sys.exit(sys.version_info < (3, 11))'; then
  python=python3
fi

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
if [[ "$check_assets" == true && -z "$python" ]]; then
  if [[ -d "$game_data_root/.gow3-assets" ]]; then
    check_assets=false
  else
    echo 'Python 3.11 or newer is needed to import the game (packages include it in python/).' >&2
    exit 1
  fi
fi
if [[ "$check_assets" == true ]]; then
  if ! "$python" tools/import-assets.py check --output "$game_data_root" >/dev/null 2>&1; then
    # First run (or damaged files): the setup wizard imports your own copy of
    # the game, then optionally its DLC.
    if ! "$python" tools/setup-wizard.py --output "$game_data_root" ||
       ! "$python" tools/import-assets.py check --output "$game_data_root"; then
      echo 'The game files are not set up. Run tools/setup-wizard.py, or import your own ISO:' >&2
      echo '  python/bin/python3 tools/import-assets.py import "/path/to/gow3.iso"' >&2
      exit 1
    fi
  fi
fi
mkdir -p user logs
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
# The game finds the packaged Vulkan loader, MoltenVK and OpenSSL in build/lib.
exec build/bin/gow3 "${args[@]}" "$@"
