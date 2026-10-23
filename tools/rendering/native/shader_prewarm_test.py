#!/usr/bin/env python3
"""CPU-only CLI regressions using two shaders from a user-owned manifest."""
import argparse
import hashlib
import json
import pathlib
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tool", type=pathlib.Path)
    parser.add_argument("manifest", type=pathlib.Path)
    args = parser.parse_args()
    tool = args.tool.resolve()
    manifest = args.manifest.resolve()
    available = json.loads(manifest.read_text())["shaders"]
    selected = []
    for stage in ("vertex", "pixel"):
        source = dict(next(item for item in available if item["stage"] == stage))
        source["path"] = str((manifest.parent / source["path"]).resolve())
        selected.append(source)
    originals = {item["path"]: hashlib.sha256(pathlib.Path(item["path"]).read_bytes()).hexdigest()
                 for item in selected}
    checks = []
    with tempfile.TemporaryDirectory(prefix="gow3-prewarm-test-") as directory:
        root = pathlib.Path(directory)
        subset = root / "subset.json"
        subset.write_text(json.dumps({"shaders": selected}))
        cache, spirv = root / "cache", root / "spirv"

        def run(arguments, success=True):
            process = subprocess.run([str(tool), *map(str, arguments)], text=True,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            report = json.loads(process.stdout)
            assert (process.returncode == 0) == success, process.stderr
            assert report["success"] == success, report
            assert "sources" not in report, "Per-source details leaked into terminal summary"
            progress = [line for line in process.stderr.splitlines() if line.startswith("Shaders ")]
            assert len(progress) <= 101, "Progress was not bounded by percentage changes"
            return report

        command = ["--cache", cache, "--jobs", "1000", "--spirv-output", spirv, subset, subset]
        cold = run(command)
        assert cold["unique_sources"] == 2 and cold["duplicates_removed"] == 2
        assert cold["workers"] == 2 and cold["variants_succeeded"] == 4
        assert cold["variants_failed"] == 0 and cold["disk_cache_hits"] == 0
        assert len(list(spirv.glob("*.spv"))) == 4
        checks.append("cold raster/FSI/depth-only compilation, deduplication and worker bound")
        warm = run(command)
        assert warm["disk_cache_hits"] == 4 and warm["variants_succeeded"] == 4
        full_report = json.loads(pathlib.Path(warm["report_path"]).read_text())
        assert len(full_report.pop("sources")) == 3
        assert full_report == warm
        checks.append("all variants persisted, validated and reused")
        checks.append("compact terminal summary preserves full per-source report on disk")

        # Bad inputs must fail before even creating their requested cache.
        for label, mutation in (("hash", {"sha256": "0" * 64}),
                                ("stage", {"stage": "pixel"}),
                                ("missing_path", {"path": "missing.bin"})):
            invalid = root / f"{label}.json"
            invalid.write_text(json.dumps({"shaders": [selected[0] | mutation]}))
            destination = root / f"bad-{label}"
            run(["--cache", destination, invalid], False)
            assert not destination.exists()
            checks.append(f"{label} preflight failure leaves outputs untouched")
        no_jobs = root / "bad-jobs"
        run(["--cache", no_jobs, "--jobs", "0", subset], False)
        assert not no_jobs.exists()
        checks.append("invalid worker count rejected before output")

        run(["--cache", root / "bad-overlap", "--spirv-output", root, subset], False)
        assert not (root / "bad-overlap").exists()
        checks.append("source-containing output directory rejected")

        # This source has valid manifest identity/stage but an invalid container.
        # Compilation failure is explicit; independent shaders still complete.
        broken = root / "broken.bin"
        broken.write_bytes(bytes.fromhex("102a1101"))
        broken_hash = hashlib.sha256(broken.read_bytes()).hexdigest()
        mixed = root / "mixed.json"
        mixed.write_text(json.dumps({"shaders": selected + [{"path": str(broken),
            "sha256": broken_hash, "stage": "vertex", "size": 4}]}))
        failed = run(["--cache", cache, "--jobs", "2", mixed], False)
        assert failed["variants_total"] == 5 and failed["variants_completed"] == 5
        assert failed["variants_failed"] == 1 and failed["variants_succeeded"] == 4
        assert failed["disk_cache_hits"] == 4
        failed_report = json.loads(pathlib.Path(failed["report_path"]).read_text())
        failure = next(item for item in failed_report["sources"] if item.get("source_sha256") == broken_hash)
        assert failure["variants"][0]["error"]
        checks.append("malformed container fails without silently skipping independent work")

        assert all(hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest() == digest
                   for path, digest in originals.items())
        checks.append("source assets remain unchanged")
        print(json.dumps({"success": True, "checks": checks, "cache_version": cold["cache_version"]}, indent=2))


if __name__ == "__main__":
    main()
