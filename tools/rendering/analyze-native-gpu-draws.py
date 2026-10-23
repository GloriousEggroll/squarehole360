#!/usr/bin/env python3
"""Summarize sampled draw-list GPU latency in an anchored gameplay window."""
import argparse
import json
from collections import defaultdict
from pathlib import Path


def analyze(folder: Path, start: float, end: float) -> dict:
    result = json.loads((folder / "result.json").read_text())
    trace = json.loads((folder / "gpu-draws.json").read_text())
    anchor = json.loads((folder / "input-anchor.json").read_text())["elapsed"]
    # API-frame counters accompany completed GPU batches. Use two observations
    # strictly inside the wall-time window rather than interpolate frame IDs
    # from presentation counters or assume every presented frame has GPU work.
    observations = [row for row in result["gpu_timing_samples"]
                    if anchor + start <= row["elapsed_seconds"] <= anchor + end
                    and row["include_in_timing_comparison"]]
    if len(observations) < 2:
        raise ValueError("Not enough completed GPU observations inside the requested window")
    low, high = observations[0]["frame"], observations[-1]["frame"]
    frames = [row for row in trace.get("frames", []) if low < row["frame"] <= high]
    if not frames:
        raise ValueError("No retained sampled API frames in the selected interval")
    pairs = {row["id"]: row for row in trace["pairs"]}
    groups = {}
    kinds = defaultdict(lambda: {"samples": 0, "inclusive_ms": 0.0})
    fields = ("pair", "depth_control", "color_mask", "blend_mask", "width", "height", "queried", "discarded")
    for frame in frames:
        for group in frame["groups"]:
            key = tuple(group[field] for field in fields)
            row = groups.setdefault(key, {**dict(zip(fields, key)), "samples": 0,
                                         "available": 0, "inclusive_ms": 0.0, "maximum_ms": 0.0})
            for field in ("samples", "available", "inclusive_ms"):
                row[field] += group[field]
            row["maximum_ms"] = max(row["maximum_ms"], group["maximum_ms"])
    for row in groups.values():
        pair = pairs.get(row["pair"], {})
        row["vertex"] = pair.get("vertex")
        row["pixel"] = pair.get("pixel")
        row["inclusive_ms_per_sampled_frame"] = row["inclusive_ms"] / len(frames)
        row["mean_list_ms"] = row["inclusive_ms"] / row["available"] if row["available"] else None
        kind = ("discarded" if row["discarded"] else "queried" if row["queried"]
                else "blended" if row["blend_mask"] else "other_draw")
        kinds[kind]["samples"] += row["samples"]
        kinds[kind]["inclusive_ms"] += row["inclusive_ms"]
    for row in kinds.values():
        row["inclusive_ms_per_sampled_frame"] = row["inclusive_ms"] / len(frames)
    return {
        "semantics": trace["semantics"],
        "limitations": ["Timestamp markers perturb GPU scheduling; use an unprofiled control for performance.",
                        "Intervals include dependencies and may overlap; their sum is not GPU frame time.",
                        "Draw-list timing excludes separate transfer lists and host presentation.",
                        "Shader groups describe recorded state, not recovered Unreal Engine pass names."],
        "requested_elapsed_seconds": [anchor + start, anchor + end],
        "selected_api_frame_bounds": {"exclusive_begin": low, "inclusive_end": high},
        "sampled_frames": len(frames),
        "samples": sum(row["samples"] for row in frames),
        "available": sum(row["available"] for row in frames),
        "selected_group_overflows": sum(row["group_overflows"] for row in frames),
        "trace_incomplete": trace.get("incomplete", True),
        "trace_status": trace.get("status"),
        "trace_diagnostic_failures": trace.get("diagnostic_failures"),
        "strict_native_unmixed": result.get("strict_native_unmixed"),
        "clean_close": result.get("clean_ui_close"),
        "categories": dict(kinds),
        "groups": sorted(groups.values(), key=lambda row: row["inclusive_ms"], reverse=True),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--start-after-anchor", type=float, default=70.5)
    parser.add_argument("--end-after-anchor", type=float, default=99.5)
    args = parser.parse_args()
    if args.start_after_anchor >= args.end_after_anchor:
        parser.error("window end must follow its start")
    report = analyze(args.run, args.start_after_anchor, args.end_after_anchor)
    output = args.run / "gpu-draw-analysis.json"
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({**{k: v for k, v in report.items() if k != "groups"},
                      "top_groups": report["groups"][:12]}, indent=2))


if __name__ == "__main__":
    main()
