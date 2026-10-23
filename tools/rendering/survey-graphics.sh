#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$project_dir"
headless="${GHIDRA_HEADLESS:-/usr/lib64/ghidra/support/analyzeHeadless}"
jdk="${GOW3_JDK:-$project_dir/../gow3_work/toolchains/jdk21/usr/lib/jvm/java-21-openjdk}"
output="$project_dir/reports/rendering/native-renderer/survey"
mkdir -p "$output" analysis/config analysis/cache
env JAVA_HOME="$jdk" XDG_CONFIG_HOME="$project_dir/analysis/config" \
  XDG_CACHE_HOME="$project_dir/analysis/cache" GHIDRA_HEADLESS_MAXMEM=6G \
  "$headless" "$project_dir/analysis/projects" gow3_base \
  -process default.xex -noanalysis -readOnly -scriptPath "$project_dir/tools" \
  -postScript FindGraphicsReferences.java "$output" \
  -log "$output/ghidra.log" -scriptlog "$output/script.log"
