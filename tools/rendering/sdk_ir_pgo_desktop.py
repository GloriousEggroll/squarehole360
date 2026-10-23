#!/usr/bin/env python3
"""Bounded, interactive SDK-PGO training with copied saves and private settings.

Linux desktop diagnostic only. Instrumented FPS is never comparison evidence.
Close the game normally after the route; stop.request also asks the compositor
to close its window. Forced shutdown fails qualification and retains evidence.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import resource
import shutil
import subprocess
import time

import sdk_ir_pgo as pgo


def terminal_health(log):
    def final(label):
        matches = re.findall(re.escape(label) + r": ([^\n]*)", log)
        return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", matches[-1])} if matches else {}
    strict = final("Strict Native scene summary")
    queries = final("Native query deferral summary")
    native_queries = final("Native deferred query counters")
    raster = final("Native renderer final counters")
    unmixed = all(strict.get(key) == 0 for key in (
        "xenia_draw", "xenia_copy", "xenia_query", "xenia_swap_source",
        "xenia_import", "xenia_restore", "rejected"))
    retired = (queries.get("deferred", 0) > 0 and queries.get("deferred") == queries.get("retired")
               == queries.get("guest_written") and queries.get("pending") == 0
               and queries.get("superseded") == 0
               and native_queries.get("deferred") == native_queries.get("completed") == queries.get("deferred")
               and all(native_queries.get(key) == 0 for key in ("pending", "ready", "scene_pending")))
    healthy = not any(marker in log for marker in (
        "disabled at API frame", "Native renderer unavailable", "Native renderer startup failed",
        "Native renderer fatal error", "Fatal native packet transaction",
        "using the reference backend", "reference_retired=true"))
    return dict(execution_complete="Execution complete" in log, strict_native_unmixed=unmixed,
                native_healthy=healthy,
                queries_retired=retired, strict_scene_counters=strict,
                query_counters=queries, native_query_counters=native_queries,
                raster_counters=raster, gameplay_observed=raster.get("frames", 0) > 100
                and raster.get("raster_packets", 0) > 1000)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    # Shared explicit train admission flags; this runner always uses the host GPU,
    # Native renderer and copied user data. There is no reference backend mode.
    for flag in ("hardware", "raster", "copy-user-data"):
        parser.add_argument("--" + flag, action="store_true", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = args.output.resolve()
    pgo.need(60 <= args.seconds <= 900, "training duration outside 60..900 seconds")
    pgo.need(out.is_relative_to(root / "reports") and not out.exists(), "fresh workspace report required")
    pgo.need(os.environ.get("WAYLAND_DISPLAY") or os.environ.get("DISPLAY"), "host desktop missing")
    pgo.need(not os.environ.get("LD_PRELOAD"), "unexpected inherited preload")
    pgo.need(os.environ.get("LLVM_PROFILE_FILE"), "training profile path missing")
    live = subprocess.run(["pgrep", "-x", "gow3"], capture_output=True, text=True)
    pgo.need(live.returncode == 1, "another game is active or process census failed")
    out.mkdir(parents=True)
    spec = importlib.util.spec_from_file_location("boot", root / "tools/rendering/verify-native-boot.py")
    boot = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(boot)
    identity = {name: {"sha256": pgo.digest(root / name), "bytes": (root / name).stat().st_size}
                for name in ("build/bin/gow3", "build/bin/librexruntime.so")}
    pgo.save(out / "build-identity.json", identity)
    original_config = root / "config/gow3.toml"
    original_sha = pgo.digest(original_config)
    original_saves = boot.user_data_manifest(root / "user")
    shutil.copytree(root / "user", out / "user", ignore=shutil.ignore_patterns("cache"))
    pgo.need(original_saves == boot.user_data_manifest(out / "user") == boot.user_data_manifest(root / "user"),
             "copied saves differ from originals")
    pgo.save(out / "user-data-seed-identity.json", original_saves)
    cache = out / "cache"
    cache.mkdir()
    seeds = {}
    for name, copier in (("native-shaders", boot.copy_native_cache),
                         ("native-pipeline-recipes", boot.copy_pipeline_cache)):
        source = root / "user/cache" / name
        if source.is_dir():
            seeds[name] = copier(source, cache / name)
    pgo.save(out / "cache-seed.json", seeds)
    overrides = dict(fullscreen=True, gow3_renderer="native", gow3_native_live=True,
                     gow3_native_present=True, gow3_frame_limit_mode="unlimited",
                     framerate_limit=0, vsync=False, anisotropic_override=-1,
                     gow3_renderer_capture="", gow3_renderer_history="", gow3_renderer_capture_trigger="",
                     gow3_native_time_cpu_stages=False, gow3_native_time_draw_recording=False,
                     gow3_native_gpu_timestamps=False, gow3_native_gpu_batch_report="",
                     gow3_native_gpu_draw_report="", present_gpu_timestamps=False,
                     gow3_native_query_wait_census=False, gow3_native_query_readiness_census=False,
                     gpu_command_summary=False, gow3_native_diagnose_completion_causes=False)
    configuration = original_config.read_text()
    for key, value in overrides.items():
        assignment = key + " = " + json.dumps(value)
        pattern = r"(?m)^" + re.escape(key) + r"\s*=.*$"
        configuration = re.sub(pattern, lambda _: assignment, configuration) if re.search(pattern, configuration) else configuration + "\n" + assignment + "\n"
    (out / "gow3.toml").write_text(configuration)
    command = ["mangohud", str(root / "tools/run-linux.sh"),
               "--user_data_root=" + str(out / "user"), "--cache_path=" + str(cache),
               "--gow3_config=" + str(out / "gow3.toml"),
               "--log_file=" + str(out / "game.log"), "--log_max_file_size_mb=128",
               "--gow3_native_failure_directory=" + str(out / "failure")]
    env = dict(os.environ, MANGOHUD_CONFIG="fps,frametime,frame_timing,gpu_stats,cpu_stats",
               DISABLE_OBS_VKCAPTURE="1", MESA_SHADER_CACHE_DIR=str(out / "mesa-cache"))
    # The launch script performs the same asset/Wayland/HDR checks as normal use.
    pgo.save(out / "launch.json", dict(command=command, overrides=overrides,
             instrumented_training_only=True, performance_claim=False,
             normal_config_sha256=original_sha, profile_path=env["LLVM_PROFILE_FILE"]))
    result = dict(passed=False, clean_ui_close=False, forced_shutdown=False, close_requested=False)
    def disable_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    with (out / "console.log").open("x") as console:
        game = subprocess.Popen(command, cwd=root, env=env, stdout=console,
                                stderr=subprocess.STDOUT, preexec_fn=disable_core)
        (out / "game.pid").write_text(str(game.pid) + "\n")
        start = time.monotonic()
        close_at = None
        print(json.dumps({"game_pid": game.pid, "output": str(out), "training": True}), flush=True)
        try:
            while game.poll() is None:
                elapsed = time.monotonic() - start
                if close_at is None and ((out / "stop.request").exists() or elapsed >= args.seconds):
                    close_at = time.monotonic()
                    result["close_requested"] = True
                    try:
                        close = subprocess.run(["wlrctl", "toplevel", "close", "app_id:gow3"],
                                               text=True, capture_output=True, timeout=3)
                        result.update(close_command_returncode=close.returncode,
                                      close_command_output=close.stdout + close.stderr)
                    except (OSError, subprocess.TimeoutExpired) as error:
                        result["close_error"] = str(error)
                if close_at is not None and time.monotonic() - close_at > 20:
                    result["forced_shutdown"] = True
                    game.terminate()
                    try:
                        game.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        game.kill()
                        game.wait()
                    break
                time.sleep(.25)
            result.update(returncode=game.wait(), elapsed_seconds=time.monotonic() - start)
        finally:
            if game.poll() is None:
                result["forced_shutdown"] = True
                game.terminate()
                try:
                    game.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    game.kill()
                    game.wait()
    logfile = out / "game.log"
    health = terminal_health(logfile.read_text(errors="replace") if logfile.is_file() else "")
    result.update(health)
    result["clean_ui_close"] = result["returncode"] == 0 and not result["forced_shutdown"] and health["execution_complete"]
    result["normal_config_unchanged"] = pgo.digest(original_config) == original_sha
    result["normal_saves_unchanged"] = boot.user_data_manifest(root / "user") == original_saves
    result["passed"] = all(result[key] for key in (
        "clean_ui_close", "strict_native_unmixed", "native_healthy", "queries_retired", "gameplay_observed",
        "normal_config_unchanged", "normal_saves_unchanged"))
    pgo.save(out / "result.json", result)
    print(json.dumps(result), flush=True)
    pgo.need(result["passed"], "desktop training failed health checks; report retained")


if __name__ == "__main__":
    main()
