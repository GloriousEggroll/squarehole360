#!/usr/bin/env python3
"""CPU-only truth-table checks of the five actual source admission guards.

No renderer is built. Expressions are extracted from production and preserved
preimage sources, parsed as a restricted boolean AST, then tested together and
under independent prerequisite mutations. This checks admission, not GPU safety.
"""
import argparse
import ast
import itertools
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
SPECS = [
    ("phase", "src/renderer/native/scene_executor.cpp", "Opaque phase requires canonical asynchronous material composition and separate authority"),
    ("scene_composition", "src/renderer/native/scene_executor.cpp", "FSI material composition requires canonical asynchronous Native materials and depth/query paths"),
    ("queue", "src/renderer/native/scene_executor.cpp", "Queued retained draw exports require the asynchronous unified Native writer"),
    ("runtime_composition", "src/renderer/native/runtime_bridge.cpp", "FSI material composition requires canonical asynchronous Native rendering without other material variants"),
    ("replay_composition", "tools/rendering/native/live_capture_replay.cpp", "FSI material composition requires its bounded original asynchronous Native provider"),
]


def condition(source, message):
    # Preserved preimages use the old diagnostic wording.
    if '"' + message + '"' not in source and message.endswith("without other material variants"):
        message += " or packed color grouping"
    assert source.count('"' + message + '"') == 1
    end = source.index('"' + message + '"')
    start = list(re.finditer(r"\bif\s*\(", source[:end]))[-1].end()
    depth = 1
    for i in range(start, end):
        depth += (source[i] == "(") - (source[i] == ")")
        if not depth:
            return source[start:i]
    raise AssertionError("Unbalanced actual guard")


def parsed(expression):
    expression = expression.replace("RetainedOpaqueEnabled()", "(retained_opaque_writer||retained_opaque_catalog)")
    expression = expression.replace("StencilMaterialEnabled()", "stencil_material")
    expression = re.sub(r"REXCVAR_GET\(gow3_native_(\w+)\)", r"\1", expression)
    expression = re.sub(r"config_?\.", "", expression)
    expression = expression.replace("debug_pixels.empty()", "debug_pixels_empty")
    expression = expression.replace("fsi_hoist_mode!=NativeFsiHoistMode::Disabled", "fsi_hoist_nonzero")
    aliases = {"native_raster": "native_fsi", "deferred_queries": "defer_native_queries",
               "description.ordered_pixel_interlock": "ordered_pixel_interlock",
               "description.features.fragmentStoresAndAtomics": "fragment_stores"}
    for old, new in aliases.items():
        expression = re.sub(r"\b" + re.escape(old) + r"\b", new, expression)
    expression = re.sub(r"\b(\w+)_enabled\b", r"\1", expression)
    expression = expression.replace("&&", " and ").replace("||", " or ").replace("!", " not ")
    tree = ast.parse(" ".join(expression.split()), mode="eval")
    assert all(isinstance(x, (ast.Expression, ast.BoolOp, ast.And, ast.Or,
                             ast.UnaryOp, ast.Not, ast.Name, ast.Load)) for x in ast.walk(tree))
    names = {x.id for x in ast.walk(tree) if isinstance(x, ast.Name)}

    def run(values):
        return bool(eval(compile(tree, "<actual-source-admission>", "eval"), {"__builtins__": {}},
                         {name: values.get(name, False) for name in names}))
    return run, names


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    controls = ("retained_opaque_phase", "queue_retained_exports_for_draws",
                "retained_opaque_writer", "retained_opaque_catalog")
    defaults = {name: True for name in ("native_fsi", "unified_gpu_queue", "async_gpu_submissions",
                "batch_transfers", "defer_native_queries", "fsi_state_material_composition",
                "specialize_fsi_pipeline_state", "debug_pixels_empty", "ordered_pixel_interlock", "fragment_stores")}
    checks = 0
    results = []
    guards = {}
    for name, path, message in SPECS:
        before, old_names = parsed(condition((args.before / path).read_text(), message))
        after, new_names = parsed(condition((ROOT / path).read_text(), message))
        guards[name] = after
        base = dict(defaults)
        # Runtime's explicit specialization cvar is separate from effective
        # Scene specialization enabled by material composition itself.
        if name == "runtime_composition":
            base["specialize_fsi_pipeline_state"] = False
        changes = 0
        # This historical queue-admission test holds packed grouping OFF; the
        # new actual-guard test independently covers its newly supported mode.
        for bits in itertools.product((False, True), repeat=len(controls)):
            values = base | dict(zip(controls, bits))
            for mutant in [None] + sorted((old_names | new_names) - set(controls) - {"packed_color_render_pass_groups"}):
                v = dict(values)
                if mutant is not None:
                    v[mutant] = not v.get(mutant, False)
                was, now = before(v), after(v)
                assert not (not was and now), (name, "previously admitted state rejected", mutant, v)
                if name == "phase":
                    expected = before(v | {"queue_retained_exports_for_draws": False})
                elif name == "queue":
                    expected = before(v) and not (v["retained_opaque_phase"] and
                        all(v.get(k, False) for k in ("native_fsi", "unified_gpu_queue", "async_gpu_submissions", "batch_transfers")))
                else:
                    expected = before(v | {"queue_retained_exports_for_draws": False}) if v["retained_opaque_phase"] else was
                assert now == expected, (name, "broadened unrelated admission", mutant, v)
                changes += was != now
                checks += 2
        assert changes > 0, (name, "new admission not exercised")
        results.append({"guard": name, "positive_admission_changes": changes})
    phase = defaults | {"retained_opaque_phase": True, "queue_retained_exports_for_draws": True}
    for name, guard in guards.items():
        state = dict(phase)
        if name == "runtime_composition":
            state["specialize_fsi_pipeline_state"] = False
        assert not guard(state), (name, "phase+queue should be admitted")
        checks += 1
    # Old retained writer/catalog queue admission and no-writer rejection.
    for writer, catalog in itertools.product((False, True), repeat=2):
        values = defaults | {"queue_retained_exports_for_draws": True,
                             "retained_opaque_writer": writer, "retained_opaque_catalog": catalog}
        assert guards["queue"](values) == (not writer and not catalog)
        checks += 1
    result = {"passed": True, "checks": checks, "guards": results,
              "scope": "Actual source boolean admission; no renderer build, GPU execution or ownership proof"}
    if args.output:
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
