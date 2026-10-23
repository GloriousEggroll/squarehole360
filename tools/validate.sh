#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
mkdir -p logs
python3 tools/audit.py --require-clean-codegen
cmake --build build --target unit_tests ppc_tests --parallel "${GOW3_BUILD_JOBS:-8}" \
  2>&1 | tee logs/build-validation.log
result=0
if ! python3 -m unittest discover -s tests -p asset_import_test.py; then
  result=1
fi
if ctest --test-dir build/rexglue-sdk -L unit --output-on-failure > logs/ctest-unit.log 2>&1; then
  tail -12 logs/ctest-unit.log
else
  result=1
  tail -28 logs/ctest-unit.log
fi
if build/bin/ppc_tests --reporter compact > logs/ppc-tests.log 2>&1; then
  cat logs/ppc-tests.log
else
  result=1
  tail -35 logs/ppc-tests.log
fi
exit "$result"
