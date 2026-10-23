#!/usr/bin/env python3
"""Boot Native or Xenia in an isolated compositor, without desktop input.

Linux-only diagnostic harness; the renderer itself uses portable Vulkan/C++.
Requires labwc and Mesa lavapipe by default; --hardware uses the system GPU.
Game assets are read from this installation;
configuration, saves and shader caches use a temporary directory.
"""
import argparse
from datetime import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import resource
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
# GOW3_BUILD_DIR selects another build tree (relative to the checkout).
BUILD = ROOT / os.environ.get("GOW3_BUILD_DIR", "build")
PERF = Path("/tmp/gow3-perf/usr/bin/perf")


def user_data_manifest(root):
    """Bounded prelaunch identity for copytree's non-cache profile/save scope."""
    root = Path(root)
    if root.is_symlink() or not root.is_dir():
        raise ValueError("User-data identity requires a real directory")
    def raise_walk_error(error):
        raise error
    records = []
    total = 0
    for directory, directories, names in os.walk(root, followlinks=False, onerror=raise_walk_error):
        directory = Path(directory)
        if len(directory.relative_to(root).parts) > 32:
            raise ValueError("User-data identity depth bound exceeded")
        directories[:] = sorted(name for name in directories if name != "cache")
        if any((directory / name).is_symlink() for name in directories):
            raise ValueError("User-data identity does not follow symlinks")
        for name in sorted(names):
            if name == "cache":
                continue
            path = directory / name
            if path.is_symlink() or not path.is_file():
                raise ValueError("User-data identity requires regular files")
            if len(records) >= 4096:
                raise ValueError("User-data identity file bound exceeded")
            with path.open("rb") as content:
                before = os.fstat(content.fileno())
                if before.st_size > 64 * 1024 * 1024 or total + before.st_size > 256 * 1024 * 1024:
                    raise ValueError("User-data identity byte bound exceeded")
                digest = hashlib.sha256()
                read_bytes = 0
                while chunk := content.read(1024 * 1024):
                    read_bytes += len(chunk)
                    if read_bytes > before.st_size:
                        raise ValueError("User-data file grew during hashing")
                    digest.update(chunk)
                after = os.fstat(content.fileno())
            signature = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_ctime_ns)
            if signature(before) != signature(after) or read_bytes != before.st_size:
                raise ValueError("User-data file changed during hashing")
            total += read_bytes
            records.append({"path": path.relative_to(root).as_posix(), "bytes": read_bytes,
                            "sha256": digest.hexdigest()})
    records.sort(key=lambda item: item["path"])
    canonical = json.dumps(records, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()
    return {"files": records, "bytes": total, "manifest_sha256": hashlib.sha256(canonical).hexdigest()}


def key_sequence_command(sequence):
    """Validate a balanced, bounded sequence and return one owned wtype job."""
    if not isinstance(sequence, list) or not 1 <= len(sequence) <= 128:
        raise ValueError("key_sequence must contain 1-128 steps")
    command = ["wtype"]
    held = set()
    milliseconds = 0
    for step in sequence:
        if not isinstance(step, dict) or len(step) != 1:
            raise ValueError("Each key_sequence step must contain exactly one operation")
        operation, value = next(iter(step.items()))
        if operation == "sleep_ms":
            if type(value) is not int or not 0 <= value <= 60000:
                raise ValueError("sleep_ms must be an integer in 0-60000")
            milliseconds += value
            command.extend(("-s", str(value)))
        elif operation in ("press", "release"):
            if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_+-]{1,64}", value):
                raise ValueError("Sequence keys must be named keysyms")
            if (value in held) == (operation == "press"):
                raise ValueError("Sequence keys must be pressed once before being released")
            if operation == "press":
                held.add(value)
            else:
                held.remove(value)
            command.extend(("-P" if operation == "press" else "-p", value))
        else:
            raise ValueError("Sequence operations are press, release and sleep_ms")
    if held or milliseconds > 60000:
        raise ValueError("Sequence must release every key and last at most 60 seconds")
    return command, milliseconds


def validate_actions(actions, seconds, capture_api):
    if not isinstance(actions, list) or len(actions) > 256:
        raise ValueError("Input sequence must be a list of at most 256 actions")
    captures = 0
    screenshots = set()
    keyboard_until = -1
    for action in actions:
        if not isinstance(action, dict):
            raise ValueError("Input actions must be objects")
        at = action.get("at")
        if type(at) not in (int, float) or not math.isfinite(at) or not 0 <= at < seconds:
            raise ValueError("Action 'at' must be finite, nonnegative and less than --seconds")
    actions = sorted(actions, key=lambda action: action["at"])
    for action in actions:
        if "key" in action and "key_sequence" in action:
            raise ValueError("An action cannot contain both key and key_sequence")
        duration = 0
        if "key_sequence" in action:
            _, duration = key_sequence_command(action["key_sequence"])
        elif "key" in action:
            if not isinstance(action["key"], str) or not re.fullmatch(r"[A-Za-z0-9_+-]{1,64}", action["key"]):
                raise ValueError("Action keys must be named keysyms")
            duration = action.get("hold_ms", 800)
            if type(duration) is not int or not 0 <= duration <= 60000:
                raise ValueError("hold_ms must be an integer in 0-60000")
        if "key" in action or "key_sequence" in action:
            if action["at"] < keyboard_until:
                raise ValueError("Timed keyboard actions may not overlap")
            keyboard_until = action["at"] + duration / 1000
            if keyboard_until > seconds:
                raise ValueError("Keyboard action must finish within --seconds")
        if "screenshot" in action:
            name = action["screenshot"]
            if not isinstance(name, str) or Path(name).name != name or not name.endswith(".png") or name in screenshots:
                raise ValueError("Screenshot actions must have distinct PNG filenames")
            screenshots.add(name)
        if "capture" in action:
            if action["capture"] is not True or not capture_api:
                raise ValueError("capture actions require --capture-api and capture:true")
            captures += 1
    if captures != int(capture_api):
        raise ValueError("--capture-api requires exactly one capture:true action")
    return actions


def capture_status(settings):
    if settings is None:
        return None
    result = dict(settings, completed=False, history_pair_present=False)
    path = Path(settings["api"])
    result["bytes"] = path.stat().st_size if path.is_file() else 0
    if result["bytes"] >= 32:
        with path.open("rb") as stream:
            magic = stream.read(8)
            stream.seek(-16, 2)
            footer = stream.read(16)
        words = [int.from_bytes(footer[i:i + 4], "little") for i in range(0, 16, 4)]
        result.update(finish_reason=words[2], events=words[3])
        result["completed"] = magic == b"G3D3D001" and words[:3] == [16, 0, 0] and words[3] > 0
    if "history" in settings:
        snapshots = []
        for metadata in sorted(Path(settings["history"]).glob("*.xtr.initial.json")):
            item = json.loads(metadata.read_text())
            trace = metadata.with_suffix("").with_suffix("")
            snapshots.append({"metadata": str(metadata), "trace": str(trace), "frame": item.get("frame"),
                              "provenance": item.get("provenance"),
                              "bytes": trace.stat().st_size if trace.is_file() else 0})
        result["history_snapshots"] = snapshots
        frames = sorted(item["frame"] for item in snapshots if type(item["frame"]) is int)
        result["history_pair_present"] = (len(snapshots) == len(frames) == 2 and frames[1] == frames[0] + 1
                                          and all(item["bytes"] > 0 and item["provenance"] == "reference_gpu_frame_boundary"
                                                  for item in snapshots))
        # Presence is not content/coverage validation. The importer independently
        # checks frame alignment and captured memory before accepting history.
        result["history_contents_validated"] = False
    return result


class InputJobs:
    """Only processes started for this isolated seat; never signal other input."""

    def __init__(self, folder, env, began):
        self.folder, self.env, self.began = folder, env, began
        self.jobs = []

    def start(self, sequence, entry):
        self.poll()
        if any(process.poll() is None for process, _, _ in self.jobs):
            raise ValueError("The preceding asynchronous keyboard action is still running")
        command, _ = key_sequence_command(sequence)
        output = (self.folder / f"input-job-{len(self.jobs):03d}.log").open("x")
        try:
            process = subprocess.Popen(command, env=self.env, stdout=output, stderr=subprocess.STDOUT,
                                       start_new_session=True)
        except OSError:
            output.close()
            raise
        entry.update(started=True, completed=False, pid=process.pid, command=command)
        self.jobs.append((process, output, entry))

    def poll(self):
        for process, output, entry in self.jobs:
            if process.poll() is not None and "returncode" not in entry:
                process.wait()
                output.close()
                entry.update(returncode=process.returncode, completed=process.returncode == 0,
                             finished_elapsed=time.monotonic() - self.began)
                if process.returncode:
                    entry["error"] = f"Input job exited with status {process.returncode}"

    def stop(self):
        for process, _, entry in self.jobs:
            if process.poll() is None:
                entry["interrupted_at_game_end"] = True
                # Disconnecting this private virtual keyboard releases its held
                # keys. Reap every child even if the compositor interrupts us.
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        self.poll()


class CpuProfile:
    """One bounded perf attachment to this test's game process only."""

    def __init__(self, folder, began, settings):
        self.folder = folder
        self.began = began
        self.settings = settings
        self.process = None
        self.output = None
        self.result = {"record_schema": 2, "requested_at_seconds": settings["at"],
                       "requested_duration_seconds": settings["seconds"],
                       "switch_events": bool(settings.get("switch_events", False)), "started": False}
        self.save()

    def save(self):
        (self.folder / "cpu-profile.json").write_text(json.dumps(self.result, indent=2) + "\n")

    def poll(self, game_pid):
        if not self.result["started"] and "error" not in self.result and time.monotonic() - self.began >= self.settings["at"]:
            command = [str(PERF), "record", "-e", "cycles:u", "-F", "99", "--call-graph", "dwarf,32768"]
            if self.result["switch_events"]:
                # Extra diagnostic records remain separate from sampled cycle
                # stacks. No game thread, renderer or benchmark option changes.
                command += ["--switch-events", "--sample-cpu", "--timestamp"]
            if self.settings.get("close_at") is not None:
                # The target's clean exit ends attachment. A separate sleep
                # workload would be SIGTERM'd by perf and propagate status -15.
                command += ["--clockid", "mono"]
                self.result.update(event_clock="CLOCK_MONOTONIC", mmap_pages="perf_default",
                                   lifetime="target_exit_or_harness_duration_limit")
            command += ["-p", str(game_pid), "-o", str(self.folder / "gameplay.data")]
            if self.settings.get("close_at") is None:
                command += ["--", "sleep", str(self.settings["seconds"])]
            self.result.update(command=command, game_pid=game_pid,
                               start_elapsed_seconds=time.monotonic() - self.began,
                               start_steady_ns=time.monotonic_ns(),
                               start_wall_time=datetime.now().isoformat(timespec="microseconds"))
            try:
                self.output = (self.folder / "perf.log").open("w")
                self.process = subprocess.Popen(command, stdout=self.output, stderr=subprocess.STDOUT,
                                                start_new_session=True)
                self.result.update(started=True, profiler_pid=self.process.pid)
            except OSError as error:
                self.result["error"] = str(error)
                if self.output:
                    self.output.close()
            self.save()
        if (self.settings.get("close_at") is not None and self.process is not None and
                self.process.poll() is None and time.monotonic() - self.began >=
                self.result["start_elapsed_seconds"] + self.settings["seconds"]):
            # Keep a failed/slow UI-close diagnostic bounded without a sleep
            # child. This is a failed/truncated profile, never a clean record.
            self.result["profile_duration_limit_reached"] = True
            self.stop()
        if self.process is not None and self.process.poll() is not None and "returncode" not in self.result:
            self.finish()

    def finish(self):
        self.result.update(returncode=self.process.returncode,
                           end_elapsed_seconds=time.monotonic() - self.began,
                           end_steady_ns=time.monotonic_ns(),
                           end_wall_time=datetime.now().isoformat(timespec="microseconds"))
        self.output.close()
        self.save()

    def wait_after_profile_close(self):
        # Optional diagnostic UI close occurs while perf is still sampling.
        # Allow attachment to drain/finalize after the target exits; there is
        # no auxiliary workload in this mode.
        if self.process is not None and self.process.poll() is None:
            deadline = (self.began + self.result["start_elapsed_seconds"] +
                        self.settings["seconds"] + 2)
            try:
                self.process.wait(timeout=max(0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                self.result["profile_close_finish_timeout"] = True
                self.save()

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            self.result["interrupted_at_game_end"] = True
            # SIGINT lets perf finish its data file. Signal only the process
            # group created above, including its bounded sleep child.
            for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGKILL):
                try:
                    os.killpg(self.process.pid, sig)
                except ProcessLookupError:
                    break
                try:
                    self.process.wait(timeout=2)
                    break
                except subprocess.TimeoutExpired:
                    continue
            self.process.wait()
        if self.process is not None and "returncode" not in self.result:
            self.finish()
        elif not self.result["started"]:
            self.result.setdefault("error", "Game ended before the scheduled profiling window")
            self.save()


def mark_profile_overlap(samples, window, starts=None):
    """Conservatively exclude aggregates spanning any profiling overhead."""
    previous = 0.0
    for sample in samples:
        end = sample.get("elapsed_seconds")
        begin = starts.get(sample.get("frame"), 0.0) if starts is not None else previous
        overlap = bool(window and (end is None or begin <= window[1] and end >= window[0]))
        sample["profile_overlap"] = overlap
        sample["include_in_timing_comparison"] = not overlap
        if end is not None:
            previous = end


def copy_mesa_cache(source, destination):
    # Diagnostic harness only: normal game launches already use the driver's
    # persistent cache. A fresh private XDG_CACHE_HOME otherwise makes every
    # validation run cold at the driver layer, even with warm native SPIR-V.
    # Copy only the explicitly selected Mesa cache, never the whole desktop cache.
    shutil.copytree(source, destination, dirs_exist_ok=True,
                    ignore=lambda directory, names: [name for name in names
                        if (Path(directory) / name).is_symlink()])
    files = [path for path in destination.rglob("*") if path.is_file()]
    return {"source": str(source), "destination": str(destination),
            "files": len(files), "bytes": sum(path.stat().st_size for path in files),
            "validation": "Driver-owned cache format; Mesa validates entries when read"}


def copy_pipeline_cache(source, destination):
    """Copy bounded observed recipes for this compiler into a private cache."""
    embedded = (BUILD / "native_shader_embedded.h").read_text()
    version = re.search(r'kNativeShaderVersion\[\] = "([0-9a-f]{64})"', embedded)[1]
    relative = Path("v1") / version
    directory = source / relative
    records = []
    source_bytes = source_count = 0
    if any(path.is_symlink() for path in (source, source / "v1", directory)):
        raise ValueError("Pipeline cache directories must not be symbolic links")
    for path in sorted(directory.glob("*")):
        manifest = path.name == "recipes.json"
        if not manifest and not re.fullmatch(r"[0-9a-f]{64}\.bin", path.name):
            continue
        if path.is_symlink() or not path.is_file():
            continue
        size = path.stat().st_size
        if size > (2 if manifest else 16) * 1024 * 1024:
            raise ValueError("Pipeline cache file exceeds its runtime bound")
        if not manifest:
            source_bytes += size
            source_count += 1
            if source_bytes > 64 * 1024 * 1024 or source_count > 1024:
                raise ValueError("Pipeline source cache exceeds its runtime budget")
        data = path.read_bytes()
        target = destination / relative / path.name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        records.append({"file": str(relative / path.name), "bytes": len(data),
                        "sha256": hashlib.sha256(data).hexdigest()})
    return {"source": str(source), "destination": str(destination), "compiler_version": version,
            "files": len(records), "bytes": sum(item["bytes"] for item in records),
            "manifest_sha256": hashlib.sha256(json.dumps(records, sort_keys=True).encode()).hexdigest(),
            "validation": "Runtime validates recipe schema and source hashes; only bounded committed files copied"}


def copy_native_cache(source, destination):
    """Copy complete current-compiler entries, leaving the source untouched."""
    embedded = (BUILD / "native_shader_embedded.h").read_text()
    version = re.search(r'kNativeShaderVersion\[\] = "([0-9a-f]{64})"', embedded)[1]
    records = []
    skipped = 0
    for path in sorted((source / version).glob("*.g3spv")):
        if path.is_symlink() or not path.is_file() or not 152 < path.stat().st_size <= 152 + 16 * 1024 * 1024:
            skipped += 1
            continue
        data = path.read_bytes()
        name = re.fullmatch(r"([0-9a-f]{64})(\.fsi)?\.g3spv", path.name)
        # Match the runtime's serialized-cache integrity checks. No temporary
        # compiler outputs or extracted asset containers belong in this cache.
        valid = (name and 152 < len(data) <= 152 + 16 * 1024 * 1024
                 and data[:8] == b"G3SPV003"
                 and int.from_bytes(data[16:20], "little") * 4 == len(data) - 152
                 and data[24:88] == name[1].encode()
                 and int.from_bytes(data[20:24], "little") == bool(name[2])
                 and data[152:156] == b"\x03\x02\x23\x07"
                 and hashlib.sha256(data[8:88] + data[152:]).hexdigest().encode() == data[88:152])
        if not valid:
            skipped += 1
            continue
        target = destination / version / path.name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        records.append({"file": str(Path(version) / path.name), "bytes": len(data),
                        "sha256": hashlib.sha256(data).hexdigest()})
    return {"source": str(source), "destination": str(destination), "compiler_version": version,
            "files": len(records), "bytes": sum(item["bytes"] for item in records),
            "skipped": skipped,
            "manifest_sha256": hashlib.sha256(json.dumps(records, sort_keys=True).encode()).hexdigest()}


def copy_fsi_state_cache(source, destination, *, shaders=False, forward_cfg=False):
    """Keep the canonical-source sidecar and derived-module namespaces separate."""
    if forward_cfg and not shaders:
        raise ValueError("The composed cache contains shaders, never ranking recipes")
    header_name = ("native_fsi_state_forward_cfg_shader_version.h" if forward_cfg
                   else "native_fsi_state_specialization_shader_version.h")
    version_name = ("kNativeFsiStateForwardCfgShaderVersion" if forward_cfg
                    else "kNativeFsiStateSpecializationShaderVersion")
    header = (BUILD / header_name).read_text()
    version = re.search(re.escape(version_name) + r'\[\] = "([0-9a-f]{64})"', header)[1]
    extension = ".g3fsifwd" if forward_cfg else ".g3fsi"
    magic = b"G3FFWD01" if forward_cfg else b"G3FSIC01"
    relative = Path(version) if shaders else Path("v1") / version
    directory = source / relative
    if any(path.is_symlink() for path in (source, source / "v1", directory)):
        raise ValueError("FSI state cache directories must not be symbolic links")
    records, skipped = [], 0
    total = 0
    for path in sorted(directory.glob("*" + extension) if shaders else directory.glob("states.json")):
        limit = 400 + 16 * 1024 * 1024 if shaders else 512 * 1024
        if path.is_symlink() or not path.is_file() or path.stat().st_size > limit:
            skipped += 1
            continue
        data = path.read_bytes()
        if shaders:
            name = re.fullmatch(r"([0-9a-f]{64})" + re.escape(extension), path.name)
            valid = (name and len(data) >= 420 and data[:8] == magic
                     and int.from_bytes(data[12:16], "little") * 4 == len(data) - 400
                     and data[16:80] == name[1].encode() and data[80:144] == version.encode()
                     and (not forward_cfg or name[1] == "49f26283e1436f4cce68ac4e75695c6375176768b1dbfbf6c1f79d9bef434ced")
                     and data[400:404] == b"\x03\x02\x23\x07"
                     and hashlib.sha256(data[:336] + data[400:]).hexdigest().encode() == data[336:400])
        else:
            try:
                value = json.loads(data)
                valid = (value.get("schema") == 1 and value.get("variant") == version
                         and isinstance(value.get("records"), list) and len(value["records"]) <= 512)
            except (ValueError, AttributeError):
                valid = False
        if not valid or shaders and (len(records) >= 256 or total + len(data) > 16 * 1024 * 1024):
            skipped += 1
            continue
        target = destination / relative / path.name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        total += len(data)
        records.append({"file": str(relative / path.name), "bytes": len(data),
                        "sha256": hashlib.sha256(data).hexdigest()})
    return {"source": str(source), "destination": str(destination), "variant_version": version,
            "files": len(records), "bytes": total, "skipped": skipped,
            "manifest_sha256": hashlib.sha256(json.dumps(records, sort_keys=True).encode()).hexdigest(),
            "validation": "Runtime authenticates exact tuples against generic source recipes and validates derived SPIR-V"}


def child(folder, seconds):
    temporary = Path(folder)
    mode_path = temporary / "output-mode"
    if mode_path.exists():
        # Before the game window exists, so it opens on the requested mode.
        subprocess.run(["wlr-randr", "--output", "HEADLESS-1", "--custom-mode", mode_path.read_text().strip()],
                       check=True, timeout=10)
    env = os.environ.copy()
    env.update(GDK_BACKEND="wayland", SDL_AUDIODRIVER="dummy", DISABLE_OBS_VKCAPTURE="1")
    # Scope allocator experiments to the game. The compositor, input jobs,
    # profiler and capture tools must keep their original allocator.
    game_env = env.copy()
    allocator_path = temporary / "allocator.json"
    allocator = json.loads(allocator_path.read_text()) if allocator_path.exists() else None
    if allocator:
        library = Path(allocator["path"])
        if hashlib.sha256(library.read_bytes()).hexdigest() != allocator["sha256"]:
            raise ValueError("Allocator library changed before game launch")
        game_env["LD_PRELOAD"] = str(library)
    reference_only = (temporary / "reference-only").exists()
    # A cold reference cache must wait for compilation during comparisons;
    # otherwise its skipped draws produce misleading zero-sample queries.
    command = [str(BUILD / "bin/gow3"), f"--game_data_root={ROOT / 'game'}",
               f"--user_data_root={temporary / 'user'}", f"--cache_path={temporary / 'cache'}",
               f"--gow3_config={temporary / 'gow3.toml'}", f"--log_file={temporary / 'game.log'}",
               "--log_max_file_size_mb=64",
               f"--gow3_native_failure_directory={temporary / 'failure'}",
               f"--gow3_native_live={str(not reference_only).lower()}",
               f"--gow3_native_present={str(not reference_only).lower()}", "--audio_mute=true",
               "--input_backend=xinput", "--mnk_mode=true", "--mnk_user_index=0",
               "--async_shader_compilation=false", "--vulkan_async_shader_translation=false",
               "--vulkan_pipeline_creation_threads=0"]
    guest_command_line = temporary / "guest-command-line.txt"
    if guest_command_line.exists():
        command.append("--cl=" + guest_command_line.read_text())
    if (temporary / "trace-input").exists():
        command.append("--mnk_trace_input=true")
    probe_path = temporary / "texture-probe-settings.json"
    if probe_path.exists():
        probe = json.loads(probe_path.read_text())
        command.extend((f"--gow3_native_texture_probe_directory={probe['directory']}",
                        f"--gow3_native_texture_probe_hashes={probe['hashes']}"))
    if (temporary / "native-ownership").exists():
        command.append("--gow3_native_ownership=true")
    elif reference_only:
        command.append("--gow3_native_ownership=false")
    # The private config selects exactly one renderer. Avoid pinning this on
    # the CLI so the overlay's restart-only selector remains testable.
    command.append("--gow3_native_draw_batch_size=" + (temporary / "draw-batch-size").read_text().strip())
    capture_path = temporary / "capture-settings.json"
    capture = json.loads(capture_path.read_text()) if capture_path.exists() else None
    if capture:
        command.extend((f"--gow3_renderer_capture={capture['api']}", "--gow3_renderer_capture_frames=2",
                        f"--gow3_renderer_capture_trigger={capture['trigger']}"))
        if "history" in capture:
            command.append(f"--gow3_renderer_history={capture['history']}")
    if (temporary / "fallback-settings.json").exists():
        for name,value in json.loads((temporary / "fallback-settings.json").read_text()).items():
            if isinstance(value, bool):
                value = str(value).lower()
            command.append(f"--{name}={value}")
    with (temporary / "console.log").open("w") as output:
        def disable_test_core_dump():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        game = subprocess.Popen(command, cwd=ROOT, env=game_env, stdout=output, stderr=subprocess.STDOUT,
                                preexec_fn=disable_test_core_dump)
        actions_path = temporary / "input-sequence.json"
        actions = json.loads(actions_path.read_text()) if actions_path.exists() else []
        action_log = []
        close_requested = False
        close_began = None
        shutdown_diagnosed = False
        keyboard_primed = False
        began = time.monotonic()
        input_jobs = InputJobs(temporary, env, began)
        profile_path = temporary / "profile-settings.json"
        profile = CpuProfile(temporary, began, json.loads(profile_path.read_text())) if profile_path.exists() else None
        # The private compositor may stop the child on timeout. Run the same
        # owned-process cleanup then, so a profiler cannot outlive this test.
        def terminate_child(signum, _frame):
            raise SystemExit(128 + signum)
        signal.signal(signal.SIGTERM, terminate_child)
        signal.signal(signal.SIGINT, terminate_child)
        startup_close = (temporary / "close-during-pipeline-prewarm").exists()
        input_anchor = None if startup_close or (temporary / "inputs-after-scene").exists() else began
        try:
            while game.poll() is None and time.monotonic() - began < seconds:
                elapsed = time.monotonic() - began
                if allocator and not allocator.get("mapped"):
                    try:
                        maps = Path(f"/proc/{game.pid}/maps").read_text()
                        allocator["mapped"] = any(line.split(maxsplit=5)[-1] == allocator["path"]
                            for line in maps.splitlines() if len(line.split(maxsplit=5)) == 6)
                        if allocator["mapped"]:
                            allocator.update(pid=game.pid, observed_elapsed_seconds=elapsed)
                            allocator_path.write_text(json.dumps(allocator, indent=2) + "\n")
                    except OSError:
                        pass
                input_jobs.poll()
                if profile:
                    profile.poll(game.pid)
                    close_at = profile.settings.get("close_at")
                    if (close_at is not None and profile.result.get("started") and
                            profile.process is not None and profile.process.poll() is None and
                            not profile.result.get("profile_close_attempted", False) and
                            time.monotonic() - began >= profile.result["start_elapsed_seconds"] + close_at):
                        profile.result.update(profile_close_attempted=True,
                                              close_request_elapsed_seconds=time.monotonic() - began,
                                              close_request_steady_ns=time.monotonic_ns())
                        try:
                            subprocess.run(["wlrctl", "toplevel", "close", "app_id:gow3"], env=env, check=True, timeout=2)
                            close_requested = True
                            close_began = time.monotonic()
                            profile.result["profile_close_requested"] = True
                        except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
                            profile.result["profile_close_error"] = str(error)
                        profile.save()
                if (close_began is not None and not shutdown_diagnosed and
                        (temporary / "diagnose-shutdown").exists() and
                        time.monotonic() - close_began >= 3):
                    shutdown_diagnosed = True
                    diagnostic = {"pid": game.pid, "seconds_after_close": time.monotonic() - close_began}
                    try:
                        stack = subprocess.run(["gdb", "-q", "-batch", "-p", str(game.pid),
                                                "-ex", "set pagination off", "-ex", "thread apply all bt"],
                                               capture_output=True, text=True, timeout=8)
                        (temporary / "shutdown-stack.log").write_text(stack.stdout + stack.stderr)
                        diagnostic["returncode"] = stack.returncode
                    except (OSError, subprocess.TimeoutExpired) as error:
                        diagnostic["error"] = str(error)
                    (temporary / "shutdown-diagnostic.json").write_text(json.dumps(diagnostic, indent=2) + "\n")
                if input_anchor is None and (temporary / "game.log").exists():
                    current_log = (temporary / "game.log").read_text(errors="replace")
                    ready = ("Initializing shader storage for title" in current_log) if startup_close else any(
                        int(count) > 4 for count in re.findall(r"Native API frame \d+ completed (\d+) operations", current_log))
                    if ready:
                        input_anchor = time.monotonic()
                        (temporary / "input-anchor.json").write_text(json.dumps({"elapsed": elapsed}) + "\n")
                input_elapsed = time.monotonic() - input_anchor if input_anchor is not None else -1
                while actions and actions[0]["at"] <= input_elapsed:
                    action = actions.pop(0)
                    entry = {"requested": action, "elapsed": elapsed}
                    try:
                        if "key" in action or "key_sequence" in action:
                            if (temporary / "trace-input").exists():
                                entry["toplevels"] = subprocess.run(["wlrctl", "toplevel", "list"], env=env,
                                                                   check=True, capture_output=True, text=True).stdout
                            subprocess.run(["wlrctl", "toplevel", "focus", "app_id:gow3"], env=env, check=True)
                            if not keyboard_primed:
                                # The first virtual keyboard activates a newly
                                # mapped GTK window; its first key-down can be
                                # consumed by that activation. Prime the private
                                # seat before sending the actual game button.
                                subprocess.run(["wtype", "-k", "Control_R", "-s", "250"], env=env, check=True)
                                keyboard_primed = True
                            if "key_sequence" in action:
                                input_jobs.start(action["key_sequence"], entry)
                            else:
                                if any(process.poll() is None for process, _, _ in input_jobs.jobs):
                                    raise ValueError("The preceding asynchronous keyboard action is still running")
                                subprocess.run(["wtype", "-P", action["key"], "-s", str(action.get("hold_ms", 800)),
                                                "-p", action["key"]], env=env, check=True)
                        if action.get("capture") is True:
                            if not capture:
                                raise ValueError("Capture action has no configured output")
                            with Path(capture["trigger"]).open("x") as trigger:
                                trigger.write(json.dumps({"elapsed_seconds": time.monotonic() - began}) + "\n")
                            entry["capture_trigger"] = capture["trigger"]
                        if action.get("close") is True:
                            subprocess.run(["wlrctl", "toplevel", "close", "app_id:gow3"], env=env, check=True)
                            close_requested = True
                            close_began = time.monotonic()
                        if "screenshot" in action:
                            name = Path(action["screenshot"]).name
                            if name != action["screenshot"] or not name.endswith(".png"):
                                raise ValueError("Screenshot action must name a PNG file")
                            subprocess.run(["grim", str(temporary / name)], env=env, check=True)
                        entry.setdefault("completed", True)
                    except (OSError, ValueError, subprocess.CalledProcessError) as error:
                        entry["error"] = str(error)
                    action_log.append(entry)
                    (temporary / "input-results.json").write_text(json.dumps(action_log, indent=2) + "\n")
                time.sleep(.1)
            status = game.poll()
        finally:
            input_jobs.stop()
            (temporary / "input-results.json").write_text(json.dumps(action_log, indent=2) + "\n")
            if profile:
                try:
                    if profile.result.get("profile_close_requested") and game.poll() is not None:
                        profile.wait_after_profile_close()
                finally:
                    profile.stop()
            if game.poll() is None:
                game.terminate()
                try:
                    game.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    game.kill()
                    game.wait()
    (temporary / "status.json").write_text(json.dumps({"early_exit": status, "close_requested": close_requested, "clean_ui_close": close_requested and status == 0}) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=35)
    parser.add_argument("--config", action="append", default=[], metavar="KEY=VALUE",
                        help="Set any game config value in this test's private configuration (repeatable; JSON values)")
    parser.add_argument("--child", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--output", type=Path, default=ROOT / "reports/rendering/native-renderer/native-boot")
    parser.add_argument("--input-sequence", type=Path, help="JSON list of timed keys/screenshots in the private compositor")
    parser.add_argument("--guest-command-line", default="", help="Pass the original game's command line through SDK --cl for an isolated engine-command test")
    parser.add_argument("--copy-user-data", action="store_true", help="Copy local game saves/profiles into the disposable test installation")
    parser.add_argument("--ownership", action="store_true", help="Legacy option; Native always owns scene rendering")
    parser.add_argument("--trace-input", action="store_true", help="Log bounded MnK keyboard/focus/polling diagnostics in the isolated test")
    parser.add_argument("--hardware", action="store_true", help="Use the system Vulkan driver with the same isolated compositor and inputs")
    parser.add_argument("--raster", action="store_true", help="Exercise native raster/query authority using the production renderer selector")
    parser.add_argument("--reference-only", action="store_true", help="Use only the Xenia renderer; success requires a clean UI close")
    parser.add_argument("--weapon-input", action="store_true", help="Map left/right triggers to F8/F9 in this test's private configuration")
    parser.add_argument("--camera-input", action="store_true", help="Map right-stick up/down/left/right to I/K/J/L in this test's private configuration")
    parser.add_argument("--texture-probe-hashes", default="", help="Comma-separated SHA256 base payloads to probe under fresh OUTPUT/texture-probes (maximum 32)")
    parser.add_argument("--capture-api", action="store_true", help="Retain a two-frame API capture under a new OUTPUT/capture directory; requires one capture:true input action")
    parser.add_argument("--capture-reference-history", action="store_true", help="Capture paired original GPU history; requires --reference-only and --capture-api")
    parser.add_argument("--draw-batch-size", type=int, default=64, help="Native draw batch size, 1-64; one is the same-binary baseline")
    parser.add_argument("--native-query-capacity", type=int, choices=range(1, 129), metavar="1-128", help="Override the native logical query limit for a same-binary pressure comparison")
    parser.add_argument("--native-submission-capacity", type=int, choices=(64, 128), help="Override physical GPU slots; two async batches share this capacity")
    parser.add_argument("--native-snapshot-workers", type=int, default=None, help="Override exact snapshot comparison helpers; runtime bounds allocation by hardware threads minus 2")
    parser.add_argument("--native-direct-resource-refs", action=argparse.BooleanOptionalAction, default=None,
                        help="Use exact direct canonical-entry handoff for typed producer bindings")
    parser.add_argument("--native-lazy-state-identities", action=argparse.BooleanOptionalAction, default=None, help="Override on-demand canonical state hashing")
    parser.add_argument("--native-shader-prewarm", action=argparse.BooleanOptionalAction, default=None, help="Override early background native shader compilation")
    parser.add_argument("--native-retain-fsi-raster-extent", action=argparse.BooleanOptionalAction, default=None, help="Reuse a bounded larger dummy raster attachment with exact guest clipping")
    parser.add_argument("--native-cache-texture-layouts", action=argparse.BooleanOptionalAction, default=None, help="Cache immutable exact fetch-word texture layout metadata")
    parser.add_argument("--native-queued-volume-alias-loads", action=argparse.BooleanOptionalAction, default=None, help="Queue bounded GPU alias volumes without per-slice CPU waits")
    parser.add_argument("--native-cache-gpu-alias-states", action=argparse.BooleanOptionalAction, default=None, help="Reuse exact GPU alias state only while physical memory serial is unchanged")
    parser.add_argument("--native-fast-snapshot-fingerprints", action=argparse.BooleanOptionalAction, default=None, help="Use faster lookup hints with exact snapshot byte verification")
    parser.add_argument("--native-shader-lookup-lookaside", action=argparse.BooleanOptionalAction, default=None, help="Reuse exact resident shader lookup results without changing cache ownership")
    parser.add_argument("--native-coalesced-texture-ranges", action=argparse.BooleanOptionalAction, default=None, help="Compute exact whole-tile/linear-row footprint runs")
    parser.add_argument("--native-range-texture-versions", action=argparse.BooleanOptionalAction, default=None, help="Reuse exact texture versions across unrelated writes (experimental; default OFF)")
    parser.add_argument("--native-block-page-versions", action=argparse.BooleanOptionalAction, default=None, help="Use exact uniform-block page version tracking (experimental; default OFF)")
    parser.add_argument("--native-compact-cpu-texture-version-pages", action=argparse.BooleanOptionalAction, default=None, help="Scan each exact sampled history page once per texture version rescan")
    parser.add_argument("--native-associative-texture-layouts", action=argparse.BooleanOptionalAction, default=None, help="Use four-way immutable texture layout metadata with the same bounds")
    parser.add_argument("--native-recycle-resolve-images", action=argparse.BooleanOptionalAction, default=None, help="Reuse fully retired native resolve images within fixed count and allocation budgets")
    parser.add_argument("--native-recycle-resolve-descriptors", action=argparse.BooleanOptionalAction, default=None, help="Reuse fence-retired native compute descriptors within bounded pools")
    parser.add_argument("--native-recycle-pass-transfer-descriptors", action=argparse.BooleanOptionalAction, default=None, help="Reuse fence-retired native graphics transfer descriptor backings")
    parser.add_argument("--native-recycle-pass-transfer-render-passes", action=argparse.BooleanOptionalAction, default=None, help="Reuse exact immutable native graphics transfer render-pass descriptions")
    parser.add_argument("--native-reuse-resolve-identity-views", action=argparse.BooleanOptionalAction, default=None, help="Reuse equivalent owned image views for native resolve loads")
    parser.add_argument("--native-diagnose-texture-eviction-reloads", action=argparse.BooleanOptionalAction, default=None, help="Record bounded texture eviction/reload causes and overlapping host timings")
    parser.add_argument("--native-query-poll-yield", action=argparse.BooleanOptionalAction, default=None, help="Yield periodically after real pending native query retries")
    parser.add_argument("--native-inline-getdata-register-helpers", action=argparse.BooleanOptionalAction, default=None, help="Override exact Native GetData helper inlining after source/instruction checks; readiness unchanged")
    parser.add_argument("--native-prewarm-overflow-pipelines", action=argparse.BooleanOptionalAction, default=None, help="Prepare known generic pipelines beyond resident shader-module startup targets using temporary modules")
    parser.add_argument("--native-pipeline-prewarm", action=argparse.BooleanOptionalAction, default=None, help="Record observed native pipelines and prepare cached recipes before guest launch")
    parser.add_argument("--native-cache-source", type=Path, help="Seed the private cache from a versioned native-shaders directory")
    parser.add_argument("--preserve-native-cache", action="store_true", help="Save completed native shader cache entries under OUTPUT/native-shaders")
    parser.add_argument("--fsi-state-recipe-cache-source", type=Path, help="Seed exact state sidecar from a previous Native run")
    parser.add_argument("--fsi-state-shader-cache-source", type=Path, help="Seed derived specialization modules from a previous Native run")
    parser.add_argument("--fsi-state-forward-shader-cache-source", type=Path, help="Seed composed FSI-state/forward-CFG modules from a separate native-fsi-state-forward-shaders directory")
    parser.add_argument("--preserve-fsi-state-cache", action="store_true", help="Save bounded state sidecar and derived modules after shutdown")
    parser.add_argument("--pipeline-cache-source", type=Path, help="Seed private observed pipeline recipes from a previous diagnostic run")
    parser.add_argument("--preserve-pipeline-cache", action="store_true", help="Save observed native pipeline recipes under OUTPUT/native-pipeline-recipes")
    parser.add_argument("--mesa-cache-source", type=Path, help="Seed the private Mesa driver cache from an existing mesa_shader_cache directory")
    parser.add_argument("--preserve-mesa-cache", action="store_true", help="Save the Mesa driver cache after clean process shutdown under OUTPUT/mesa-cache")
    parser.add_argument("--profile-cpu-seconds", type=int, default=0, help="Record a bounded CPU perf profile (1-60 seconds); zero disables it")
    parser.add_argument("--allocator-library", type=Path,
                        help="Diagnostic only: preload this local allocator into the game process")
    parser.add_argument("--profile-cpu-at", type=float, default=80, help="Start profiling this many seconds after game launch")
    parser.add_argument("--anisotropic-override", type=int, choices=(-1,), default=-1, help="Use original per-texture game filtering; forced filtering is no longer supported")
    parser.add_argument("--output-mode", help="Headless output mode WIDTHxHEIGHT (for example 3840x2160) for upscaling tests")
    parser.add_argument("--frame-limit", type=float, default=0, help="Diagnostic frame cap; unlimited by default (zero)")
    parser.add_argument("--gpu-command-summary", action="store_true", help="Log compatibility command submission counters for performance diagnosis")
    parser.add_argument("--present-gpu-timestamps", action=argparse.BooleanOptionalAction, default=None, help="Observe existing scaler/UI/HDR paint GPU duration without additional waits")
    parser.add_argument("--gpu-native-marker-operands", action=argparse.BooleanOptionalAction, default=False, help="Observe native marker operands and pairs; requires --gpu-command-summary")
    parser.add_argument("--bulk-type0-constants", action=argparse.BooleanOptionalAction, default=None, help="Use existing bulk loader for sequential Type0 constants (experimental; default OFF)")
    parser.add_argument("--native-submit-completion-markers", action=argparse.BooleanOptionalAction, default=None, help="Submit owned Native inputs before exact known completion-marker pairs; requires native cache coherence waits")
    parser.add_argument("--synchronous-native-queries", action="store_true", help="Disable native query deferral for a same-binary baseline")
    parser.add_argument("--synchronous-native-transfers", action="store_true", help="Disable operation-local surface transfer batching for a same-binary baseline")
    parser.add_argument("--separate-native-submissions", action="store_true", help="Disable the shared native draw/transfer queue for a same-binary baseline")
    parser.add_argument("--async-native-submissions", action="store_true", help="Exercise two bounded native GPU batches with asynchronous submission")
    parser.add_argument("--native-gpu-timestamps", action="store_true", help="Measure inclusive GPU intervals and separate host phases without additional fence waits")
    parser.add_argument("--native-gpu-batch-attribution", action="store_true", help="Save bounded GPU batch composition timings; requires the unified native queue")
    parser.add_argument("--native-gpu-physical-timestamps", action="store_true", help="Sample unchanged physical GPU groups and queued transfers")
    parser.add_argument("--native-gpu-physical-first", type=int, default=0)
    parser.add_argument("--native-gpu-physical-end", type=int, default=6000)
    parser.add_argument("--native-gpu-physical-interval", type=int, default=30)
    parser.add_argument("--native-gpu-draw-timestamps", action="store_true", help="Sample inclusive GPU draw-list latency every16th API frame without extra submissions or fence waits")
    parser.add_argument("--native-detailed-reports", action="store_true", help="Keep optional per-draw provenance histories in the native renderer")
    parser.add_argument("--no-native-texture-retention", action="store_true", help="Disable extra dormant CPU texture residency for comparison")
    parser.add_argument("--native-dormant-texture-entries", type=int, default=None,
                        help="Override extra dormant CPU texture entries, 0-2048; 128 MiB and TTL120 remain unchanged")
    parser.add_argument("--native-vertex-retention", action="store_true", help="Enable experimental extra dormant vertex-buffer residency")
    parser.add_argument("--diagnose-shutdown", action="store_true", help="Capture a bounded debugger stack if UI shutdown takes over three seconds")
    parser.add_argument("--fallback-after-packets", type=int, default=0, help="Legacy spelling: force a terminal Native error after N packets; no fallback allowed")
    parser.add_argument("--fallback-query-draws", type=int, default=0, help="Force a terminal Native error after N draws inside an active query")
    parser.add_argument("--fallback-before-query-draw", action="store_true", help="Force a terminal Native error before an active query's first draw")
    parser.add_argument("--fallback-after-deferred-queries", type=int, default=0, help="Force a terminal Native error inside the Nth deferred query-end callback")
    parser.add_argument("--inputs-after-scene", action="store_true", help="Start input timing after the first native frame containing scene operations")
    parser.add_argument("--native-batch-draw-descriptors", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-skip-unused-descriptor-sets", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-cache-draw-descriptors", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-delta-constant-uploads", action=argparse.BooleanOptionalAction, default=None, help="Compare all exact float bits and write only changed blocks in fenced draw slots")
    parser.add_argument("--native-pinned-null-constant-uploads", action=argparse.BooleanOptionalAction, default=None, help="Omit only certified unread float constants for canonical null-pixel draws")
    parser.add_argument("--native-atlas-depth-region-restores", action=argparse.BooleanOptionalAction, default=None, help="Restore only the source rectangle of certified small depth atlas resolves")
    parser.add_argument("--native-fused-atlas-depth-clear", action=argparse.BooleanOptionalAction, default=None, help="Combine canonical depth atlas clear conversions into one compute pass")
    parser.add_argument("--native-direct-atlas-depth-resolves", action=argparse.BooleanOptionalAction, default=None, help="Read canonical depth atlas data directly during resolves")
    parser.add_argument("--native-packed-primary-stencil-clear", action=argparse.BooleanOptionalAction, default=None, help="Normalize packed primary FP24 and clear stencil without temporary depth images")
    parser.add_argument("--native-direct-packed-color-resolves", action=argparse.BooleanOptionalAction, default=None, help="Bypass a redundant host image copy for canonical primary color resolves")
    parser.add_argument("--native-packed-secondary-stencil-clear", action=argparse.BooleanOptionalAction, default=None, help="Normalize packed secondary FP24 and clear stencil while preserving tile padding")
    parser.add_argument("--native-diagnose-cpu-write-cohorts", action=argparse.BooleanOptionalAction, default=None, help="Count consecutive immutable CPU-write operations at execution prefix boundaries")
    parser.add_argument("--native-diagnose-completion-causes", action=argparse.BooleanOptionalAction, default=None, help="Count and time outer Runtime completion causes using existing queue timing counters")
    parser.add_argument("--native-reuse-cpu-write-upload", action=argparse.BooleanOptionalAction, default=None, help="Reuse a fully fenced 4 KiB CPU upload buffer without changing submission order")
    parser.add_argument("--native-canonical-resolve-stores", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-fused-packed-atlas-resolves", action=argparse.BooleanOptionalAction, default=None, help="Combine canonical packed atlas decoding and resolve output in one GPU pass")
    parser.add_argument("--native-packed-zero-depth-clear", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-skip-full-color-clear-restore", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-stencil-export-restores", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-builtin-readonly-depth-queries", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-queue-retained-exports-for-draws", action=argparse.BooleanOptionalAction, default=None, help="Queue retained exports only before ordinary typed Native GPU draws")
    parser.add_argument("--native-reuse-retained-opaque-resources", action=argparse.BooleanOptionalAction, default=None, help="Reuse only actual fence-retired Native retained image and scratch leases")
    parser.add_argument("--native-retained-opaque-catalog", action=argparse.BooleanOptionalAction, default=None, help="Override the three-source startup-resident retained packed-color/D32 catalogue")
    parser.add_argument("--native-retained-opaque-phase", action=argparse.BooleanOptionalAction, default=None, help="Override the separately certified startup-resident opaque phase prototype")
    parser.add_argument("--native-retained-opaque-phase-render-pass-groups", action=argparse.BooleanOptionalAction, default=None, help="Continue certified phase draws within one exact attachment generation and pass")
    parser.add_argument("--native-retained-d32-composition", action=argparse.BooleanOptionalAction, default=None,
                        help="Compose one exact primary writer with canonical D32 queries using strong completion")
    parser.add_argument("--native-retain-primary-across-disjoint-queries", action=argparse.BooleanOptionalAction, default=None,
                        help="Retain exact primary attachments across disjoint deferred D32 queries and full query-pressure fences")
    parser.add_argument("--native-retained-shared-depth-queries", action=argparse.BooleanOptionalAction, default=None, help="Read audited visibility queries from the same retained V2 depth image")
    parser.add_argument("--native-fsi-hoist-mode", choices=("disabled", "state", "incoming"), default=None,
                        help="Use a separate non-null Native FSI epilogue; null-pixel shaders remain canonical")
    parser.add_argument("--native-exact-fsi-stores", action=argparse.BooleanOptionalAction, default=None, help="Use separate non-null FSI shaders with certified redundant storage operations omitted")
    parser.add_argument("--native-retained-fp24-depth-writer", action=argparse.BooleanOptionalAction, default=None, help="Override the two-source retained FP24 depth-only writer experiment")
    parser.add_argument("--native-retained-opaque-writer", action=argparse.BooleanOptionalAction, default=None, help="Override the isolated startup-resident packed-color/D32 opaque writer")
    parser.add_argument("--native-early-depth", action=argparse.BooleanOptionalAction, default=None, help="Override experimental resident conservative material early-depth rejection")
    parser.add_argument("--native-reuse-early-depth-readonly-boundaries", action=argparse.BooleanOptionalAction, default=None, help="Preserve V1 immutable depth guard only across exact unchanged completion/query boundaries")
    parser.add_argument("--native-early-depth-material-retention", action=argparse.BooleanOptionalAction, default=None, help="Retain audited material early-depth guards across proved monotonic depth writes")
    parser.add_argument("--native-early-depth-catalog", action=argparse.BooleanOptionalAction, default=None, help="Override separate default-OFF audited two-material early-depth catalogue")
    parser.add_argument("--native-readonly-depth-no-write-metadata", action=argparse.BooleanOptionalAction, default=None, help="Preserve packed versions for exact readonly D32 queries and invalidate overlapping host precision")
    parser.add_argument("--native-readonly-depth-conservative", action=argparse.BooleanOptionalAction, default=None, help="Use a pinned conservative-depth mode on the existing exact D32 readonly query shader")
    parser.add_argument("--native-readonly-depth-attachment", action=argparse.BooleanOptionalAction, default=None, help="Override resident exact quantized Native D32 readonly queries")
    parser.add_argument("--native-readonly-depth-read-only-pass", action=argparse.BooleanOptionalAction, default=None, help="Use preserved read-only attachment access for certified Native D32 queries")
    parser.add_argument("--native-readonly-depth-lower-bound-query", action=argparse.BooleanOptionalAction, default=None, help="Use hardware lower-bound rejection with exact packed comparison and late query counts")
    parser.add_argument("--native-host-query-reset", action=argparse.BooleanOptionalAction, default=None, help="Reset only completed Native query ranges on the CPU")
    parser.add_argument("--native-readonly-query-render-pass-groups", action=argparse.BooleanOptionalAction, default=None, help="Group immutable D32 visibility draws with independent query slots")
    parser.add_argument("--native-readonly-depth-skip-edram-barrier", action=argparse.BooleanOptionalAction, default=None, help="Override packed-buffer barrier omission only for audited selected Native D32 queries")
    parser.add_argument("--native-builtin-atomic-depth", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-stencil-material-cutoff", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-opaque-color-overwrite", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-forward-control-flow", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-forward-vertex-control-flow", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-specialize-fsi-pipeline-state", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-fsi-state-material-composition", action=argparse.BooleanOptionalAction, default=None, help="Specialize eligible unqueried materials while preserving canonical Native depth/query paths")
    parser.add_argument("--native-fsi-state-inactive-early-catalog", action=argparse.BooleanOptionalAction, default=None, help="Admit two exact canonical material pairs while their early-depth catalogue is disabled")
    parser.add_argument("--native-fsi-action-state-overflow-lookup", action=argparse.BooleanOptionalAction, default=None, help="Prepare only the exact observed action material state from its successfully prepared overflow generic")
    parser.add_argument("--native-fsi-state-cache-headroom", action=argparse.BooleanOptionalAction, default=None, help="Reserve bounded shader-cache headroom for material specialization")
    parser.add_argument("--native-fsi-state-forward-cfg", action=argparse.BooleanOptionalAction, default=None, help="Override the exclusive pinned state/forward-CFG composition; startup resident variants only")
    parser.add_argument("--native-forward-pixel-catalog", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-submission-worker", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-four-inflight-batches", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-certified-snapshots", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-stable-certified-snapshots", action=argparse.BooleanOptionalAction, default=None, help="Only arm optional SDK certificates after exact reuse; requires certified snapshots")
    parser.add_argument("--native-batch-texture-snapshots", action=argparse.BooleanOptionalAction, default=None, help="Batch required CPU texture snapshots under one SDK certificate lock; enabled by default for Native, with an explicit disable override")
    parser.add_argument("--native-certified-min-snapshot-bytes", type=int, default=None, help="Minimum snapshot size for the optional SDK certificate path; zero retains all eligible sizes")
    parser.add_argument("--native-certified-fault-fastpath", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-certified-batch-queries", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-certified-diagnostics", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-implicit-fsi-barriers", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-retire-query-prefixes", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-cache-coherence-waits", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-queue-cpu-memory-writes", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-recycle-vertex-buffers", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-raw-gpu-float-cpu-textures", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-defer-cpu-texture-drains", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-cpu-image-slot-lookup", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-diagnose-full-cpu-texture-overlays", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-reset-draw-command-pool", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-time-draw-recording", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-time-cpu-stages", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-published-query-results", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-query-readiness-census", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-reuse-defined-ranges", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-query-wait-census", action=argparse.BooleanOptionalAction, default=None, help="Observe pending query GPU submission frontiers without changing scheduling")
    parser.add_argument("--native-packed-color-render-pass-groups", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-packed-depth-render-pass-groups", action=argparse.BooleanOptionalAction, default=None,
                        help="Keep only certified canonical packed depth writers in a bounded subpass")
    parser.add_argument("--native-multi-draw-command-buffers", action=argparse.BooleanOptionalAction, default=None,
                        help="Record consecutive resident Native draws in shared command buffers; keep logical owners separate")
    parser.add_argument("--native-attachmentless-fsi", action=argparse.BooleanOptionalAction, default=None,
                        help="Use a zero-attachment Native raster pass with exact packed EDRAM/query behavior")
    parser.add_argument("--native-discard-dummy-fsi-attachment", action=argparse.BooleanOptionalAction, default=None,
                        help="Discard only unobserved Native FSI dummy attachment contents")
    parser.add_argument("--native-snapshot-metadata-lru", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-thin-live-metadata", action=argparse.BooleanOptionalAction, default=None, help="Omit unused live-only shader/auxiliary object copies; full API capture stays complete")
    parser.add_argument("--native-shared-providers", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-compact-resolve-channels", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-coalesce-draw-barriers", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-skip-readonly-sample-barriers", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--native-precise-fsi-barriers", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--close-during-pipeline-prewarm", action="store_true", help="Test UI cancellation during cached pipeline startup; uses an automatic close sequence")
    parser.add_argument("--profile-cpu-switch-events", action="store_true", help="Add per-thread context switches to the bounded CPU profile for off-CPU timing; default disabled")
    parser.add_argument("--profile-cpu-close-at", type=float, default=None,
                        help="Request clean UI close this many seconds after CPU profiling starts; let its bounded record finish")
    args = parser.parse_args()
    if args.profile_cpu_switch_events and not args.profile_cpu_seconds:
        parser.error("--profile-cpu-switch-events requires --profile-cpu-seconds")
    if args.profile_cpu_close_at is not None and (not math.isfinite(args.profile_cpu_close_at) or
            not 0 < args.profile_cpu_close_at < args.profile_cpu_seconds):
        parser.error("--profile-cpu-close-at must be positive and earlier than the CPU profile duration")
    if args.native_snapshot_workers is not None and not 0 <= args.native_snapshot_workers <= 0x7FFFFFFF:
        parser.error("--native-snapshot-workers must be between 0 and 2147483647; runtime applies the hardware limit")
    if args.native_dormant_texture_entries is not None and not 0 <= args.native_dormant_texture_entries <= 2048:
        parser.error("--native-dormant-texture-entries must be between 0 and 2048")
    if args.native_certified_min_snapshot_bytes is not None and not 0 <= args.native_certified_min_snapshot_bytes <= 8*1024*1024:
        parser.error("--native-certified-min-snapshot-bytes must be between 0 and 8388608")
    if args.native_batch_texture_snapshots and (args.reference_only or args.native_certified_snapshots is False or args.native_stable_certified_snapshots is False):
        parser.error("Batched CPU texture snapshots require Native with stable certified snapshots")
    if args.raster and args.reference_only:
        parser.error("Choose Native or Xenia, not both")
    # Keep the historical --raster spelling, but there is no shadow/preview
    # combination anymore. The application's default is also Native.
    args.raster = not args.reference_only
    if (args.async_native_submissions or args.native_gpu_timestamps) and (
            not args.raster or args.separate_native_submissions or args.synchronous_native_transfers):
        parser.error("Native asynchronous submissions and GPU timestamps require --raster with the unified transfer queue")
    if not 1 <= args.seconds <= 300:
        parser.error("--seconds must be 1-300")
    if not 1 <= args.draw_batch_size <= 64:
        parser.error("--draw-batch-size must be 1-64")
    if not 0 <= args.profile_cpu_seconds <= 60:
        parser.error("--profile-cpu-seconds must be 0-60")
    if not math.isfinite(args.profile_cpu_at) or args.profile_cpu_at < 0:
        parser.error("--profile-cpu-at must be finite and nonnegative")
    if args.profile_cpu_seconds and args.profile_cpu_at + args.profile_cpu_seconds > args.seconds:
        parser.error("The scheduled profiling window must fit within --seconds")
    if not math.isfinite(args.frame_limit) or args.frame_limit < 0:
        parser.error("--frame-limit must be finite and nonnegative")
    if args.fallback_after_deferred_queries < 0:
        parser.error("--fallback-after-deferred-queries must be nonnegative")
    if args.fallback_after_deferred_queries and (not args.raster or args.synchronous_native_queries):
        parser.error("--fallback-after-deferred-queries requires --raster and deferred native queries")
    if args.reference_only and (args.raster or args.ownership or args.fallback_after_packets or
                               args.fallback_query_draws or args.fallback_before_query_draw or args.fallback_after_deferred_queries):
        parser.error("--reference-only is incompatible with --raster, --ownership and forced fallback")
    if args.reference_only and args.inputs_after_scene:
        parser.error("--reference-only cannot use --inputs-after-scene; use fixed input times")
    if args.native_fsi_state_cache_headroom and not args.native_fsi_state_material_composition:
        parser.error("FSI shader cache headroom requires material composition")
    if args.native_fsi_state_inactive_early_catalog and not args.native_fsi_state_material_composition:
        parser.error("Inactive early catalogue admission requires material composition")
    if args.native_fsi_action_state_overflow_lookup and not args.native_fsi_state_material_composition:
        parser.error("FSI action state overflow lookup requires material composition")
    if args.native_retained_opaque_phase_render_pass_groups and (
            not args.native_retained_opaque_phase or not args.native_fsi_state_material_composition or
            not args.raster or args.reference_only or args.synchronous_native_queries or
            args.synchronous_native_transfers or args.separate_native_submissions or not args.async_native_submissions or
            args.native_multi_draw_command_buffers):
        parser.error("Retained phase pass groups require canonical asynchronous phase rendering without generic grouping")
    if args.native_retained_opaque_phase and (
            not args.native_fsi_state_material_composition or args.native_early_depth_material_retention or
            args.native_reuse_early_depth_readonly_boundaries):
        parser.error("Retained opaque phase requires canonical FSI material composition without the early-depth retention experiment")
    if args.native_fsi_state_material_composition and (
            not args.raster or args.reference_only or args.synchronous_native_queries or
            args.synchronous_native_transfers or args.separate_native_submissions or not args.async_native_submissions or
            args.native_specialize_fsi_pipeline_state or args.native_fsi_state_forward_cfg or
            args.native_multi_draw_command_buffers or args.native_attachmentless_fsi or
            args.native_implicit_fsi_barriers or args.native_builtin_atomic_depth or args.native_builtin_readonly_depth_queries or
            args.native_opaque_color_overwrite or args.native_forward_control_flow or args.native_forward_vertex_control_flow or
            args.native_forward_pixel_catalog or args.native_stencil_material_cutoff or args.native_exact_fsi_stores or
            args.native_fsi_hoist_mode in ("state", "incoming") or args.native_retained_opaque_writer or
            args.native_retained_opaque_catalog or args.native_retained_fp24_depth_writer or args.native_retained_shared_depth_queries or
            args.native_retained_d32_composition or args.native_retain_primary_across_disjoint_queries or
            args.native_reuse_retained_opaque_resources or args.native_queue_retained_exports_for_draws):
        parser.error("FSI material composition requires canonical asynchronous Native rendering without other material variants")
    if args.native_queue_retained_exports_for_draws and not (args.native_retained_opaque_writer or args.native_retained_opaque_catalog):
        parser.error("Queued retained draw exports require an explicit retained Native writer variant")
    if args.native_reuse_retained_opaque_resources and not (args.native_retained_opaque_writer or args.native_retained_opaque_catalog):
        parser.error("Retained resource reuse requires an explicit exclusive Native writer variant")
    if args.native_retain_primary_across_disjoint_queries and not args.native_retained_d32_composition:
        parser.error("--native-retain-primary-across-disjoint-queries requires --native-retained-d32-composition")
    if args.native_retained_d32_composition:
        opaque = bool(args.native_retained_opaque_writer or args.native_retained_opaque_catalog)
        if (not args.raster or args.reference_only or opaque == bool(args.native_retained_fp24_depth_writer) or
                args.native_readonly_depth_attachment is False or args.synchronous_native_queries or
                args.synchronous_native_transfers or args.separate_native_submissions or not args.async_native_submissions or
                args.native_retained_shared_depth_queries or args.native_readonly_depth_skip_edram_barrier or
                args.native_fsi_hoist_mode in ("state", "incoming") or args.native_exact_fsi_stores):
            parser.error("Retained D32 composition requires one exact primary writer, deferred asynchronous Native D32 and no other query variant")
    if args.native_retained_shared_depth_queries and not args.native_retained_opaque_catalog:
        parser.error("--native-retained-shared-depth-queries requires --native-retained-opaque-catalog")
    if args.native_retained_opaque_catalog and args.native_retained_opaque_writer:
        parser.error("Retained opaque writer variants are mutually exclusive")
    if (args.native_retained_opaque_writer or args.native_retained_opaque_catalog) and (not args.raster or args.reference_only or
            (args.native_readonly_depth_attachment and not args.native_retained_d32_composition) or args.native_readonly_depth_skip_edram_barrier or args.native_builtin_readonly_depth_queries or
            args.native_specialize_fsi_pipeline_state or args.native_fsi_state_forward_cfg or args.native_attachmentless_fsi or
            args.native_builtin_atomic_depth or args.native_opaque_color_overwrite or args.native_forward_control_flow or
            args.native_forward_vertex_control_flow or args.native_forward_pixel_catalog or args.native_implicit_fsi_barriers):
        parser.error("Retained opaque writer requires the exclusive asynchronous Native shader experiment")
    if args.native_fsi_hoist_mode in ("state", "incoming") and (args.native_exact_fsi_stores or not args.raster or args.reference_only or
            args.native_implicit_fsi_barriers or args.native_builtin_atomic_depth or args.native_builtin_readonly_depth_queries or
            args.native_opaque_color_overwrite or args.native_forward_control_flow or args.native_forward_vertex_control_flow or
            args.native_forward_pixel_catalog or args.native_specialize_fsi_pipeline_state or args.native_fsi_state_forward_cfg or
            args.native_retained_opaque_writer or args.native_retained_opaque_catalog or args.native_retained_fp24_depth_writer or
            args.native_retained_shared_depth_queries or args.native_queue_retained_exports_for_draws or
            args.native_reuse_retained_opaque_resources or args.native_attachmentless_fsi):
        parser.error("FSI hoist requires exclusive non-null Native FSI shaders; canonical D32 queries may remain enabled")
    if args.native_exact_fsi_stores and (not args.raster or args.reference_only or
            args.native_implicit_fsi_barriers or args.native_builtin_atomic_depth or args.native_builtin_readonly_depth_queries or
            args.native_opaque_color_overwrite or args.native_forward_control_flow or args.native_forward_vertex_control_flow or
            args.native_forward_pixel_catalog or args.native_specialize_fsi_pipeline_state or args.native_fsi_state_forward_cfg or
            args.native_retained_opaque_writer or args.native_retained_opaque_catalog or args.native_retained_fp24_depth_writer or
            args.native_retained_shared_depth_queries or args.native_queue_retained_exports_for_draws or
            args.native_reuse_retained_opaque_resources or args.native_attachmentless_fsi):
        parser.error("Exact FSI stores require exclusive non-null Native FSI shaders; canonical D32 queries may remain enabled")
    if args.native_retained_fp24_depth_writer and (not args.raster or args.reference_only or args.native_retained_opaque_writer or
            args.native_retained_opaque_catalog or args.native_retained_shared_depth_queries or args.native_reuse_retained_opaque_resources or
            args.native_queue_retained_exports_for_draws or (args.native_readonly_depth_attachment and not args.native_retained_d32_composition) or args.native_readonly_depth_skip_edram_barrier or
            args.native_builtin_readonly_depth_queries or args.native_builtin_atomic_depth or args.native_specialize_fsi_pipeline_state or
            args.native_fsi_state_forward_cfg or args.native_attachmentless_fsi or args.native_forward_control_flow or
            args.native_forward_vertex_control_flow or args.native_forward_pixel_catalog or args.native_opaque_color_overwrite or
            args.native_implicit_fsi_barriers or args.native_multi_draw_command_buffers):
        parser.error("Retained FP24 depth writer requires the isolated asynchronous Native experiment")
    if args.native_readonly_depth_skip_edram_barrier and not args.native_readonly_depth_attachment:
        parser.error("--native-readonly-depth-skip-edram-barrier requires --native-readonly-depth-attachment")
    if args.native_readonly_depth_attachment and (not args.raster or args.reference_only or args.native_builtin_readonly_depth_queries or
            args.native_specialize_fsi_pipeline_state or args.native_fsi_state_forward_cfg or args.native_attachmentless_fsi or
            (args.native_builtin_atomic_depth and not args.native_packed_depth_render_pass_groups) or args.native_opaque_color_overwrite or args.native_forward_control_flow or
            args.native_forward_vertex_control_flow or args.native_forward_pixel_catalog or args.native_implicit_fsi_barriers):
        parser.error("Native readonly D32 requires exclusive asynchronous Native packed query shaders")
    if args.native_stencil_material_cutoff and (not args.raster or args.reference_only or
            args.synchronous_native_transfers or args.separate_native_submissions or not args.async_native_submissions or
            args.native_attachmentless_fsi or args.native_implicit_fsi_barriers or args.native_specialize_fsi_pipeline_state or
            args.native_fsi_state_forward_cfg or args.native_exact_fsi_stores or args.native_fsi_hoist_mode in ("state", "incoming") or
            args.native_retained_opaque_writer or args.native_retained_opaque_catalog or args.native_retained_fp24_depth_writer or
            args.native_opaque_color_overwrite or args.native_forward_control_flow or args.native_forward_vertex_control_flow or
            args.native_forward_pixel_catalog):
        parser.error("Native stencil material cutoff requires the canonical asynchronous packed Native provider")
    if args.native_specialize_fsi_pipeline_state and args.native_fsi_state_forward_cfg:
        parser.error("Select one FSI state shader experiment")
    if (args.native_specialize_fsi_pipeline_state or args.native_fsi_state_forward_cfg) and (not args.raster or args.reference_only or
            args.native_builtin_readonly_depth_queries or args.native_readonly_depth_attachment or args.native_builtin_atomic_depth or
            args.native_opaque_color_overwrite or args.native_forward_control_flow or
            args.native_forward_vertex_control_flow or args.native_forward_pixel_catalog or args.native_implicit_fsi_barriers):
        parser.error("FSI specialization requires exclusive Native packed shader experiment")
    if args.close_during_pipeline_prewarm:
        if not args.native_pipeline_prewarm or not args.pipeline_cache_source or args.input_sequence or args.inputs_after_scene or args.reference_only:
            parser.error("Startup close requires native pipeline warmup and a recipe cache, without another input sequence/anchor or reference-only mode")
    if args.capture_reference_history and not (args.reference_only and args.capture_api):
        parser.error("--capture-reference-history requires --reference-only and --capture-api")
    if args.texture_probe_hashes:
        hashes = args.texture_probe_hashes.split(",")
        if len(hashes) > 32 or any(not re.fullmatch(r"[0-9a-f]{64}", value) for value in hashes):
            parser.error("--texture-probe-hashes requires 1-32 lowercase SHA256 hashes")
        if not args.raster or args.reference_only:
            parser.error("--texture-probe-hashes requires --raster")
    if args.profile_cpu_seconds and not (PERF.is_file() and os.access(PERF, os.X_OK)):
        parser.error(f"CPU profiling requires the existing executable {PERF}")
    if args.child:
        child(args.child, args.seconds)
        return
    try:
        actions = ([{"at": 0.1, "screenshot": "pipeline-startup.png"}, {"at": 0.2, "close": True}]
                   if args.close_during_pipeline_prewarm else
                   json.loads(args.input_sequence.read_text()) if args.input_sequence else [])
        actions = validate_actions(actions, args.seconds, args.capture_api)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    drivers = Path("/usr/share/vulkan/icd.d")
    driver = next((p for p in [drivers / f"lvp_icd.{platform.machine()}.json", drivers / "lvp_icd.json"] if p.is_file()), None)
    if not args.hardware and driver is None:
        parser.error("Mesa lavapipe driver is required without --hardware")
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    build_identity = {}
    for relative in ("bin/gow3", "bin/librexruntime.so"):
        binary = BUILD / relative
        with binary.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        build_identity[relative] = {"sha256": digest, "bytes": binary.stat().st_size}
    (args.output / "build-identity.json").write_text(json.dumps(build_identity, indent=2) + "\n")
    probe_settings = None
    if args.texture_probe_hashes:
        probe_directory = args.output / "texture-probes"
        if probe_directory.exists():
            parser.error("OUTPUT/texture-probes already exists; use a fresh output directory")
        probe_settings = {"directory": str(probe_directory), "hashes": args.texture_probe_hashes}
    capture_settings = None
    if args.capture_api:
        # Reserve the entire namespace atomically; neither an earlier capture,
        # its trigger, nor its GPU-history directory may ever be overwritten.
        capture_directory = args.output / "capture"
        try:
            capture_directory.mkdir()
        except FileExistsError:
            parser.error("OUTPUT/capture already exists; use a fresh output directory")
        capture_settings = {"api": str(capture_directory / "api.g3d"),
                            "trigger": str(capture_directory / "capture.trigger"), "frames": 2,
                            "reference_only": args.reference_only}
        if args.capture_reference_history:
            capture_settings["history"] = str(capture_directory / "reference-history")
        (capture_directory / "settings.json").write_text(json.dumps(capture_settings, indent=2) + "\n")
    cache_seed = None
    cache_preserved = None
    pipeline_cache_seed = None
    pipeline_cache_preserved = None
    fsi_state_recipe_seed = fsi_state_shader_seed = fsi_state_forward_shader_seed = None
    fsi_state_recipe_preserved = fsi_state_shader_preserved = fsi_state_forward_shader_preserved = None
    mesa_cache_seed = None
    mesa_cache_preserved = None
    with tempfile.TemporaryDirectory(prefix="gow3-native-boot-") as folder:
        temporary = Path(folder)
        if args.allocator_library:
            library = args.allocator_library.resolve(strict=True)
            if not library.is_file() or any(c.isspace() or c == ':' for c in str(library)):
                parser.error("--allocator-library requires one regular library without loader separators")
            if os.environ.get("LD_PRELOAD"):
                parser.error("Allocator comparison requires an unset inherited LD_PRELOAD")
            (temporary / "allocator.json").write_text(json.dumps({
                "path": str(library), "sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
                "mapped": False, "scope": "game process only"}, indent=2) + "\n")
        if args.guest_command_line:
            (temporary / "guest-command-line.txt").write_text(args.guest_command_line)
        if args.reference_only:
            (temporary / "reference-only").touch()
        if capture_settings:
            (temporary / "capture-settings.json").write_text(json.dumps(capture_settings))
        if probe_settings:
            (temporary / "texture-probe-settings.json").write_text(json.dumps(probe_settings))
        (temporary / "draw-batch-size").write_text(str(args.draw_batch_size))
        if args.profile_cpu_seconds:
            (temporary / "profile-settings.json").write_text(json.dumps({"at": args.profile_cpu_at, "seconds": args.profile_cpu_seconds, "switch_events": args.profile_cpu_switch_events, "close_at": args.profile_cpu_close_at}))
        if args.native_cache_source:
            if not args.native_cache_source.is_dir():
                parser.error("--native-cache-source must be an existing native-shaders directory")
            cache_seed = copy_native_cache(args.native_cache_source, temporary / "cache/native-shaders")
        if args.mesa_cache_source:
            if not args.mesa_cache_source.is_dir():
                parser.error("--mesa-cache-source must be an existing mesa_shader_cache directory")
            mesa_cache_seed = copy_mesa_cache(args.mesa_cache_source, temporary / "desktop-cache/mesa_shader_cache")
        if args.pipeline_cache_source:
            if not args.pipeline_cache_source.is_dir():
                raise ValueError("Pipeline cache source must be an existing directory")
            pipeline_cache_seed = copy_pipeline_cache(args.pipeline_cache_source, temporary / "cache/native-pipeline-recipes")
        for source, name, shaders, forward_cfg in (
                (args.fsi_state_recipe_cache_source, "native-fsi-state-recipes", False, False),
                (args.fsi_state_shader_cache_source, "native-fsi-state-shaders", True, False),
                (args.fsi_state_forward_shader_cache_source, "native-fsi-state-forward-shaders", True, True)):
            if source is None:
                continue
            if not source.is_dir():
                parser.error("FSI state cache source must be an existing directory")
            copied = copy_fsi_state_cache(source, temporary / "cache" / name, shaders=shaders, forward_cfg=forward_cfg)
            if forward_cfg:
                fsi_state_forward_shader_seed = copied
            elif shaders:
                fsi_state_shader_seed = copied
            else:
                fsi_state_recipe_seed = copied
        config = temporary / "config/labwc"
        config.mkdir(parents=True)
        (config / "rc.xml").write_text("<labwc_config/>\n")
        (config / "autostart").write_text("")
        # Preserve the game's essential rendering fixes (for example unclamped
        # vertex indices). SDK defaults are not an equivalent reference setup.
        configuration = (ROOT / "config/gow3.toml").read_text()
        overrides = dict(fullscreen=False, window_width=1280, window_height=720,
                         present_hdr=False, present_effect="bilinear", vsync=False,
                         gow3_frame_limit_mode="unlimited" if args.frame_limit == 0 else "custom",
                         framerate_limit=args.frame_limit,
                         vulkan_pipeline_creation_threads=0, async_shader_compilation=False,
                         vulkan_async_shader_translation=False, audio_mute=True)
        if args.anisotropic_override is not None:
            overrides["anisotropic_override"] = args.anisotropic_override
        if args.bulk_type0_constants is not None:
            overrides["gpu_bulk_type0_constants"] = args.bulk_type0_constants
        overrides["gpu_command_summary"] = args.gpu_command_summary
        overrides["gpu_native_marker_operands"] = args.gpu_native_marker_operands
        if args.native_submit_completion_markers is not None:
            overrides["gpu_native_submit_completion_markers"] = args.native_submit_completion_markers
        if args.present_gpu_timestamps is not None:
            overrides["present_gpu_timestamps"] = args.present_gpu_timestamps
        overrides["gow3_renderer"] = "native" if args.raster else "xenia"
        overrides["gow3_renderer_history"] = ""
        overrides["gow3_native_live"] = not args.reference_only
        overrides["gow3_native_present"] = not args.reference_only
        overrides.update(gow3_renderer_capture="", gow3_renderer_history="", gow3_renderer_capture_trigger="")
        if args.reference_only:
            overrides["gow3_native_ownership"] = False
        if args.weapon_input:
            overrides.update(keybind_left_trigger="F8", keybind_right_trigger="F9")
        if args.camera_input:
            overrides.update(keybind_rstick_up="I", keybind_rstick_down="K",
                             keybind_rstick_left="J", keybind_rstick_right="L")
        overrides["gow3_native_draw_batch_size"] = args.draw_batch_size
        overrides["gow3_native_deferred_queries"] = not args.synchronous_native_queries
        overrides["gow3_native_batch_transfers"] = not args.synchronous_native_transfers
        overrides["gow3_native_unified_gpu_queue"] = not (args.separate_native_submissions or args.synchronous_native_transfers)
        overrides["gow3_native_async_gpu_submissions"] = args.async_native_submissions
        if args.native_fsi_hoist_mode is not None:
            overrides["gow3_native_fsi_hoist_mode"] = {"disabled": 0, "state": 1, "incoming": 2}[args.native_fsi_hoist_mode]
        if args.native_query_capacity is not None:
            overrides["gow3_native_query_capacity"] = args.native_query_capacity
        if args.native_published_query_results is not None:
            overrides["gow3_native_published_query_results"] = args.native_published_query_results
        if args.native_reuse_defined_ranges is not None:
            overrides["gow3_native_reuse_defined_ranges"] = args.native_reuse_defined_ranges
        if args.native_query_wait_census is not None:
            overrides["gow3_native_query_wait_census"] = args.native_query_wait_census
        if args.native_query_readiness_census is not None:
            overrides["gow3_native_query_readiness_census"] = args.native_query_readiness_census
        for option in ("native_fast_snapshot_fingerprints", "native_exact_fsi_stores", "native_specialize_fsi_pipeline_state", "native_fsi_state_forward_cfg", "native_fsi_state_material_composition", "native_fsi_state_inactive_early_catalog", "native_fsi_action_state_overflow_lookup", "native_fsi_state_cache_headroom", "native_snapshot_workers", "native_lazy_state_identities", "native_direct_resource_refs", "native_shader_prewarm", "native_retain_fsi_raster_extent", "native_submission_capacity", "native_four_inflight_batches", "native_submission_worker", "native_cache_texture_layouts", "native_associative_texture_layouts", "native_compact_cpu_texture_version_pages", "native_block_page_versions", "native_range_texture_versions", "native_coalesced_texture_ranges", "native_cache_gpu_alias_states", "native_queued_volume_alias_loads", "native_shader_lookup_lookaside", "native_recycle_resolve_images", "native_recycle_resolve_descriptors", "native_recycle_pass_transfer_descriptors", "native_recycle_pass_transfer_render_passes", "native_reuse_resolve_identity_views", "native_diagnose_texture_eviction_reloads", "native_recycle_vertex_buffers", "native_raw_gpu_float_cpu_textures", "native_defer_cpu_texture_drains", "native_cpu_image_slot_lookup", "native_query_poll_yield", "native_inline_getdata_register_helpers", "native_pipeline_prewarm", "native_prewarm_overflow_pipelines", "native_coalesce_draw_barriers", "native_skip_readonly_sample_barriers", "native_precise_fsi_barriers", "native_compact_resolve_channels", "native_shared_providers", "native_batch_draw_descriptors", "native_skip_unused_descriptor_sets", "native_cache_draw_descriptors", "native_delta_constant_uploads", "native_packed_zero_depth_clear", "native_skip_full_color_clear_restore", "native_reset_draw_command_pool", "native_time_draw_recording", "native_time_cpu_stages", "native_multi_draw_command_buffers", "native_packed_color_render_pass_groups", "native_packed_depth_render_pass_groups", "native_discard_dummy_fsi_attachment", "native_attachmentless_fsi", "native_snapshot_metadata_lru", "native_thin_live_metadata", "native_certified_snapshots", "native_stable_certified_snapshots", "native_batch_texture_snapshots", "native_certified_min_snapshot_bytes", "native_certified_fault_fastpath", "native_implicit_fsi_barriers", "native_retire_query_prefixes", "native_cache_coherence_waits", "native_queue_cpu_memory_writes", "native_diagnose_full_cpu_texture_overlays", "native_certified_batch_queries", "native_certified_diagnostics"):
            if (value := getattr(args, option)) is not None:
                overrides["gow3_" + option] = value
        overrides["gow3_native_gpu_timestamps"] = args.native_gpu_timestamps
        if args.native_stencil_export_restores is not None:
            overrides["gow3_native_stencil_export_restores"] = args.native_stencil_export_restores
        if args.native_builtin_readonly_depth_queries is not None:
            overrides["gow3_native_builtin_readonly_depth_queries"] = args.native_builtin_readonly_depth_queries
        if args.native_queue_retained_exports_for_draws is not None:
            overrides["gow3_native_queue_retained_exports_for_draws"] = args.native_queue_retained_exports_for_draws
        if args.native_reuse_retained_opaque_resources is not None:
            overrides["gow3_native_reuse_retained_opaque_resources"] = args.native_reuse_retained_opaque_resources
        if args.native_retained_opaque_catalog is not None:
            overrides["gow3_native_retained_opaque_catalog"] = args.native_retained_opaque_catalog
        if args.native_retained_opaque_phase is not None:
            overrides["gow3_native_retained_opaque_phase"] = args.native_retained_opaque_phase
        if args.native_retained_opaque_phase_render_pass_groups is not None:
            overrides["gow3_native_retained_opaque_phase_render_pass_groups"] = args.native_retained_opaque_phase_render_pass_groups
        if args.native_retained_d32_composition is not None:
            overrides["gow3_native_retained_d32_composition"] = args.native_retained_d32_composition
        if args.native_retain_primary_across_disjoint_queries is not None:
            overrides["gow3_native_retain_primary_across_disjoint_queries"] = args.native_retain_primary_across_disjoint_queries
        if args.native_retained_shared_depth_queries is not None:
            overrides["gow3_native_retained_shared_depth_queries"] = args.native_retained_shared_depth_queries
        if args.native_retained_fp24_depth_writer is not None:
            overrides["gow3_native_retained_fp24_depth_writer"] = args.native_retained_fp24_depth_writer
        if args.native_pinned_null_constant_uploads is not None:
            overrides["gow3_native_pinned_null_constant_uploads"] = args.native_pinned_null_constant_uploads
        if args.native_atlas_depth_region_restores is not None:
            overrides["gow3_native_atlas_depth_region_restores"] = args.native_atlas_depth_region_restores
        if args.native_fused_atlas_depth_clear is not None:
            overrides["gow3_native_fused_atlas_depth_clear"] = args.native_fused_atlas_depth_clear
        if args.native_direct_atlas_depth_resolves is not None:
            overrides["gow3_native_direct_atlas_depth_resolves"] = args.native_direct_atlas_depth_resolves
        if args.native_packed_primary_stencil_clear is not None:
            overrides["gow3_native_packed_primary_stencil_clear"] = args.native_packed_primary_stencil_clear
        if args.native_direct_packed_color_resolves is not None:
            overrides["gow3_native_direct_packed_color_resolves"] = args.native_direct_packed_color_resolves
        if args.native_packed_secondary_stencil_clear is not None:
            overrides["gow3_native_packed_secondary_stencil_clear"] = args.native_packed_secondary_stencil_clear
        if args.native_diagnose_cpu_write_cohorts is not None:
            overrides["gow3_native_diagnose_cpu_write_cohorts"] = args.native_diagnose_cpu_write_cohorts
        if args.native_diagnose_completion_causes is not None:
            overrides["gow3_native_diagnose_completion_causes"] = args.native_diagnose_completion_causes
        if args.native_reuse_cpu_write_upload is not None:
            overrides["gow3_native_reuse_cpu_write_upload"] = args.native_reuse_cpu_write_upload
        if args.native_canonical_resolve_stores is not None:
            overrides["gow3_native_canonical_resolve_stores"] = args.native_canonical_resolve_stores
        if args.native_fused_packed_atlas_resolves is not None:
            overrides["gow3_native_fused_packed_atlas_resolves"] = args.native_fused_packed_atlas_resolves
        if args.native_retained_opaque_writer is not None:
            overrides["gow3_native_retained_opaque_writer"] = args.native_retained_opaque_writer
        if args.native_early_depth is not None:
            overrides["gow3_native_early_depth"] = args.native_early_depth
        if args.native_reuse_early_depth_readonly_boundaries is not None:
            overrides["gow3_native_reuse_early_depth_readonly_boundaries"] = args.native_reuse_early_depth_readonly_boundaries
        if args.native_early_depth_material_retention is not None:
            overrides["gow3_native_early_depth_material_retention"] = args.native_early_depth_material_retention
        if args.native_early_depth_catalog is not None:
            overrides["gow3_native_early_depth_catalog"] = args.native_early_depth_catalog
        if args.native_readonly_depth_attachment is not None:
            overrides["gow3_native_readonly_depth_attachment"] = args.native_readonly_depth_attachment
        if args.native_readonly_depth_no_write_metadata is not None:
            overrides["gow3_native_readonly_depth_no_write_metadata"] = args.native_readonly_depth_no_write_metadata
        if args.native_readonly_depth_conservative is not None:
            overrides["gow3_native_readonly_depth_conservative"] = args.native_readonly_depth_conservative
        if args.native_readonly_depth_read_only_pass is not None:
            overrides["gow3_native_readonly_depth_read_only_pass"] = args.native_readonly_depth_read_only_pass
        if args.native_readonly_depth_lower_bound_query is not None:
            overrides["gow3_native_readonly_depth_lower_bound_query"] = args.native_readonly_depth_lower_bound_query
        if args.native_host_query_reset is not None:
            overrides["gow3_native_host_query_reset"] = args.native_host_query_reset
        if args.native_readonly_query_render_pass_groups is not None:
            overrides["gow3_native_readonly_query_render_pass_groups"] = args.native_readonly_query_render_pass_groups
        if args.native_readonly_depth_skip_edram_barrier is not None:
            overrides["gow3_native_readonly_depth_skip_edram_barrier"] = args.native_readonly_depth_skip_edram_barrier
        if args.native_builtin_atomic_depth is not None:
            overrides["gow3_native_builtin_atomic_depth"] = args.native_builtin_atomic_depth
        if args.native_stencil_material_cutoff is not None:
            overrides["gow3_native_stencil_material_cutoff"] = args.native_stencil_material_cutoff
        if args.native_opaque_color_overwrite is not None:
            overrides["gow3_native_opaque_color_overwrite"] = args.native_opaque_color_overwrite
        if args.native_forward_control_flow is not None:
            overrides["gow3_native_forward_control_flow"] = args.native_forward_control_flow
        if args.native_forward_vertex_control_flow is not None:
            overrides["gow3_native_forward_vertex_control_flow"] = args.native_forward_vertex_control_flow
        if args.native_forward_pixel_catalog is not None:
            overrides["gow3_native_forward_pixel_catalog"] = args.native_forward_pixel_catalog
        if args.native_gpu_batch_attribution:
            if not overrides["gow3_native_unified_gpu_queue"]:
                parser.error("--native-gpu-batch-attribution requires the unified native queue")
            overrides["gow3_native_gpu_batch_report"] = str(args.output / "gpu-batches.json")
        if args.native_gpu_physical_timestamps:
            if not args.raster or not overrides["gow3_native_unified_gpu_queue"] or args.native_gpu_draw_timestamps:
                parser.error("Physical GPU timing requires unified raster and no per-draw timer")
            if not (0 <= args.native_gpu_physical_first < args.native_gpu_physical_end < 0xffffffff and 0 < args.native_gpu_physical_interval <= 0xffffffff):
                parser.error("Invalid physical GPU API frame sampling range")
            overrides["gow3_native_gpu_physical_report"] = str(args.output / "gpu-physical.json")
            overrides["gow3_native_gpu_physical_first"] = args.native_gpu_physical_first
            overrides["gow3_native_gpu_physical_end"] = args.native_gpu_physical_end
            overrides["gow3_native_gpu_physical_interval"] = args.native_gpu_physical_interval
        if args.native_gpu_draw_timestamps:
            if not args.raster or not overrides["gow3_native_unified_gpu_queue"]:
                parser.error("--native-gpu-draw-timestamps requires the unified native queue")
            overrides["gow3_native_gpu_draw_report"] = str(args.output / "gpu-draws.json")
        overrides["gow3_native_detailed_reports"] = args.native_detailed_reports
        overrides["gow3_native_retain_unused_textures"] = not args.no_native_texture_retention
        if args.native_dormant_texture_entries is not None:
            overrides["gow3_native_dormant_texture_entries"] = args.native_dormant_texture_entries
        overrides["gow3_native_retain_unused_vertices"] = args.native_vertex_retention
        for item in args.config:
            key, _, text = item.partition("=")
            if not key or not _:
                raise SystemExit(f"--config expects KEY=VALUE, got {item!r}")
            try:
                overrides[key.strip()] = json.loads(text)
            except json.JSONDecodeError:
                overrides[key.strip()] = text
        for key, value in overrides.items():
            assignment = f"{key} = {json.dumps(value)}"
            pattern = rf"(?m)^{re.escape(key)}\s*=.*$"
            if re.search(pattern, configuration):
                configuration = re.sub(pattern, lambda match: assignment, configuration)
            else:
                configuration += "\n" + assignment + "\n"
        (temporary / "gow3.toml").write_text(configuration)
        if args.trace_input:
            (temporary / "trace-input").touch()
        if args.diagnose_shutdown:
            (temporary / "diagnose-shutdown").touch()
        if args.ownership:
            (temporary / "native-ownership").touch()
        if args.raster:
            (temporary / "native-raster").touch()
        if args.fallback_after_packets or args.fallback_query_draws or args.fallback_before_query_draw or args.fallback_after_deferred_queries:
            (temporary / "fallback-settings.json").write_text(json.dumps({
                "gow3_native_test_fallback_after_packets": args.fallback_after_packets,
                "gow3_native_test_fallback_query_draws": args.fallback_query_draws,
                "gow3_native_test_fallback_before_query_draw": args.fallback_before_query_draw,
                "gow3_native_test_fallback_after_deferred_queries": args.fallback_after_deferred_queries}))
        if args.inputs_after_scene:
            (temporary / "inputs-after-scene").touch()
        if args.close_during_pipeline_prewarm:
            (temporary / "close-during-pipeline-prewarm").touch()
        if args.copy_user_data:
            seed_identity = {"schema": 1, "stage": "before_guest_launch", "verified": False,
                             "scope": "user tree excluding every child named cache",
                             "source_root": str(ROOT / "user"), "private_root": str(temporary / "user")}
            try:
                seed_identity["source_before"] = user_data_manifest(ROOT / "user")
            except (OSError, ValueError) as error:
                seed_identity["error"] = str(error)
            shutil.copytree(ROOT / "user", temporary / "user", ignore=shutil.ignore_patterns("cache"))
            if "source_before" in seed_identity:
                try:
                    seed_identity["source_after"] = user_data_manifest(ROOT / "user")
                    seed_identity["private_before_launch"] = user_data_manifest(temporary / "user")
                    seed_identity["verified"] = (seed_identity["source_before"] == seed_identity["source_after"] ==
                                                  seed_identity["private_before_launch"])
                    if not seed_identity["verified"]:
                        seed_identity["error"] = "Source and private prelaunch user-data bytes differ"
                except (OSError, ValueError) as error:
                    seed_identity["error"] = str(error)
            (args.output / "user-data-seed-identity.json").write_text(json.dumps(seed_identity, indent=2) + "\n")
        if actions:
            (temporary / "input-sequence.json").write_text(json.dumps(actions))
        if args.output_mode:
            if not re.fullmatch(r"\d{3,5}x\d{3,5}", args.output_mode):
                parser.error("--output-mode must be WIDTHxHEIGHT")
            (temporary / "output-mode").write_text(args.output_mode)
        env = os.environ.copy()
        env.update(XDG_RUNTIME_DIR=folder, XDG_CONFIG_HOME=str(temporary / "config"),
                   XDG_CACHE_HOME=str(temporary / "desktop-cache"), XDG_DATA_HOME=str(temporary / "data"),
                   WLR_BACKENDS="headless", WLR_HEADLESS_OUTPUTS="1",
                   WLR_RENDERER="gles2" if args.hardware else "pixman",
                   DISABLE_OBS_VKCAPTURE="1")
        if args.hardware:
            for key in ("VK_DRIVER_FILES", "VK_ICD_FILENAMES"):
                env.pop(key, None)
        else:
            env["VK_DRIVER_FILES"] = str(driver)
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"):
            env.pop(key, None)
        command = shlex.join([sys.executable, str(Path(__file__).resolve()), "--child", folder, "--seconds", str(args.seconds)])
        with (args.output / "compositor.log").open("w") as compositor_output:
            compositor = subprocess.Popen(["labwc", "-C", str(config), "-S", command], cwd=ROOT, env=env,
                                          stdout=compositor_output, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                compositor.wait(timeout=args.seconds + 15)
            except subprocess.TimeoutExpired:
                pass
            finally:
                # This session contains only the test compositor and its children.
                try:
                    os.killpg(compositor.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    compositor.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(compositor.pid, signal.SIGKILL)
                    compositor.wait()
        for name in ("game.log", "console.log", "status.json", "gow3.toml", "input-sequence.json", "input-results.json", "input-anchor.json",
                     "cpu-profile.json", "perf.log", "gameplay.data", "shutdown-stack.log", "shutdown-diagnostic.json", "allocator.json"):
            source = temporary / name
            if source.is_file():
                shutil.copyfile(source, args.output / name)
        rotated_logs = []
        for source in temporary.glob("game.*.log"):
            match = re.fullmatch(r"game\.(\d+)\.log", source.name)
            if match:
                rotated_logs.append((int(match[1]), source))
                shutil.copyfile(source, args.output / source.name)
        if rotated_logs:
            ordered_logs = [source for _, source in sorted(rotated_logs, reverse=True)]
            ordered_logs.append(temporary / "game.log")
            (args.output / "game.full.log").write_text("".join(
                source.read_text(errors="replace") for source in ordered_logs if source.exists()))
        for screenshot in temporary.glob("*.png"):
            shutil.copyfile(screenshot, args.output / screenshot.name)
        for input_log in temporary.glob("input-job-*.log"):
            shutil.copyfile(input_log, args.output / input_log.name)
        if (temporary / "failure").is_dir():
            shutil.copytree(temporary / "failure", args.output / "failure", dirs_exist_ok=True)
        if args.preserve_native_cache:
            cache_preserved = copy_native_cache(temporary / "cache/native-shaders", args.output / "native-shaders")
        if args.preserve_fsi_state_cache:
            fsi_state_recipe_preserved = copy_fsi_state_cache(temporary / "cache/native-fsi-state-recipes", args.output / "native-fsi-state-recipes")
            fsi_state_shader_preserved = copy_fsi_state_cache(temporary / "cache/native-fsi-state-shaders", args.output / "native-fsi-state-shaders", shaders=True)
            if args.native_fsi_state_forward_cfg is not None or args.fsi_state_forward_shader_cache_source is not None:
                fsi_state_forward_shader_preserved = copy_fsi_state_cache(
                    temporary / "cache/native-fsi-state-forward-shaders", args.output / "native-fsi-state-forward-shaders",
                    shaders=True, forward_cfg=True)
        if args.preserve_pipeline_cache:
            pipeline_cache_preserved = copy_pipeline_cache(temporary / "cache/native-pipeline-recipes", args.output / "native-pipeline-recipes")
        if args.preserve_mesa_cache and (temporary / "desktop-cache/mesa_shader_cache").is_dir():
            mesa_cache_preserved = copy_mesa_cache(temporary / "desktop-cache/mesa_shader_cache", args.output / "mesa-cache")
    log_path = args.output / "game.full.log"
    if not log_path.exists():
        log_path = args.output / "game.log"
    log = log_path.read_text(errors="replace") if log_path.exists() else ""
    native_lines = [line for line in log.splitlines()
                    if "Native renderer" in line or "Native API frame" in line or "Native raster output" in line
                    or "Native query prefix" in line or "Native packet" in line or "Fatal native" in line
                    or "Native query publication" in line or "Native state handed back" in line]
    sampled_frames = [int(match.group(1)) for line in native_lines
                      if (match := re.search(r"Native API frame (\d+) completed", line))]
    completed = re.search(r"after (\d+) completed native frames \(last API frame (\d+)\)", log)
    query_prefixes = [int(value) for value in re.findall(r"Native query prefix (\d+) completed", log)]
    comparisons = [json.loads(line.split("Native query comparison: ", 1)[1])
                   for line in log.splitlines() if "Native query comparison: " in line]
    prefix_comparisons = [json.loads(line.split("Native query prefix comparison: ", 1)[1])
                         for line in log.splitlines() if "Native query prefix comparison: " in line]
    timestamps = re.findall(r"\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]", log)
    log_started = datetime.fromisoformat(timestamps[0]) if timestamps else None
    def elapsed(line):
        stamp = re.search(r"\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+)\]", line)
        return (datetime.fromisoformat(stamp[1]) - log_started).total_seconds() if stamp and log_started else None

    frame_timings = []
    for line in log.splitlines():
        match = re.search(r"Native API frame (\d+) completed (\d+) operations in ([\d.]+) ms", line)
        if match:
            frame_timings.append({"frame": int(match[1]), "operations": int(match[2]),
                                  "milliseconds": float(match[3]), "elapsed_seconds": elapsed(line)})
        elif (match := re.search(r"Native API frame (\d+) completed (\d+) operations; CPU stage timings disabled", line)):
            frame_timings.append({"frame": int(match[1]), "operations": int(match[2]), "milliseconds": None,
                                  "cpu_stage_timings_enabled": False, "elapsed_seconds": elapsed(line)})
    operation_timings = []
    for line in log.splitlines():
        match = re.search(r"Native operation timings at API frame (\d+) \(count/ms\): (.*); transaction_flush=([\d.]+) ms", line)
        if match:
            entry = {"frame": int(match[1]), "transaction_flush_ms": float(match[3]), "elapsed_seconds": elapsed(line)}
            for name, count, milliseconds in re.findall(r"(\w+)=(\d+)/([\d.]+)", match[2]):
                entry[name] = {"count": int(count), "milliseconds": float(milliseconds)}
            operation_timings.append(entry)
        elif (match := re.search(r"Native operation counts at API frame (\d+) \(CPU stage timings disabled\): (.*)", line)):
            entry = {"frame": int(match[1]), "transaction_flush_ms": None,
                     "cpu_stage_timings_enabled": False, "elapsed_seconds": elapsed(line)}
            for name, count in re.findall(r"(\w+)=(\d+)", match[2]):
                entry[name] = {"count": int(count), "milliseconds": None}
            operation_timings.append(entry)
    query_pressure = []
    for line in log.splitlines():
        match = re.search(r"Native query pressure at API frame (\d+): (.*)", line)
        if match:
            entry = {"frame": int(match[1]), "elapsed_seconds": elapsed(line)}
            for name, value in re.findall(r"(\w+)=([\d.]+)", match[2]):
                entry[name] = float(value) if name.endswith("_ms") else int(value)
            query_pressure.append(entry)
    def json_samples(label):
        samples = []
        frame = None
        for line in log.splitlines():
            completed_frame = re.search(r"Native API frame (\d+) completed", line)
            if completed_frame:
                frame = int(completed_frame[1])
            prefix = "Native " + label + ": "
            if prefix in line:
                entry = json.loads(line.split(prefix, 1)[1])
                entry.setdefault("frame", frame)
                entry["elapsed_seconds"] = elapsed(line)
                samples.append(entry)
        return samples
    def batch_texture_rows(prefix, fields):
        rows = []
        for line in log.splitlines():
            if prefix not in line:
                continue
            values = dict(re.findall(r"(\w+)=(true|false|\d+)(?=\s|$)", line.split(prefix, 1)[1]))
            if set(values) != set(fields):
                raise ValueError("Malformed Native batch texture snapshot diagnostic")
            row = {key: (value == "true" if value in ("true", "false") else int(value))
                   for key, value in values.items()}
            row["elapsed_seconds"] = elapsed(line)
            rows.append(row)
        return rows
    batch_texture_startup = batch_texture_rows("Native batch texture snapshots startup: ",
        ("requested", "enabled", "maximum_views", "maximum_requested_bytes"))
    batch_texture_shutdown = batch_texture_rows("Native batch texture snapshots: ",
        ("enabled", "calls", "views", "unique_views", "provider_calls", "provider_views", "cohort_bytes", "scalar_fallback_calls", "provider_errors"))
    draw_timings = json_samples("draw stages")
    draw_batches = json_samples("draw_batches")
    texture_updates = json_samples("texture_updates")
    transfer_batches = json_samples("transfer_batches")
    gpu_work_queue = json_samples("gpu_work_queue")
    alias_generations = json_samples("alias_generations")
    gpu_timings = json_samples("gpu_timings")
    resource_cache_samples = {key: json_samples(key) for key in
                              ("device", "page_versions", "texture_filtering", "pipeline_cache", "shader_cache", "shader_lookup_lookaside", "gpu_alias_state_cache", "query_prefix_retirement", "index_cache", "texture_cache", "vertex_cache",
                               "queued_volume_alias_loads", "cpu_write_queue_decisions", "cpu_memory_writes", "vertex_buffer_pool", "full_cpu_texture_overlays", "raw_gpu_float_cpu_textures", "cpu_texture_drains", "cpu_image_slot_lookup", "texture_layout_cache", "resolve_image_pool", "resolve_descriptor_pool", "pass_transfer_descriptor_pool", "pass_transfer_render_pass_cache", "resolve_identity_views", "texture_eviction_reload_census", "fsi_raster_attachment", "draw_descriptors", "unused_descriptor_sets", "draw_descriptor_content_cache", "constant_delta_uploads", "pinned_null_constant_uploads", "atlas_depth_region_restores", "fused_atlas_depth_clear", "direct_atlas_depth_resolves", "packed_primary_stencil_clear", "direct_packed_color_resolves", "packed_secondary_stencil_clear", "cpu_write_cohorts", "completion_causes", "cpu_write_upload_reuse", "fused_packed_atlas_resolves", "producer_resource_handoffs", "cpu_stage_timings", "draw_recording", "draw_command_groups", "retained_opaque_phase_render_pass_groups", "packed_color_render_pass_groups", "packed_depth_render_pass_groups", "native_readonly_depth_no_write_metadata", "dummy_fsi_render_pass", "attachmentless_fsi_render_pass", "draw_barriers", "compact_resolve_channels", "canonical_resolve_stores", "gpu_batch_attribution", "gpu_physical_timings", "cpu_texture_footprint_cache", "gpu_draw_timings", "presentation_gpu_timings", "packed_zero_depth_clear", "full_color_clear_restore", "depth_stencil_restores", "builtin_atomic_depth", "builtin_readonly_queries", "native_readonly_depth_attachment", "native_early_depth", "native_early_depth_catalog", "native_early_depth_guard_reuse","native_early_depth_material_retention", "retained_opaque_writer", "retained_opaque_catalog","retained_opaque_phase", "retained_fp24_depth_writer", "retained_disjoint_queries", "fsi_immutable_state_hoist", "exact_fsi_stores", "native_stencil_material_cutoff", "opaque_color_overwrite", "forward_control_flow", "forward_vertex_control_flow", "forward_pixel_catalog", "fsi_state_specialization", "fsi_state_material_composition")}
    frame_intervals = []
    for line in log.splitlines():
        match = re.search(r"Native frame intervals API (\d+)\.\.(\d+): samples=(\d+), median=([\d.]+) ms, p95=([\d.]+) ms, p99=([\d.]+) ms, max=([\d.]+) ms, over50ms=(\d+), over100ms=(\d+), final_operations=(\d+)", line)
        if match:
            names = ("first_frame", "last_frame", "samples", "median_ms", "p95_ms", "p99_ms", "max_ms", "over50ms", "over100ms", "final_operations")
            entry = {name: (float(match[index]) if name.endswith("_ms") else int(match[index]))
                     for index, name in enumerate(names, 1)}
            entry["elapsed_seconds"] = elapsed(line)
            frame_intervals.append(entry)
    profile = None
    profile_window = None
    if args.profile_cpu_seconds:
        profile_path = args.output / "cpu-profile.json"
        profile = json.loads(profile_path.read_text()) if profile_path.exists() else {"error": "No profiling status was recorded"}
        data_path = args.output / "gameplay.data"
        profile["data_bytes"] = data_path.stat().st_size if data_path.is_file() else 0
        profile["completed"] = (profile.get("started", False) and profile.get("returncode") == 0
                                and not profile.get("interrupted_at_game_end", False) and profile["data_bytes"] > 0)
        if profile.get("started") and log_started:
            begin = (datetime.fromisoformat(profile["start_wall_time"]) - log_started).total_seconds()
            end = ((datetime.fromisoformat(profile["end_wall_time"]) - log_started).total_seconds()
                   if "end_wall_time" in profile else float("inf"))
            profile_window = (begin, end)
            profile["log_elapsed_window_seconds"] = [begin, end if math.isfinite(end) else None]
    # Frame aggregates may include several earlier query-prefix submissions.
    # Use the preceding logged frame as a conservative lower bound; never
    # include an aggregate that may overlap the profiler in a timing comparison.
    frame_starts = {}
    previous = 0.0
    for sample in frame_timings:
        frame_starts[sample["frame"]] = previous
        if sample["elapsed_seconds"] is not None:
            previous = sample["elapsed_seconds"]
    for samples in (frame_timings, operation_timings, query_pressure, draw_timings, draw_batches, texture_updates, transfer_batches, gpu_work_queue, alias_generations, gpu_timings, *resource_cache_samples.values()):
        mark_profile_overlap(samples, profile_window, frame_starts)
    mark_profile_overlap(frame_intervals, profile_window)
    process_status_path = args.output / "status.json"
    process_status = json.loads(process_status_path.read_text()) if process_status_path.exists() else {}
    input_status_path = args.output / "input-results.json"
    input_results = json.loads(input_status_path.read_text()) if input_status_path.exists() else []
    failed_inputs = len(input_results) != len(actions) or any(not item.get("completed", False) for item in input_results)
    capture_result = capture_status(capture_settings)
    final_counts = re.search(r"Native renderer final counters: frames=(\d+), query_prefixes=(\d+), raster_packets=(\d+), copy_packets=(\d+), matched_packets=(\d+), unmatched_packets=(\d+), no_effect_packets=(\d+), reference_retired=(true|false)", log)
    final_counters = {}
    if final_counts:
        names = ("frames", "query_prefixes", "raster_packets", "copy_packets", "matched_packets", "unmatched_packets", "no_effect_packets")
        final_counters = {name: int(final_counts.group(index + 1)) for index, name in enumerate(names)}
        final_counters["reference_retired"] = final_counts.group(8) == "true"
    def final_key_values(label):
        matches = re.findall(re.escape(label) + r": ([^\n]+)", log)
        if not matches:
            return {}
        return {key: value == "true" if value in ("true", "false") else int(value)
                for key, value in re.findall(r"(\w+)=(true|false|\d+)", matches[-1])}
    deferred_counters = final_key_values("Native deferred query counters")
    ticket_counters = final_key_values("Native query deferral summary")
    strict_counters = final_key_values("Strict Native scene summary")
    strict_keys = ("xenia_draw", "xenia_copy", "xenia_query", "xenia_swap_source", "xenia_import", "xenia_restore")
    strict_unmixed = bool(strict_counters) and all(strict_counters.get(key) == 0 for key in strict_keys)
    allocator_report = args.output / "allocator.json"
    allocator = json.loads(allocator_report.read_text()) if allocator_report.exists() else None
    if allocator:
        allocator["unchanged_after_run"] = (hashlib.sha256(Path(allocator["path"]).read_bytes()).hexdigest()
                                            == allocator["sha256"])
        allocator_report.write_text(json.dumps(allocator, indent=2) + "\n")
    status = {"hardware_driver_requested": args.hardware,
              "allocator_library": allocator,
              "build_identity_before_launch": build_identity,
              "guest_command_line_requested": args.guest_command_line,
              "reference_only_requested": args.reference_only,
              "weapon_input_requested": args.weapon_input,
              "camera_input_requested": args.camera_input,
              "texture_probe": probe_settings,
              "capture": capture_result,
              "input_actions_complete": not failed_inputs,
              "compositor_renderer": "gles2" if args.hardware else "pixman",
              "native_draw_batch_size": args.draw_batch_size,
              "native_query_capacity_override": args.native_query_capacity,
              "native_submission_capacity_override": args.native_submission_capacity,
              "native_snapshot_workers_override": args.native_snapshot_workers,
              "native_lazy_state_identities_override": args.native_lazy_state_identities,
              "native_direct_resource_refs_override": args.native_direct_resource_refs,
              "native_shader_prewarm_override": args.native_shader_prewarm,
              "native_retain_fsi_raster_extent_override": args.native_retain_fsi_raster_extent,
              "native_cache_texture_layouts_override": args.native_cache_texture_layouts,
              "native_associative_texture_layouts_override": args.native_associative_texture_layouts,
              "native_block_page_versions_override": args.native_block_page_versions,
              "native_range_texture_versions_override": args.native_range_texture_versions,
              "native_coalesced_texture_ranges_override": args.native_coalesced_texture_ranges,
              "native_compact_cpu_texture_version_pages_override": args.native_compact_cpu_texture_version_pages,
              "native_fast_snapshot_fingerprints_override": args.native_fast_snapshot_fingerprints,
              "native_cache_gpu_alias_states_override": args.native_cache_gpu_alias_states,
              "native_queued_volume_alias_loads_override": args.native_queued_volume_alias_loads,
              "native_shader_lookup_lookaside_override": args.native_shader_lookup_lookaside,
              "native_recycle_resolve_images_override": args.native_recycle_resolve_images,
              "anisotropic_override_requested": args.anisotropic_override,
              "native_recycle_resolve_descriptors_override": args.native_recycle_resolve_descriptors,
              "native_recycle_pass_transfer_descriptors_override": args.native_recycle_pass_transfer_descriptors,
              "native_recycle_pass_transfer_render_passes_override": args.native_recycle_pass_transfer_render_passes,
              "native_reuse_resolve_identity_views_override": args.native_reuse_resolve_identity_views,
              "native_diagnose_texture_eviction_reloads_override": args.native_diagnose_texture_eviction_reloads,
              "native_query_poll_yield_override": args.native_query_poll_yield,
              "native_inline_getdata_register_helpers_override": args.native_inline_getdata_register_helpers,
              "native_pipeline_prewarm_override": args.native_pipeline_prewarm,
              "native_prewarm_overflow_pipelines_override": args.native_prewarm_overflow_pipelines,
              "native_compact_resolve_channels_override": args.native_compact_resolve_channels,
              "native_batch_draw_descriptors_override": args.native_batch_draw_descriptors,
              "native_skip_unused_descriptor_sets_override": args.native_skip_unused_descriptor_sets,
              "native_cache_draw_descriptors_override": args.native_cache_draw_descriptors,
              "native_delta_constant_uploads_override": args.native_delta_constant_uploads,
              "native_pinned_null_constant_uploads_override": args.native_pinned_null_constant_uploads,
              "native_atlas_depth_region_restores_override": args.native_atlas_depth_region_restores,
              "native_fused_atlas_depth_clear_override": args.native_fused_atlas_depth_clear,
              "native_direct_atlas_depth_resolves_override": args.native_direct_atlas_depth_resolves,
              "native_packed_primary_stencil_clear_override": args.native_packed_primary_stencil_clear,
              "native_direct_packed_color_resolves_override": args.native_direct_packed_color_resolves,
              "native_packed_secondary_stencil_clear_override": args.native_packed_secondary_stencil_clear,
              "native_diagnose_cpu_write_cohorts_override": args.native_diagnose_cpu_write_cohorts,
              "native_diagnose_completion_causes_override": args.native_diagnose_completion_causes,
              "native_reuse_cpu_write_upload_override": args.native_reuse_cpu_write_upload,
              "native_canonical_resolve_stores_override": args.native_canonical_resolve_stores,
              "native_fused_packed_atlas_resolves_override": args.native_fused_packed_atlas_resolves,
              "native_packed_zero_depth_clear_override": args.native_packed_zero_depth_clear,
              "native_skip_full_color_clear_restore_override": args.native_skip_full_color_clear_restore,
              "native_stencil_export_restores_override": args.native_stencil_export_restores,
              "native_builtin_readonly_depth_queries_override": args.native_builtin_readonly_depth_queries,
              "native_fsi_hoist_mode_override": args.native_fsi_hoist_mode,
              "native_exact_fsi_stores_override": args.native_exact_fsi_stores,
              "native_retained_fp24_depth_writer_override": args.native_retained_fp24_depth_writer,
              "native_retained_opaque_writer_override": args.native_retained_opaque_writer,
              "native_retained_opaque_catalog_override": args.native_retained_opaque_catalog,
              "native_retained_opaque_phase_override": args.native_retained_opaque_phase,
              "native_retained_opaque_phase_render_pass_groups_override": args.native_retained_opaque_phase_render_pass_groups,
              "native_reuse_retained_opaque_resources_override": args.native_reuse_retained_opaque_resources,
              "native_retained_shared_depth_queries_override": args.native_retained_shared_depth_queries,
              "native_retain_primary_across_disjoint_queries_override": args.native_retain_primary_across_disjoint_queries,
              "native_retained_d32_composition_override": args.native_retained_d32_composition,
              "native_queue_retained_exports_for_draws_override": args.native_queue_retained_exports_for_draws,
              "native_readonly_depth_attachment_override": args.native_readonly_depth_attachment,
              "native_readonly_depth_no_write_metadata_override": args.native_readonly_depth_no_write_metadata,
              "native_readonly_depth_conservative_override": args.native_readonly_depth_conservative,
              "native_early_depth_override": args.native_early_depth,
              "native_reuse_early_depth_readonly_boundaries_override": args.native_reuse_early_depth_readonly_boundaries,
              "native_early_depth_material_retention_override": args.native_early_depth_material_retention,
              "native_early_depth_catalog_override": args.native_early_depth_catalog,
              "native_readonly_depth_skip_edram_barrier_override": args.native_readonly_depth_skip_edram_barrier,
              "native_readonly_depth_read_only_pass_override": args.native_readonly_depth_read_only_pass,
              "native_readonly_depth_lower_bound_query_override": args.native_readonly_depth_lower_bound_query,
              "native_host_query_reset_override": args.native_host_query_reset,
              "native_readonly_query_render_pass_groups_override": args.native_readonly_query_render_pass_groups,
              "native_builtin_atomic_depth_override": args.native_builtin_atomic_depth,
              "native_stencil_material_cutoff_override": args.native_stencil_material_cutoff,
              "native_opaque_color_overwrite_override": args.native_opaque_color_overwrite,
              "native_forward_control_flow_override": args.native_forward_control_flow,
              "native_forward_vertex_control_flow_override": args.native_forward_vertex_control_flow,
              "native_specialize_fsi_pipeline_state_override": args.native_specialize_fsi_pipeline_state,
              "native_fsi_state_material_composition_override": args.native_fsi_state_material_composition,
              "native_fsi_state_inactive_early_catalog_override": args.native_fsi_state_inactive_early_catalog,
              "native_fsi_action_state_overflow_lookup_override": args.native_fsi_action_state_overflow_lookup,
              "native_fsi_state_forward_cfg_override": args.native_fsi_state_forward_cfg,
              "native_forward_pixel_catalog_override": args.native_forward_pixel_catalog,
              "native_four_inflight_batches_override": args.native_four_inflight_batches,
              "native_submission_worker_override": args.native_submission_worker,
              "native_certified_snapshots_override": args.native_certified_snapshots,
              "native_stable_certified_snapshots_override": args.native_stable_certified_snapshots,
              "native_batch_texture_snapshots_override": args.native_batch_texture_snapshots,
              "native_batch_texture_snapshots_startup": batch_texture_startup,
              "native_batch_texture_snapshots_shutdown": batch_texture_shutdown,
              "native_certified_min_snapshot_bytes_override": args.native_certified_min_snapshot_bytes,
              "native_certified_fault_fastpath_override": args.native_certified_fault_fastpath,
              "native_certified_batch_queries_override": args.native_certified_batch_queries,
              "native_certified_diagnostics_override": args.native_certified_diagnostics,
              "native_implicit_fsi_barriers_override": args.native_implicit_fsi_barriers,
              "native_retire_query_prefixes_override": args.native_retire_query_prefixes,
              "native_cache_coherence_waits_override": args.native_cache_coherence_waits,
              "native_submit_completion_markers_override": args.native_submit_completion_markers,
              "bulk_type0_constants_override": args.bulk_type0_constants,
              "native_queue_cpu_memory_writes_override": args.native_queue_cpu_memory_writes,
              "native_recycle_vertex_buffers_override": args.native_recycle_vertex_buffers,
              "native_raw_gpu_float_cpu_textures_override": args.native_raw_gpu_float_cpu_textures,
              "native_defer_cpu_texture_drains_override": args.native_defer_cpu_texture_drains,
              "native_cpu_image_slot_lookup_override": args.native_cpu_image_slot_lookup,
              "native_diagnose_full_cpu_texture_overlays_override": args.native_diagnose_full_cpu_texture_overlays,
              "native_reset_draw_command_pool_override": args.native_reset_draw_command_pool,
              "native_time_draw_recording_override": args.native_time_draw_recording,
              "native_time_cpu_stages_override": args.native_time_cpu_stages,
              "native_published_query_results_override": args.native_published_query_results,
              "native_query_readiness_census_override": args.native_query_readiness_census,
              "native_query_wait_census_override": args.native_query_wait_census,
              "native_reuse_defined_ranges_override": args.native_reuse_defined_ranges,
              "native_multi_draw_command_buffers_override": args.native_multi_draw_command_buffers,
              "native_packed_color_render_pass_groups_override": args.native_packed_color_render_pass_groups,
              "native_packed_depth_render_pass_groups_override": args.native_packed_depth_render_pass_groups,
              "native_discard_dummy_fsi_attachment_override": args.native_discard_dummy_fsi_attachment,
              "native_attachmentless_fsi_override": args.native_attachmentless_fsi,
              "native_snapshot_metadata_lru_override": args.native_snapshot_metadata_lru,
              "native_thin_live_metadata_override": args.native_thin_live_metadata,
              "native_shared_providers_override": args.native_shared_providers,
              "native_coalesce_draw_barriers_override": args.native_coalesce_draw_barriers,
              "native_skip_readonly_sample_barriers_override": args.native_skip_readonly_sample_barriers,
              "native_precise_fsi_barriers_override": args.native_precise_fsi_barriers,

              "close_during_pipeline_prewarm_requested": args.close_during_pipeline_prewarm,
              "native_query_deferral_requested": args.raster and not args.synchronous_native_queries,
              "native_transfer_batching_requested": args.raster and not args.synchronous_native_transfers,
              "native_unified_queue_requested": args.raster and not (args.separate_native_submissions or args.synchronous_native_transfers),
              "native_async_submissions_requested": args.raster and args.async_native_submissions,
              "native_gpu_timestamps_requested": args.raster and args.native_gpu_timestamps,
              "native_gpu_batch_attribution_requested": args.raster and args.native_gpu_batch_attribution,
              "native_gpu_physical_timestamps_requested": args.raster and args.native_gpu_physical_timestamps,
              "native_gpu_draw_timestamps_requested": args.raster and args.native_gpu_draw_timestamps,
              "native_detailed_reports_requested": args.native_detailed_reports,
              "native_texture_retention_requested": not args.no_native_texture_retention,
              "native_dormant_texture_entries_override": args.native_dormant_texture_entries,
              "native_vertex_retention_requested": args.native_vertex_retention,
              "frame_limit": args.frame_limit,
              "cpu_profile": profile,
              "timing_comparison_policy": "Exclude samples with include_in_timing_comparison=false; profiling overlap uses conservative aggregate bounds",
              "native_cache_seed": cache_seed,
              "native_cache_preserved": cache_preserved,
              "fsi_state_recipe_cache_seed": fsi_state_recipe_seed,
              "fsi_state_shader_cache_seed": fsi_state_shader_seed,
              "fsi_state_forward_shader_cache_seed": fsi_state_forward_shader_seed,
              "fsi_state_recipe_cache_preserved": fsi_state_recipe_preserved,
              "fsi_state_shader_cache_preserved": fsi_state_shader_preserved,
              "fsi_state_forward_shader_cache_preserved": fsi_state_forward_shader_preserved,
              "pipeline_cache_seed": pipeline_cache_seed,
              "pipeline_cache_preserved": pipeline_cache_preserved,
              "mesa_cache_seed": mesa_cache_seed,
              "mesa_cache_preserved": mesa_cache_preserved,
              "final_native_counters": final_counters,
              "final_deferred_query_counters": deferred_counters,
              "final_query_ticket_counters": ticket_counters,
              "strict_scene_counters": strict_counters,
              "strict_native_unmixed": strict_unmixed,
              "frame_timing_samples": frame_timings,
              "operation_timing_samples": operation_timings,
              "query_pressure_samples": query_pressure,
              "draw_timing_samples": draw_timings,
              "draw_batch_samples": draw_batches,
              "texture_update_samples": texture_updates,
              "transfer_batch_samples": transfer_batches,
              "gpu_work_queue_samples": gpu_work_queue,
              "alias_generation_samples": alias_generations,
              "gpu_timing_samples": gpu_timings,
              "resource_cache_samples": resource_cache_samples,
              "frame_interval_samples": frame_intervals,
              "native_started": "Native renderer enabled from guest boot" in log,
              "native_ownership_requested": args.ownership,
              "native_raster_requested": args.raster,
              "logged_native_query_publications": len(re.findall(r"Native query publication \d+", log)),
              "last_logged_native_raster_packets": max([int(value) for value in re.findall(r"(\d+) raster packets", log)], default=0),
              "logged_native_packet_replacements": len(re.findall(r"Native packet replacement \d+:", log)),
              "fatal_native_transaction": "Fatal native packet transaction" in log or "Native renderer fatal error:" in log,
              "ordered_pixel_interlock": "Native raster output: ordered pixel interlock" in log,
              "last_logged_query_prefix": max(query_prefixes, default=None),
              "logged_query_comparisons": len(comparisons),
              "logged_verified_queries": sum(item["verified"] for item in comparisons),
              "logged_exact_queries": sum(item["exact_matches"] for item in comparisons),
              "logged_visibility_mismatches": sum(item["visibility_mismatches"] for item in comparisons),
              "logged_prefix_verified_queries": sum(item["verified"] for item in prefix_comparisons),
              "logged_prefix_exact_queries": sum(item["exact_matches"] for item in prefix_comparisons),
              "logged_prefix_visibility_mismatches": sum(item["visibility_mismatches"] for item in prefix_comparisons),
              "logged_frame_samples": len(sampled_frames),
              "last_logged_api_frame": max(sampled_frames, default=None),
              "completed_native_frames_at_failure": int(completed.group(1)) if completed else None,
              "last_completed_api_frame_at_failure": int(completed.group(2)) if completed else None,
              "process_status_recorded": bool(process_status),
              "process_exit_before_timeout": process_status.get("early_exit"),
              "clean_ui_close": process_status.get("clean_ui_close", False),
              "native_failure": any("disabled at API frame" in line or "Native renderer unavailable" in line or
                                    "Native renderer startup failed" in line or "Native renderer fatal error" in line
                                    for line in native_lines),
              "fallback": final_counters.get("reference_retired", False) or "using the reference backend" in log,
              "diagnostics": native_lines}
    (args.output / "result.json").write_text(json.dumps(status, indent=2) + "\n")
    # Full evidence stays in result.json; avoid flooding the terminal with
    # hundreds of periodic query records and timing samples after each run.
    summary = {key: value for key, value in status.items()
               if key not in ("diagnostics", "frame_timing_samples", "operation_timing_samples", "query_pressure_samples", "draw_timing_samples",
                              "draw_batch_samples", "texture_update_samples", "transfer_batch_samples", "gpu_work_queue_samples", "alias_generation_samples", "gpu_timing_samples", "resource_cache_samples", "frame_interval_samples")}
    summary["report"] = str(args.output / "result.json")
    summary["last_frame_timing_sample"] = frame_timings[-1] if frame_timings else None
    summary["last_draw_timing_sample"] = draw_timings[-1] if draw_timings else None
    summary["last_frame_interval_sample"] = frame_intervals[-1] if frame_intervals else None
    summary["last_draw_batch_sample"] = draw_batches[-1] if draw_batches else None
    summary["last_query_pressure_sample"] = query_pressure[-1] if query_pressure else None
    print(json.dumps(summary, indent=2))
    unexpected_exit = process_status.get("early_exit") is not None and not status["clean_ui_close"]
    failed_close = process_status.get("close_requested", False) and not status["clean_ui_close"]
    failed_profile = profile is not None and not profile["completed"]
    failed_capture = capture_result is not None and (not capture_result["completed"] or
                      args.capture_reference_history and not capture_result["history_pair_present"])
    failed_renderer = (not status["clean_ui_close"] or status["native_started"]) if args.reference_only else (
        not sampled_frames or status["fallback"] or status["native_failure"])
    forced_fallback = (args.fallback_after_packets or args.fallback_query_draws or
                       args.fallback_before_query_draw or args.fallback_after_deferred_queries)
    if forced_fallback:
        reason = ("Diagnostic fatal error after deferred query end" if args.fallback_after_deferred_queries else
                  "Explicit diagnostic fatal error after an accepted native query draw" if args.fallback_query_draws else
                  "Explicit diagnostic native fatal-error trigger")
        failed_renderer = (not status["native_started"] or not status["fatal_native_transaction"] or
                           reason not in log or not strict_unmixed or
                           final_counters.get("reference_retired", False))
        # Deliberate terminal errors may abort before later scripted actions.
        # Require a real process exit, the exact injected reason and zero legacy
        # scene operations; a hung test or a different crash still fails.
        unexpected_exit = process_status.get("early_exit") is None
        failed_close = False
        failed_inputs = any(not item.get("completed", False) for item in input_results)
    elif args.raster:
        failed_renderer = failed_renderer or not strict_unmixed
    if args.close_during_pipeline_prewarm:
        failed_renderer = (not status["native_started"] or not status["clean_ui_close"] or status["fallback"] or
                           status["fatal_native_transaction"] or "Initializing shader storage for title" not in log or
                           any("Observed native pipeline preparation: loaded=" in line and "cancelled=false" in line
                               for line in log.splitlines()))
    failed_allocator = bool(args.allocator_library and
                            (not allocator or not allocator.get("mapped") or not allocator.get("unchanged_after_run")))
    raise SystemExit(failed_renderer or not process_status or unexpected_exit or failed_close or failed_profile or failed_inputs or failed_capture or failed_allocator)


if __name__ == "__main__":
    main()
