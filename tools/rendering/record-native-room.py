#!/usr/bin/env python3
"""Private bounded portal recorder; never controls the user's OBS process.

Start after the Gears window exists, before asking the user to play. The portal
asks the user to choose Gears of War 3 from its Window tab. Recording readiness
is proven only by gpu-screen-recorder's first-frame timestamp plus output bytes.
This helper does not infer the chosen window from a portal node ID; visually
confirm the first preview if the portal offers monitor sources too.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


def wall():
    return datetime.now(timezone.utc).isoformat(timespec="microseconds")


def stamp():
    return {"utc": wall(), "monotonic_ns": time.monotonic_ns(), "realtime_ns": time.time_ns()}


def identity(pid):
    try:
        return {"exe": str(Path(f"/proc/{pid}/exe").resolve(strict=True)),
                "starttime_ticks": Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19]}
    except (OSError, IndexError):
        return None


def first_frame(path):
    try:
        lines = path.read_text().strip().splitlines()
        if len(lines) != 2 or lines[0].split() != ["monotonic_microsec", "realtime_microsec"]:
            return None
        mono, real = map(int, lines[1].split())
        if min(mono, real) <= 0:
            return None
        return {"monotonic_microsec": mono, "realtime_microsec": real,
                "utc": datetime.fromtimestamp(real / 1_000_000, timezone.utc).isoformat(timespec="microseconds")}
    except (OSError, ValueError):
        return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path, help="Existing unique diagnostic report directory")
    parser.add_argument("--game-pid", type=int, required=True)
    parser.add_argument("--seconds", type=int, default=600, help="Maximum seconds after first video frame (30..600)")
    parser.add_argument("--selection-timeout", type=int, default=120)
    parser.add_argument("--recorder", default="/usr/bin/gpu-screen-recorder")
    parser.add_argument("--size", default="1920x1080")
    args = parser.parse_args()
    if not 30 <= args.seconds <= 600 or not 10 <= args.selection_timeout <= 300:
        parser.error("Recording/selection bounds invalid")
    report = args.report.resolve(strict=True)
    if not report.is_dir():
        parser.error("Report is not a directory")
    game = identity(args.game_pid)
    if not game or Path(game["exe"].removesuffix(" (deleted)")).name != "gow3":
        parser.error("PID is not a live gow3 process")
    video = report / "gameplay.mkv"
    first = report / "gameplay.mkv.ts"
    metadata = report / "video-capture.json"
    log_path = report / "video-capture.log"
    ready = report / "video-ready.json"
    marker = report / "video-stop.request"
    for path in [video, first, metadata, log_path, ready, marker]:
        if path.exists():
            parser.error(f"Refusing to overwrite {path.name}")
    state = {"schema": 1, "game_pid": args.game_pid, "game_identity": game,
             "requested": stamp(), "maximum_recording_seconds": args.seconds,
             "selection_timeout_seconds": args.selection_timeout, "status": "awaiting_portal_selection",
             "source": "user-selected portal stream; choose Gears window", "window_selection_verified": False,
             "normal_hdr_game_unchanged": True,
             "video_color_limit": "Recorder portal capture is SDR; this video is for timing and artifacts, not HDR color validation",
             "audio": "disabled to avoid unrelated desktop audio", "obs_changed": False,
             "output": str(video), "first_frame_sidecar": str(first), "recorder_log": str(log_path)}

    def save():
        tmp = metadata.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(state, indent=2) + "\n")
        tmp.replace(metadata)

    stopping = False

    def request_stop(signum, frame):
        nonlocal stopping
        stopping = True
        state["signal"] = signum

    signal.signal(signal.SIGINT, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    old_umask = os.umask(0o077)
    process = None
    try:
        with tempfile.TemporaryDirectory(prefix="gow3-video-portal-") as private:
            token_path = str(Path(private) / "restore-token")
            command = [args.recorder, "-w", "portal", "-restore-portal-session", "no",
                       "-portal-session-token-filepath", token_path, "-c", "mkv", "-f", "60",
                       "-s", args.size, "-fm", "content", "-k", "h264", "-encoder", "gpu",
                       "-fallback-cpu-encoding", "no", "-q", "high", "-tune", "performance",
                       "-cursor", "no", "-write-first-frame-ts", "yes", "-o", str(video)]
            # Token contents stay private and are deleted after recorder exit.
            state["command"] = ["<private-token-path>" if value == token_path else value for value in command]
            save()
            with log_path.open("x") as log:
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                state["recorder_pid"] = process.pid
                state["recorder_started"] = stamp()
                save()
                print(json.dumps({"status": state["status"], "metadata": str(metadata),
                                  "instruction": "Select only the Gears of War 3 window in the portal dialog."}), flush=True)
                selected_at = None
                began = time.monotonic()
                while process.poll() is None:
                    if stopping or marker.exists():
                        state["stop_reason"] = "explicit_request"
                        break
                    if identity(args.game_pid) != game:
                        state["stop_reason"] = "game_exited"
                        break
                    now = time.monotonic()
                    if selected_at is None:
                        frame = first_frame(first)
                        if frame and video.exists() and video.stat().st_size > 0:
                            selected_at = frame["monotonic_microsec"] / 1_000_000
                            # A .ts file is emitted on the first captured frame,
                            # not merely after process startup or portal opening.
                            state["first_frame"] = frame
                            state["ready_observed"] = stamp()
                            state["status"] = "recording"
                            save()
                            ready.write_text(json.dumps({"status": "recording", "first_frame": frame,
                                                         "observed": state["ready_observed"],
                                                         "video": str(video)}, indent=2) + "\n")
                            print(json.dumps({"status": "recording", "ready": str(ready),
                                              "first_frame": frame}), flush=True)
                        elif now - began >= args.selection_timeout:
                            state["stop_reason"] = "portal_selection_or_first_frame_timeout"
                            break
                    elif now - selected_at >= args.seconds:
                        state["stop_reason"] = "duration_limit"
                        break
                    time.sleep(0.1)
                if process.poll() is None:
                    process.send_signal(signal.SIGINT)
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        state["forced_termination"] = True
                        process.terminate()
                        try:
                            process.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
                state["returncode"] = process.returncode
                state["finished"] = stamp()
                state.setdefault("stop_reason", "recorder_exited")
                state["video_bytes"] = video.stat().st_size if video.exists() else 0
                state["status"] = "recorded_unverified" if state.get("first_frame") and state["video_bytes"] > 0 else "failed"
                save()
        if state["status"] == "recorded_unverified":
            # Runs only after recording has stopped; no decoding during gameplay.
            result = subprocess.run(["/usr/bin/ffprobe", "-v", "error", "-show_entries",
                "format=duration,size:stream=codec_type,codec_name,width,height,avg_frame_rate,color_space,color_transfer",
                "-of", "json", str(video)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=15)
            state["ffprobe_returncode"] = result.returncode
            if result.returncode == 0:
                probe = json.loads(result.stdout)
                state["ffprobe"] = probe
                videos = [s for s in probe.get("streams", []) if s.get("codec_type") == "video"]
                if videos and float(probe.get("format", {}).get("duration", 0)) > 0:
                    state["status"] = "recorded"
            else:
                state["ffprobe_error"] = result.stderr[:1000]
            save()
        print(json.dumps({"status": state["status"], "metadata": str(metadata), "bytes": state["video_bytes"]}), flush=True)
        return 0 if state["status"] == "recorded" else 1
    except Exception as error:
        state["status"] = "failed"
        state["error"] = str(error)
        if process is not None and process.poll() is None:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
        save()
        raise
    finally:
        os.umask(old_umask)


if __name__ == "__main__":
    raise SystemExit(main())
