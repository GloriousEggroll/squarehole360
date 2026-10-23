#!/usr/bin/env python3
"""Run the real GTK window-mode test in a private headless compositor.

This leaves the user's compositor, tiling rules, configuration and game alone.
It requires labwc, wlr-randr and Mesa lavapipe (Vulkan through wl_shm, which
works with the isolated software compositor); Xwayland is needed for x11.
"""
import argparse
import json
import os
import platform
from pathlib import Path
import shlex
import subprocess
import tempfile
from PIL import Image
import numpy as np

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--backend", choices=("wayland", "x11"), default="wayland")
parser.add_argument("--scale", type=float, choices=(1.0, 1.25, 2.0), default=1.0)
parser.add_argument("--windowed-start", action="store_true")
args = parser.parse_args()
if args.backend == "x11" and args.scale != 1:
    parser.error("fractional-scale validation uses native Wayland")
drivers = Path("/usr/share/vulkan/icd.d")
lavapipe = next((p for p in (drivers / f"lvp_icd.{platform.machine()}.json",
                           drivers / "lvp_icd.json") if p.is_file()), None)
if lavapipe is None:
    parser.error("Mesa lavapipe Vulkan driver is required for the isolated compositor")
suffix = "" if args.scale == 1 else f"-scale-{args.scale:g}"
if args.windowed_start:
    suffix += "-windowed"
report = root / "reports/rendering/native-renderer" / f"window-size-{args.backend}{suffix}.log"
screenshots = report.with_suffix("")
screenshots.mkdir(exist_ok=True)
for stage in range(7):
    for extension in ("png", "json"):
        (screenshots / f"stage-{stage}.{extension}").unlink(missing_ok=True)
with tempfile.TemporaryDirectory(prefix="gow3-window-test-") as folder:
    temporary = Path(folder)
    config = temporary / "config/labwc"
    config.mkdir(parents=True)
    (config / "rc.xml").write_text("<labwc_config/>\n")
    (config / "autostart").write_text("")
    env = os.environ.copy()
    env.update(XDG_RUNTIME_DIR=folder, XDG_CONFIG_HOME=str(temporary / "config"),
               WLR_BACKENDS="headless", WLR_HEADLESS_OUTPUTS="1", WLR_RENDERER="pixman",
               GOW3_WINDOW_TEST_BACKEND=args.backend, GOW3_WINDOW_TEST_SCALE=str(args.scale),
               VK_DRIVER_FILES=str(lavapipe), DISABLE_OBS_VKCAPTURE="1")
    env["GOW3_WINDOW_TEST_SCREENSHOTS"] = str(screenshots)
    if args.windowed_start:
        env["GOW3_WINDOW_TEST_START_WINDOWED"] = "1"
    # Prevent the nested session from selecting either desktop display.
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    command = "bash " + shlex.quote(str(root / "tools/rendering/window-size-session.sh"))
    result = subprocess.run(["labwc", "-C", str(config), "-S", command], cwd=root, env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=35)
    report.write_text(result.stdout)
    print(result.stdout)
    # labwc can exit successfully even when its session command failed.
    expected = ("PASS: startup fullscreen -> 1280x720", "PASS: live resize -> 1024x576",
                "PASS: second fullscreen -> configured 1280x720",
                "PASS: borderless renders at 2560x1440 (stage 0)",
                "PASS: borderless renders at 2560x1440 (stage 3)")
    failed = bool(result.returncode or "FAIL" in result.stdout or
                  not all(line in result.stdout for line in expected))
    visual_results = []
    for stage in range(7 if args.windowed_start else 5):
        png = screenshots / f"stage-{stage}.png"
        if not png.is_file():
            failed = True
            visual_results.append(f"FAIL: missing stage {stage} screenshot")
            continue
        pixels = np.array(Image.open(png).convert("RGB"))
        red, green, blue = (pixels[:,:,i] for i in range(3))
        frame = (red > 240) & (green < 15) & (blue < 15)
        center = (blue > 240) & (red < 15) & (green < 15)
        uncovered = (green > 240) & (blue > 240) & (red < 15)
        fy, fx = np.where(frame)
        cy, cx = np.where(center)
        expected_size = json.loads(png.with_suffix(".json").read_text())
        ok = bool(len(fx) and len(cx))
        bounds = None
        if ok:
            bounds = (int(fx.max()-fx.min()+1),int(fy.max()-fy.min()+1))
            # A fractional-scale surface edge may straddle a physical pixel.
            tolerance = 1 if args.scale % 1 else 0
            ok = (abs(bounds[0]-expected_size["width"]) <= tolerance and
                  abs(bounds[1]-expected_size["height"]) <= tolerance and
                  abs(float(cx.mean())-(fx.min()+fx.max())/2) <= 1 and
                  abs(float(cy.mean())-(fy.min()+fy.max())/2) <= 1 and
                  not np.any(uncovered))
        failed |= not ok
        visual_results.append(f"{'PASS' if ok else 'FAIL'}: visual stage {stage}: "
                              f"visible frame {bounds}, expected {expected_size}, "
                              f"uncovered parent pixels {np.count_nonzero(uncovered)}")
    with report.open("a") as output:
        output.write("\n".join(visual_results)+"\n")
    print("\n".join(visual_results))
    raise SystemExit(failed)
