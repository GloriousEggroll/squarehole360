#!/usr/bin/env python3
"""Bounded Linux diagnostics for one user-controlled native gameplay run.

The game is never stopped by this helper. CPU profiles have explicit time spans
so their overhead can be excluded from frame-time comparisons.
"""
import argparse
from datetime import datetime, timezone
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("native_boot", Path(__file__).with_name("verify-native-boot.py"))
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)


def identity(pid):
    root = Path("/proc") / str(pid)
    try:
        executable = (root / "exe").resolve(strict=True)
        fields = (root / "stat").read_text().rsplit(")", 1)[1].split()
        return executable, fields[19]
    except (OSError, IndexError):
        return None


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("report", type=Path)
    p.add_argument("--seconds", type=int, default=600)
    args = p.parse_args()
    if not 30 <= args.seconds <= 900:
        p.error("Duration must be30–900seconds")
    report = args.report.resolve()
    pid = int((report / "game.pid").read_text())
    began = time.monotonic()
    expected = ROOT / "build/bin/gow3"
    # The launcher's PID survives its shell preflight and exec into the game.
    owner = None
    while time.monotonic() - began < 15:
        candidate = identity(pid)
        if candidate and candidate[0] == expected:
            owner = candidate
            break
        time.sleep(.1)
    if owner is None:
        raise SystemExit("The recorded PID did not become this installation's game")
    meta = {"pid": pid, "process_start_ticks": owner[1],
            "monitor_start_utc": datetime.now(timezone.utc).isoformat(),
            "monitor_start_monotonic": began,
            "profile_policy": "Three15second profiles at scene+10,+65,+130seconds; exclude these intervals from timing comparisons",
            "scene_anchor": None}
    status_path = report / "monitor.json"
    def save():
        temp = status_path.with_suffix(".tmp")
        temp.write_text(json.dumps(meta, indent=2) + "\n")
        temp.replace(status_path)
    save()
    profiles = []
    sampler_log = (report / "process-sampler.log").open("x")
    sampler = subprocess.Popen([sys.executable, str(Path(__file__).with_name("sample-process.py")),
        str(pid), str(report / "threads.csv"), "--seconds", str(args.seconds)],
        stdout=sampler_log, stderr=subprocess.STDOUT, start_new_session=True)
    def interrupted(signum, _frame):
        raise SystemExit(128 + signum)
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    position = 0
    pending = ""
    scene_samples = 0
    try:
        while identity(pid) == owner and time.monotonic() - began < args.seconds:
            log = report / "game.log"
            if meta["scene_anchor"] is None and log.exists():
                with log.open(errors="replace") as stream:
                    stream.seek(position)
                    pending += stream.read()
                    position = stream.tell()
                lines = pending.split("\n")
                pending = lines.pop()
                for line in lines:
                    match = re.search(r"Native API frame (\d+) completed (\d+) operations", line)
                    if match:
                        scene_samples = scene_samples + 1 if int(match[2]) >= 600 else 0
                        if scene_samples >= 3:
                            anchor = time.monotonic()
                            meta["scene_anchor"] = {"observed_utc": datetime.now(timezone.utc).isoformat(),
                                "monitor_elapsed": anchor - began, "frame": int(match[1]),
                                "log_line": line, "heuristic": "Three consecutive sampled frames with at least600operations"}
                            if boot.PERF.is_file():
                                for index, delay in enumerate((10, 65, 130)):
                                    folder = report / f"cpu-{index + 1}"
                                    folder.mkdir()
                                    profiles.append(boot.CpuProfile(folder, anchor, {"at": delay, "seconds": 15}))
                            else:
                                meta["profile_error"] = "perf executable is unavailable"
                            save()
                            break
            for profile in profiles:
                profile.poll(pid)
            time.sleep(.5)
    finally:
        for profile in profiles:
            profile.stop()
        if sampler.poll() is None:
            sampler.terminate()
            try:
                sampler.wait(timeout=2)
            except subprocess.TimeoutExpired:
                sampler.kill()
                sampler.wait()
        sampler_log.close()
        meta.update(monitor_end_utc=datetime.now(timezone.utc).isoformat(),
                    elapsed_seconds=time.monotonic() - began,
                    game_still_running=identity(pid) == owner)
        save()


if __name__ == "__main__":
    main()
