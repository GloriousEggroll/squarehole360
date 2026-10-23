#!/usr/bin/env python3
"""Fail closed if the exact GetData/register-helper source extraction drifts."""
import argparse
import hashlib
import json
from pathlib import Path

EXPECTED = {
    "sub_82AFA708": "7995aa4351021f69021773ea1df583d33fe0ebf8cde711ff44b271ce9c1fef2a",
    "__savegprlr_26": "5b956530571ddb4032489cf9734ed77562d8d8b31f460384ce584c2d50cdd15d",
    "__restgprlr_26": "8516e64a83e7b1e9672ecebb42d071dca3133d0d0170ee2d0efe55779213581d",
}

def body(text, signature):
    marker = signature + " {\n"
    if text.count(marker) != 1:
        raise RuntimeError("Pinned function signature is missing or duplicated: " + signature)
    start = text.index(marker) + len(marker)
    end = text.index("\n}\n", start)
    return text[start:end]

def validate(root, candidate):
    paths = {
        "sub_82AFA708": root / "generated/default/gow3_recomp.71.cpp",
        "__savegprlr_26": root / "generated/default/gow3_recomp.85.cpp",
        "__restgprlr_26": root / "generated/default/gow3_recomp.85.cpp",
    }
    extracted = {
        "sub_82AFA708": ("getdata_leaf_body.inc", "REX_FUNC(GOW3_GETDATA_LEAF_NAME)"),
        "__savegprlr_26": ("getdata_register_helpers.inc", "GOW3_GETDATA_HELPER_LINKAGE REX_FUNC(GOW3_GETDATA_SAVE_NAME)"),
        "__restgprlr_26": ("getdata_register_helpers.inc", "GOW3_GETDATA_HELPER_LINKAGE REX_FUNC(GOW3_GETDATA_RESTORE_NAME)"),
    }
    result = {}
    for name, path in paths.items():
        original = body(path.read_text(), "DEFINE_REX_FUNC(" + name + ")")
        digest = hashlib.sha256(original.encode()).hexdigest()
        file, signature = extracted[name]
        if digest != EXPECTED[name] or original != body((candidate / "generated/native-getdata" / file).read_text(), signature):
            raise RuntimeError("Exact GetData/helper source authentication failed: " + name)
        result[name] = digest
    # Public strong kick/memzero calls must stay reachable through the ordinary
    # generated declarations, rather than becoming local candidate substitutes.
    leaf = body((candidate / "generated/native-getdata/getdata_leaf_body.inc").read_text(), "REX_FUNC(GOW3_GETDATA_LEAF_NAME)")
    if leaf.count("sub_82AF58B8(ctx, base);") != 2 or leaf.count("sub_82CC9EA0(ctx, base);") != 1:
        raise RuntimeError("Pinned original external-call shape changed")
    # The optional native consumer has one explicit scheduling change; its
    # actual report decode, helper calls and every other instruction stay pinned.
    hook = "\t// Native query result may precede the unrelated global submission marker.\n\tif (ctx.cr6.lt && Gow3NativeQueryResultAvailable(ctx.r6.u32, ctx.r3.u32, ctx.r11.u32, uint8_t(ctx.r30.u32), base)) goto loc_82AFA78C;\n"
    gate = "\t// bge cr6,0x82afa78c\n\tif (!ctx.cr6.lt) goto loc_82AFA78C;"
    published = body((candidate / "generated/native-getdata/getdata_published_body.inc").read_text(), "REX_FUNC(GOW3_GETDATA_LEAF_NAME)")
    if leaf.count(gate) != 1 or published != leaf.replace(gate, hook + gate):
        raise RuntimeError("Native published-query body differs beyond its single availability gate")
    result["published_query_body"] = hashlib.sha256(published.encode()).hexdigest()
    return result

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--candidate-root", type=Path)
    parser.add_argument("--stamp", type=Path, required=True)
    args = parser.parse_args()
    results = validate(args.root, args.candidate_root or args.root)
    args.stamp.parent.mkdir(parents=True, exist_ok=True)
    args.stamp.write_text(json.dumps({"source_authentication": results, "scope": "Exact source only; no production codegen, GPU or speed proof"}, indent=2) + "\n")
