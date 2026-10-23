#!/usr/bin/env python3
"""Export a reference GPU trace's final guest image in a private compositor.

Linux diagnostic harness only; rendering/export use the portable SDK viewer.
The output is the presenter's guest image (including its gamma conversion),
without window scaling, HDR expansion, upscaling, or viewer overlays.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("output", type=Path, help="New output directory")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--export-state", action="store_true", help="Also export explicitly diagnostic GPU memory/EDRAM after replay")
    parser.add_argument("--command", type=int, default=-1, help="Viewer draw/swap command index, or -1 for the endpoint")
    parser.add_argument("--anisotropy", type=int, choices=range(-1, 6), default=5,
                        help="Texture anisotropy override: -1 uses guest state, 5 forces 16x")
    args = parser.parse_args()
    trace = args.trace.resolve(strict=True)
    output = args.output.resolve()
    if not 1 <= args.timeout <= 600:
        parser.error("--timeout must be between 1 and 600 seconds")
    output.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix="gow3-reference-replay-") as folder:
        temporary = Path(folder)
        config = temporary / "config/labwc"
        config.mkdir(parents=True)
        (config / "rc.xml").write_text("<labwc_config/>\n")
        (config / "autostart").write_text("")
        env = os.environ.copy()
        env.update(XDG_RUNTIME_DIR=folder, XDG_CONFIG_HOME=str(temporary / "config"),
                   XDG_CACHE_HOME=str(temporary / "cache"), XDG_DATA_HOME=str(temporary / "data"),
                   WLR_BACKENDS="headless", WLR_HEADLESS_OUTPUTS="1", WLR_RENDERER="pixman",
                   GDK_BACKEND="wayland", SDL_AUDIODRIVER="dummy", DISABLE_OBS_VKCAPTURE="1")
        for key in ("DISPLAY", "WAYLAND_DISPLAY", "DBUS_SESSION_BUS_ADDRESS"):
            env.pop(key, None)
        image = output / "reference.ppm"
        command = [str(ROOT / "build/bin/rex-gpu-vulkan-trace-viewer"),
                   f"--target_trace_file={trace}", "--trace_replay_on_load=true",
                   "--trace_exit_after_replay=true", f"--trace_output_path={image}",
                   "--vulkan_pipeline_creation_threads=0", "--render_target_path_vulkan=fsi",
                   "--async_shader_compilation=false", "--vulkan_async_shader_translation=false",
                   "--vulkan_unclamped_vertex_indices=true", "--gamma_render_target_as_unorm16=false",
                   "--native_2x_msaa=true", "--readback_memexport=false", f"--anisotropic_override={args.anisotropy}",
                   f"--cache_path={temporary / 'gpu-cache'}", f"--log_file={output / 'viewer.log'}"]
        command.append(f"--trace_replay_command={args.command}")
        if args.export_state:
            command.append(f"--trace_state_output_directory={output / 'gpu-state'}")
        # labwc parses its startup command through a shell; quote each argument.
        with (output / "console.log").open("w") as log:
            process = subprocess.Popen(["labwc", "-C", str(config), "-S", shlex.join(command)],
                                       cwd=temporary, env=env, stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            try:
                status = process.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                status = None
            finally:
                # This session contains only our compositor and diagnostic viewer.
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()
    success = image.is_file() and image.stat().st_size > 32
    with trace.open("rb") as source:
        digest = hashlib.file_digest(source, "sha256").hexdigest()
    with Path(command[0]).open("rb") as source:
        viewer_digest = hashlib.file_digest(source, "sha256").hexdigest()
    sidecar = trace.with_suffix(trace.suffix + ".initial.json")
    report = {"trace_sha256": digest, "trace": str(trace), "image_written": success,
              "viewer_sha256": viewer_digest, "viewer_arguments": command[1:],
              "compositor_exit": status, "image_space": "reference_presenter_guest_output",
              "synchronous_shaders_and_pipelines": True,
              "same_frame_native_comparison": False}
    if args.export_state:
        report["diagnostic_replayed_state"] = str(output / "gpu-state")
        try:
            state_path = output / "gpu-state/state.xtr"
            metadata = json.loads((output / "gpu-state/state.json").read_text())
            if metadata.get("provenance") != "diagnostic_replayed_gpu_state" or metadata.get("has_command_stream") is not False:
                raise ValueError("Diagnostic snapshot provenance is absent")
            spec = importlib.util.spec_from_file_location("diagnostic_history", Path(__file__).with_name("import-gpu-history.py"))
            history = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(history)
            if state_path.stat().st_size > 2 * history.MAX_MEMORY:
                raise ValueError("Diagnostic snapshot is too large")
            state_data = state_path.read_bytes()
            ranges, edram, gamma = history.initial_state(state_data, state_only=True)
            if not ranges or edram is None or gamma is None:
                raise ValueError("Diagnostic snapshot is missing GPU memory, EDRAM or gamma state")
            report["diagnostic_state_sha256"] = hashlib.sha256(state_data).hexdigest()
            report["diagnostic_state_memory_bytes"] = sum(len(blob) for _, blob in ranges)
            report["diagnostic_state_last_present"] = metadata["last_present_api_frame"]
            report["diagnostic_state_viewer_command"] = args.command
        except (OSError, ValueError, KeyError) as error:
            success = False
            report["diagnostic_state_error"] = str(error)
    if sidecar.is_file():
        report["initial_snapshot"] = json.loads(sidecar.read_text())
    (output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    raise SystemExit(not success)


if __name__ == "__main__":
    main()
