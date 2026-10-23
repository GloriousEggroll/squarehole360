#!/usr/bin/env python3
"""Compile extracted Gears shaders with the native XenosRecomp/DXC toolchain.

Compilation is a compatibility check, not a rendering-correctness test. Each
failure is retained and makes the command fail; no shader is silently omitted.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--converter", type=Path, help="Override the native XenosRecomp executable")
    parser.add_argument("--dxc", type=Path, help="Override the native DXC executable")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    limiter = shutil.which("prlimit")
    manifest = json.loads(args.manifest.read_text())
    source = args.manifest.parent.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    converter = args.converter or root / ("build-xenos/XenosRecomp/XenosRecomp.exe" if os.name == 'nt' else "build-xenos/XenosRecomp/XenosRecomp")
    sdk = root / "vendor/xenos-recomp"
    compiler = args.dxc or sdk / ("thirdparty/dxc-bin/bin/x64/dxc.exe" if os.name == 'nt' else "thirdparty/dxc-bin/bin/x64/dxc-linux")
    common = sdk / "XenosRecomp/shader_common.h"
    env = os.environ.copy()
    if os.name != 'nt':
        env["LD_LIBRARY_PATH"] = str(sdk / "thirdparty/dxc-bin/lib/x64") + (
            ":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
    # All platforms have the subprocess deadline. Where available, also bound
    # compiler address space/CPU and prevent assertion core dumps.
    limits = [limiter, "--as=1073741824", "--cpu=30", "--core=0", "--"] if limiter else []

    def compile_one(shader):
        stem = shader["sha256"]
        bytecode = source / shader["path"]
        result = {"sha256": stem, "stage": shader["stage"], "status": "failed"}
        if hashlib.sha256(bytecode.read_bytes()).hexdigest() != stem:
            result["error"] = "Input hash mismatch"
            return result
        hlsl, spirv = output / (stem + ".hlsl"), output / (stem + ".spv")
        # A failed rebuild must not leave an old shader looking usable.
        spirv.unlink(missing_ok=True)
        log = output / (stem + ".log")
        commands = [
            [str(converter), str(bytecode), str(hlsl), str(common)],
            [str(compiler), "-T", "ps_6_0" if shader["stage"] == "pixel" else "vs_6_0",
             "-E", "main", "-HV", "2021", "-spirv", "-fvk-use-dx-layout",
             *(["-fvk-invert-y"] if shader["stage"] == "vertex" else []),
             "-Fo", str(spirv), str(hlsl)],
        ]
        with log.open("w") as stderr:
            for command in commands:
                try:
                    proc = subprocess.run([*limits, *command],
                                          env=env, stdout=stderr, stderr=stderr, timeout=45)
                except subprocess.TimeoutExpired:
                    result.update(error="Shader compilation exceeded 45 seconds", log=log.name)
                    return result
                if proc.returncode:
                    result.update(error="Translation failed" if command == commands[0] else "SPIR-V compilation failed",
                                  return_code=proc.returncode, log=log.name)
                    return result
        binary = spirv.read_bytes()
        if len(binary) < 20 or len(binary) % 4 or binary[:4] != b"\x03\x02\x23\x07":
            result["error"] = "Invalid SPIR-V header/length"
            return result
        result.update(status="compiled", spirv=spirv.name, spirv_sha256=hashlib.sha256(binary).hexdigest())
        return result

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(compile_one, s) for s in manifest["shaders"]]
        for f in concurrent.futures.as_completed(futures):
            results.append(f.result())
            # Preserve completed work if the agent or build is interrupted.
            progress = output / "progress.json.tmp"
            progress.write_text(json.dumps(results, indent=2) + "\n")
            progress.replace(output / "progress.json")
            if len(results) % 25 == 0 or len(results) == len(futures):
                print(f"Compiled {len(results)}/{len(futures)}; failures: {sum(r['status'] != 'compiled' for r in results)}", flush=True)
    results.sort(key=lambda r: r["sha256"])
    failed = sum(r["status"] != "compiled" for r in results)
    report = {"validation": "Compilation only; image equivalence and shader semantics unverified",
              "source_sha256": manifest["source_sha256"], "compiled": len(results) - failed,
              "failed": failed, "shaders": results}
    (output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    raise SystemExit(bool(failed))


if __name__ == "__main__":
    main()
