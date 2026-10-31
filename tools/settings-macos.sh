#!/usr/bin/env bash
# Standalone settings editor. Packages carry their own Python (python/);
# otherwise it needs Python 3.11+, PySide6 and tomli-w.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -x "$project_dir/python/bin/python3" ]]; then
  exec "$project_dir/python/bin/python3" "$project_dir/tools/settings/app.py" "$@"
fi
exec python3 "$project_dir/tools/settings/app.py" "$@"
