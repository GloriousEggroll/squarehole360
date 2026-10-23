#!/usr/bin/env python3
"""Verify an intentional native-to-reference query transition in a boot report."""
import argparse
import json
from pathlib import Path
import re


def verify(directory, require_reference_samples=False, allow_zero_native=False):
    status = json.loads((directory / "result.json").read_text())
    log = (directory / "game.log").read_text(errors="replace")
    begins = re.findall(
        r"Native query handback begins residual reference query: generation (\d+), "
        r"report 0x([0-9A-Fa-f]+), native subtotal (\d+)", log)
    finishes = re.findall(
        r"Native query residual published: generation (\d+), report 0x([0-9A-Fa-f]+), "
        r"native subtotal (\d+), reference raw (\d+), reference normalized (\d+), "
        r"total (\d+), segments (\d+), guest ZPass (\d+), sentinel (true|false)", log)
    errors = []
    def require(condition, message):
        if not condition:
            errors.append(message)
    require(status.get("native_started"), "Native rendering never started")
    require(status.get("native_raster_requested"), "Test did not request native raster authority")
    require(status.get("fallback"), "No renderer handback occurred")
    require("Explicit diagnostic" in log, "Handback was not deliberately triggered")
    require(not status.get("fatal_native_transaction"), "A native transaction failed fatally")
    require(status.get("clean_ui_close"), "The game did not close cleanly through its UI")
    require(status.get("final_native_counters", {}).get("reference_retired"),
            "Reference continuation was not retained until shutdown")
    require(len(begins) == 1, "Expected exactly one active-query handback")
    require(len(finishes) == 1, "Expected exactly one residual query publication")
    details = None
    if len(begins) == 1 and len(finishes) == 1:
        generation, address, native = int(begins[0][0]), int(begins[0][1], 16), int(begins[0][2])
        fields = finishes[0]
        values = [int(fields[0]), int(fields[1], 16)] + [int(x) for x in fields[2:8]]
        final_generation, final_address, final_native, raw, normalized, total, segments, guest = values
        require((generation, address, native) == (final_generation, final_address, final_native),
                "Query generation, address or native subtotal changed during handback")
        if not allow_zero_native:
            require(native > 0, "This fixture did not prove a nonempty native sample subtotal")
        require(total == min(native + normalized, (1 << 64) - 1),
                "Combined query samples do not equal the two renderer contributions")
        require(guest == min(total, (1 << 32) - 1),
                "Guest query report differs from the completed combined count")
        require(fields[8] == "false", "Guest query completion sentinel was not cleared")
        require(segments > 0, "No residual GPU query segment retired")
        if require_reference_samples:
            require(raw > 0 and normalized > 0, "Residual reference draws produced no samples")
        details = dict(generation=generation, end_address=f"0x{address:08x}",
                       native_subtotal=native, reference_raw=raw, reference_normalized=normalized,
                       total=total, guest_samples=guest, reference_segments=segments,
                       nonempty_reference_contribution=normalized > 0)
    return dict(passed=not errors, errors=errors, query=details,
                scope="One-way active-query handback, residual GPU retirement and actual guest report publication; not full campaign coverage")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("--require-reference-samples", action="store_true")
    parser.add_argument("--allow-zero-native", action="store_true")
    args = parser.parse_args()
    result = verify(args.report, args.require_reference_samples, args.allow_zero_native)
    (args.report / "query-handback-check.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    raise SystemExit(not result["passed"])


if __name__ == "__main__":
    main()
