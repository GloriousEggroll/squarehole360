#!/usr/bin/env python3
"""Extract bounded Xenos shader containers from an unpacked game file.

UE3 serializes bytecode at unaligned offsets, so scanning only DWORD-aligned
positions (as Unleashed's archive scanner does) misses valid Gears shaders.
This does not decompress UE3 packages or claim whole-game shader coverage.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def containers(data):
    offset = 0
    while True:
        offset = data.find(b"\x10\x2a\x11", offset)
        if offset < 0:
            return
        start = offset
        offset += 1
        if start + 36 > len(data):
            continue
        flags, virtual, physical, unused, ctab, defs, shader, zero1, zero2 = struct.unpack_from(">9I", data, start)
        size = virtual + physical
        # Runtime vertex containers carry variant (0x20) and linked (0x40)
        # flags as well as the stage bit. Preserve the patched bytes verbatim.
        allowed = 0x61 if flags & 1 else 0
        if (flags & ~allowed) != 0x102A1100 or zero1 or zero2 or unused:
            continue
        if virtual < 36 or not physical or size > len(data) - start:
            continue
        # Metadata and instruction payload must fit their respective regions.
        if not 36 <= shader <= virtual - 32:
            continue
        if ctab and not 36 <= ctab <= virtual - 32:
            continue
        if defs and not 36 <= defs <= virtual - 20:
            continue
        physical_offset, code_size, _, _, _, interpolator_info = struct.unpack_from(">6I", data, start + shader)
        if not code_size or code_size % 12 or physical_offset + code_size > physical:
            continue
        interpolators = (interpolator_info >> 5) & 31
        stage = "vertex" if flags & 1 else "pixel"
        if stage == "vertex":
            extra, elements, _ = struct.unpack_from(">3I", data, start + shader + 24)
            if shader + 36 + 4 * (extra + elements + interpolators) > virtual:
                continue
        else:
            elements = 0
            if shader + 32 + 4 * interpolators > virtual:
                continue
        blob = data[start:start + size]
        yield blob, {
            "offset": start, "size": size, "stage": stage,
            "virtual_size": virtual, "physical_size": physical,
            "code_offset": virtual + physical_offset, "code_size": code_size,
            "constant_table": bool(ctab), "definitions": bool(defs),
            "vertex_elements": elements, "interpolators": interpolators,
        }
        offset = start + size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    data = args.input.read_bytes()
    args.output.mkdir(parents=True, exist_ok=True)
    shaders = {}
    for blob, info in containers(data):
        digest = hashlib.sha256(blob).hexdigest()
        if digest in shaders:
            shaders[digest]["offsets"].append(info["offset"])
            continue
        name = f"{digest}.{info['stage']}.bin"
        (args.output / name).write_bytes(blob)
        info["offsets"] = [info.pop("offset")]
        shaders[digest] = {"path": name, "sha256": digest, **info}
    manifest = {
        "source": str(args.input), "source_sha256": hashlib.sha256(data).hexdigest(),
        "status": "Extracted containers; translation and rendering not yet validated",
        "shaders": list(shaders.values()),
    }
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"unique_shaders": len(shaders), "vertex": sum(s["stage"] == "vertex" for s in shaders.values()),
                      "pixel": sum(s["stage"] == "pixel" for s in shaders.values()),
                      "without_constant_table": sum(not s["constant_table"] for s in shaders.values())}))
    if not shaders:
        raise SystemExit("No validated shader containers found")


if __name__ == "__main__":
    main()
