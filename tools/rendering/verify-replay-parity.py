#!/usr/bin/env python3
"""Compare native replay outputs while ignoring timing and allocation counters."""
import argparse
import hashlib
import json
from pathlib import Path


def verify(baseline: Path, candidate: Path) -> dict:
    reports = [json.loads((folder / "replay.json").read_text())
               for folder in (baseline, candidate)]
    problems = []
    for name, report in zip(("baseline", "candidate"), reports):
        if not report.get("requested_range_complete"):
            problems.append(f"{name}: requested replay range did not complete")
        if report.get("valid_for_equivalence") is False:
            problems.append(f"{name}: replay reports invalid equivalence inputs")
    coverage_keys = ("draws", "clears", "resolves", "completed_frames", "last_completed_event")
    coverage = {}
    for key in coverage_keys:
        values = [report.get(key) for report in reports]
        coverage[key] = values
        if values[0] is None or values[0] != values[1]:
            problems.append(f"coverage differs: {key}")
    queries = [report.get("native_occlusion_results", []) for report in reports]
    if queries[0] != queries[1]:
        problems.append("native occlusion query results differ")
    names = [set(path.name for path in folder.iterdir()
                 if path.suffix in (".rgba32f", ".ppm"))
             for folder in (baseline, candidate)]
    if names[0] != names[1]:
        problems.append("rendered image file sets differ")
    if not any(name.endswith(".rgba32f") for name in names[0]):
        problems.append("baseline has no raw RGBA32F images")
    comparisons = []
    for name in sorted(names[0] | names[1]):
        info = []
        for folder in (baseline, candidate):
            path = folder / name
            data = path.read_bytes() if path.is_file() else None
            info.append(None if data is None else
                        {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        identical = info[0] is not None and info[0] == info[1]
        comparisons.append({"file": name, "identical": identical,
                            "baseline": info[0], "candidate": info[1]})
        if not identical:
            problems.append(f"image differs: {name}")
    return {"baseline": str(baseline), "candidate": str(candidate),
            "passed": not problems, "problems": problems, "coverage": coverage,
            "queries_compared": len(queries[0]), "query_results_identical": queries[0] == queries[1],
            "raw_images": sum(name.endswith(".rgba32f") for name in names[0]),
            "ppm_images": sum(name.endswith(".ppm") for name in names[0]),
            "comparisons": comparisons}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = verify(args.baseline, args.candidate)
    output = args.output or args.candidate / "replay-parity.json"
    output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: value for key, value in result.items() if key != "comparisons"}, indent=2))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
