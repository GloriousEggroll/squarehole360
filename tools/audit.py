#!/usr/bin/env python3
"""Record mechanically checkable progress; this does not prove game correctness."""
import argparse
from collections import Counter
import csv
import hashlib
import json
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parent.parent
EXPECTED_XEX = "4e5d00b8f584f42951344b26a2ae8292bb30c7634b1a75cec0c70a80b3f8f790"


def audit():
    binary = (ROOT / "game/default.xex").read_bytes()
    digest = hashlib.sha256(binary).hexdigest()
    if digest != EXPECTED_XEX:
        raise ValueError("default.xex differs from the executable these function hints target")
    u32 = lambda offset: struct.unpack_from(">I", binary, offset)[0]
    headers = {u32(24 + i * 8): u32(28 + i * 8) for i in range(u32(20))}
    execution = headers[0x40006]
    result = {
        "xex_sha256": digest,
        "entry_point": hex(headers[0x10100]),
        "image_base": hex(headers[0x10201]),
        "title_id": hex(u32(execution + 12)),
        "full_source_decompilation_complete": False,
        "gameplay_validated": False,
    }
    counts = Counter()
    patterns = {
        "function_definitions": r"^DEFINE_REX_FUNC\(",
        "unimplemented_instruction_sites": r"REX_UNIMPLEMENTED\(",
        "unresolved_branches": r'REX_FATAL\("Unresolved branch',
        "unresolved_calls": r'REX_FATAL\("Unresolved call',
        "switch_default_traps": r"__builtin_trap\(\); // Switch case out of range",
    }
    for path in sorted((ROOT / "generated/default").glob("gow3_recomp.*.cpp")):
        source = path.read_text()
        counts["translation_units"] += 1
        counts["source_bytes"] += path.stat().st_size
        for key, pattern in patterns.items():
            counts[key] += len(re.findall(pattern, source, re.MULTILINE))
    result["static_translation"] = dict(counts)
    ledger = ROOT / "analysis/decompiled/functions.tsv"
    if ledger.exists():
        with ledger.open() as stream:
            result["ghidra_export"] = dict(Counter(
                row["status"] for row in csv.DictReader(stream, delimiter="\t")
            ))
    executable = ROOT / "build/bin/gow3"
    if executable.exists():
        with executable.open("rb") as stream:
            header = stream.read(20)
        result["host_executable"] = {
            "path": "build/bin/gow3",
            "elf64_little_endian_x86_64": (
                header[:6] == b"\x7fELF\x02\x01" and header[18:20] == b"\x3e\x00"
            ),
            "bytes": executable.stat().st_size,
        }
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-clean-codegen", action="store_true")
    args = parser.parse_args()
    result = audit()
    output = json.dumps(result, indent=2) + "\n"
    (ROOT / "reports").mkdir(exist_ok=True)
    (ROOT / "reports/progress.json").write_text(output)
    print(output, end="")
    counts = result["static_translation"]
    if args.require_clean_codegen and (
        not counts.get("translation_units") or any(counts.get(key, 0) for key in (
            "unimplemented_instruction_sites", "unresolved_branches", "unresolved_calls"
        ))
    ):
        raise SystemExit("Generated C++ is missing or contains known translation failures")
