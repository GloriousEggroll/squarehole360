#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"
headless="${GHIDRA_HEADLESS:-/usr/lib64/ghidra/support/analyzeHeadless}"
jdk="${GOW3_JDK:-$project_dir/../gow3_work/toolchains/jdk21/usr/lib/jvm/java-21-openjdk}"
test -x "$headless"
test -x "$jdk/bin/java"
test -f analysis/projects/gow3_base.gpr
mkdir -p logs analysis/config analysis/cache
env JAVA_HOME="$jdk" \
  XDG_CONFIG_HOME="$project_dir/analysis/config" \
  XDG_CACHE_HOME="$project_dir/analysis/cache" \
  GHIDRA_HEADLESS_MAXMEM=6G \
  "$headless" "$project_dir/analysis/projects" gow3_base \
  -process default.xex -noanalysis -scriptPath "$project_dir/tools" \
  -postScript ExportDecompilation.java "$project_dir/analysis/decompiled" \
  4e5d00b8f584f42951344b26a2ae8292bb30c7634b1a75cec0c70a80b3f8f790 \
  -log "$project_dir/logs/ghidra.log" \
  -scriptlog "$project_dir/logs/ghidra-export.log" \
  2>&1 | tee logs/decompile-session.log
