#!/usr/bin/env bash
# Fetch the pinned dependencies (git submodules). Our changes to them live on
# the squarehole360 branch of each fork listed in .gitmodules.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
git submodule sync --recursive
git submodule update --init --recursive
