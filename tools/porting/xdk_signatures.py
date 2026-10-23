#!/usr/bin/env python3
"""XDK function signatures for porting the native renderer to another Xbox 360 game.

Works on images written by gow3-xex-image-dump (PREFIX.bin + PREFIX.json).

  extract REF_PREFIX PROFILE.json OUT.json
      For every named function address in PROFILE ({"functions":{name:"0x82..."}}),
      take its instruction words (length from .pdata, capped) and mask the bits that
      depend on where code and data were linked:
        - b/bl (opcode 18): target offset
        - addis (lis) immediates, and the 16-bit displacement of any D-form
          instruction whose base register was last set by addis (absolute data)
      Conditional branches inside the function keep their offsets.

  scan SIGNATURES.json TARGET_PREFIX [--min-similarity 0.85]
      Exact masked matches are reported first. A signature without exactly one
      exact match gets its best candidates by masked-word similarity, found from
      anchors taken across the whole pattern (robust to small XDK code changes).
"""
import argparse, bisect, difflib, json, struct, sys
from collections import defaultdict

MAX_WORDS = 96

def load(prefix):
    meta = json.load(open(prefix + ".json"))
    image = open(prefix + ".bin", "rb").read()
    return meta, image

def words_at(meta, image, address, count):
    offset = address - meta["base"]
    return list(struct.unpack(">%dI" % count, image[offset:offset + 4 * count]))

def pdata_lengths(meta, image):
    """PPC .pdata: 8-byte entries {begin address, packed flags}; bits 8..29 = length in words."""
    lengths = {}
    for section in meta["sections"]:
        if section["name"] != ".pdata": continue
        offset = section["address"] - meta["base"]
        for i in range(section["size"] // 8):
            begin, packed = struct.unpack(">II", image[offset + 8 * i: offset + 8 * i + 8])
            if begin: lengths[begin] = (packed >> 8) & 0x3FFFFF
    return lengths

def mask_words(words):
    masks, lis_regs = [], set()
    for w in words:
        op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        m = 0xFFFFFFFF
        if op == 18:                      # b / bl: relocated target
            m = 0xFC000003
        elif op == 15:                    # addis: high half of an absolute address
            m = 0xFFFF0000
            lis_regs.add(rd)
        elif op in (14, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
                    48, 49, 50, 51, 52, 53, 54, 55, 58, 62) and ra in lis_regs:
            m = 0xFFFF0000                # low half / displacement off an absolute base
            if op in (14, 24, 25) and rd != ra: lis_regs.discard(rd)
        else:
            # Any other write to a tracked register ends its absolute-address meaning.
            if op in (31, 21, 20, 23, 30) or op in (32, 33, 34, 35, 40, 41, 42, 43, 58):
                lis_regs.discard(rd if op != 31 else rd)
        if op in (32, 34, 40, 42, 58) and rd in lis_regs and ra not in lis_regs:
            lis_regs.discard(rd)
        masks.append(m)
    return masks

D_FORM = {32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu", 38: "stb", 39: "stbu",
          40: "lhz", 41: "lhzu", 42: "lha", 44: "sth", 46: "lmw", 47: "stmw", 48: "lfs", 50: "lfd",
          52: "stfs", 54: "stfd", 14: "addi", 15: "addis", 24: "ori", 25: "oris", 28: "andi.",
          10: "cmplwi", 11: "cmpwi", 7: "mulli"}

def disassemble(w):
    """Enough PPC to read a diff: D/DS-form fields, branches, rotates; otherwise raw."""
    op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    if op in D_FORM:
        name = D_FORM[op]
        if op in (10, 11): return "%s cr%d,r%d,0x%x" % (name, rd >> 2, ra, imm)
        if op in (14, 15, 7): return "%s r%d,r%d,%d" % (name, rd, ra, simm)
        if op in (24, 25, 28): return "%s r%d,r%d,0x%x" % (name, ra, rd, imm)
        return "%s r%d,0x%x(r%d)" % (name, rd, imm, ra) if simm >= 0 else "%s r%d,-0x%x(r%d)" % (name, rd, -simm, ra)
    if op == 58: return "%s r%d,0x%x(r%d)" % (("ld", "ldu", "lwa", "?")[w & 3], rd, imm & ~3, ra)
    if op == 62: return "%s r%d,0x%x(r%d)" % (("std", "stdu", "?", "?")[w & 3], rd, imm & ~3, ra)
    if op == 18: return "b%s" % ("l" if w & 1 else "")
    if op == 16: return "bc %d,%d,%+d" % (rd, ra, (imm & ~3) - (0x10000 if imm & 0x8000 else 0))
    if op == 21: return "rlwinm r%d,r%d,%d,%d,%d" % (ra, rd, (w >> 11) & 31, (w >> 6) & 31, (w >> 1) & 31)
    return "op%d %08X" % (op, w)

def extract(args):
    meta, image = load(args.reference)
    lengths = pdata_lengths(meta, image)
    profile = json.load(open(args.profile))
    out = {"reference": args.reference, "signatures": []}
    for name, value in sorted(profile["functions"].items(), key=lambda x: int(x[1], 16)):
        address = int(value, 16)
        count = min(lengths.get(address, 32) or 32, MAX_WORDS)
        words = words_at(meta, image, address, count)
        masks = mask_words(words)
        out["signatures"].append({"name": name, "reference_address": "0x%08X" % address,
                                  "length_words": lengths.get(address),
                                  "pdata_start": address in lengths,
                                  "pattern": ["%08X" % w for w in words],
                                  "mask": ["%08X" % m for m in masks]})
    starts = sorted(lengths)
    out["sites"] = []
    for name, value in sorted(profile.get("sites", {}).items(), key=lambda x: int(x[1], 16)):
        address = int(value, 16)
        k = bisect.bisect_right(starts, address) - 1
        function = starts[k] if k >= 0 else None
        if function is None or address >= function + 4 * (lengths[function] or 0):
            print("site %s 0x%08X is not inside a .pdata function" % (name, address)); continue
        offset = address - function
        if offset // 4 >= MAX_WORDS:
            # Sites deep in large functions get their own containing-function signature.
            pass
        out["sites"].append({"name": name, "reference_address": "0x%08X" % address,
                             "function": "0x%08X" % function, "offset": offset,
                             "word": "%08X" % words_at(meta, image, address, 1)[0],
                             "function_pattern": ["%08X" % w for w in words_at(meta, image, function, min(lengths[function], MAX_WORDS))],
                             "function_mask": ["%08X" % m for m in mask_words(words_at(meta, image, function, min(lengths[function], MAX_WORDS)))],
                             "context": ["%08X" % w for w in words_at(meta, image, max(function, address - 32), (address - max(function, address - 32)) // 4 + 8)]})
    json.dump(out, open(args.out, "w"), indent=1)
    print("extracted %d signatures (%d with .pdata length), %d sites" %
          (len(out["signatures"]), sum(1 for s in out["signatures"] if s["length_words"]), len(out["sites"])))

def masked(words, mask):
    return [w & m for w, m in zip(words, mask)]

def function_words(meta, image, lengths, start, fallback):
    count = min(lengths.get(start) or fallback, MAX_WORDS)
    return words_at(meta, image, start, count)

def aligned_similarity(pattern, mask, words):
    """Alignment-tolerant similarity of masked word sequences (insertions allowed)."""
    a, b = masked(pattern, mask), masked(words, mask[:len(words)] + [0xFFFFFFFF] * (len(words) - len(mask)))
    return difflib.SequenceMatcher(None, a, b, autojunk=False).ratio()

def displacement_pairs(pattern, words):
    """Same-shaped D/DS-form instructions whose only difference is the displacement."""
    pairs = []
    matcher = difflib.SequenceMatcher(None, pattern, words, autojunk=False)
    for tag, i1, i2, j1, j2 in matcher.get_opcodes():
        if tag != "replace" or i2 - i1 != j2 - j1: continue
        for p, w in zip(pattern[i1:i2], words[j1:j2]):
            if (p >> 16) == (w >> 16) and (p >> 26) in D_FORM and (p >> 26) not in (10, 11, 14, 15, 24, 25, 28, 7):
                pairs.append((p & 0xFFFF, w & 0xFFFF))
            elif (p >> 16) == (w >> 16) and (p >> 26) in (58, 62):
                pairs.append((p & 0xFFFC, w & 0xFFFC))
    return pairs

class Matcher:
    """Locates reference functions in a target image (exact, then aligned similarity)."""

    def __init__(self, meta, image):
        self.meta, self.image = meta, image
        self.code = [s for s in meta["sections"] if s["executable"]]
        self.lengths = pdata_lengths(meta, image)
        self.starts = sorted(self.lengths)
        self.pdata_flags = {}

    def candidates(self, pattern, mask, pdata_start=True):
        n = len(pattern)
        anchors = [i for i in range(0, n - 2) if mask[i] == mask[i + 1] == mask[i + 2] == 0xFFFFFFFF]
        anchors = anchors[::max(1, len(anchors) // 12)][:12]
        found = set()
        for i in anchors:
            needle = struct.pack(">3I", *pattern[i:i + 3])
            for section in self.code:
                lo = section["address"] - self.meta["base"]; hi = lo + section["size"]
                pos = self.image.find(needle, lo, hi)
                while pos != -1:
                    if pos % 4 == 0 and not pdata_start:
                        # No .pdata entry (CRT helpers, shared tails): use the anchor offset.
                        found.add(self.meta["base"] + pos - 4 * i)
                    elif pos % 4 == 0:
                        k = bisect.bisect_right(self.starts, self.meta["base"] + pos) - 1
                        if k >= 0: found.add(self.starts[k])
                    pos = self.image.find(needle, pos + 4, hi)
        return found

    def exact(self, pattern, mask, candidates):
        want = masked(pattern, mask)
        return sorted(a for a in candidates
                      if masked(words_at(self.meta, self.image, a, len(pattern)), mask) == want)

    def best(self, pattern, mask, window):
        scored = []
        for a in window:
            words = function_words(self.meta, self.image, self.lengths, a, len(pattern))
            scored.append((aligned_similarity(pattern, mask, words), a))
        scored.sort(reverse=True)
        return scored

    def resolve(self, items, min_similarity):
        """items: list of (name, reference_address, pattern, mask). Uses the XDK
        link order to bound the search for functions without an exact match."""
        first = {}
        for name, ref, pattern, mask in items:
            candidates = self.candidates(pattern, mask, self.pdata_flags.get(name, True))
            first[name] = (candidates, self.exact(pattern, mask, candidates))
        ordered = sorted(items, key=lambda x: x[1])
        resolved = {name: first[name][1][0] for name, _, _, _ in ordered if len(first[name][1]) == 1}
        out = {}
        for index, (name, ref, pattern, mask) in enumerate(ordered):
            candidates, exact = first[name]
            if len(exact) == 1:
                out[name] = ("unique", exact[0], 1.0, 1); continue
            if exact:
                out[name] = ("ambiguous", exact[0], 1.0, len(exact)); continue
            lo = max([resolved[o[0]] for o in ordered[:index] if o[0] in resolved], default=None)
            hi = min([resolved[o[0]] for o in ordered[index + 1:] if o[0] in resolved], default=None)
            window = set(candidates)
            if lo is not None and hi is not None and hi > lo:
                inside = self.starts[bisect.bisect_right(self.starts, lo):bisect.bisect_left(self.starts, hi)]
                if len(inside) <= 400: window |= set(inside)
            scored = self.best(pattern, mask, window)
            if not scored:
                out[name] = ("missing", None, 0.0, 0); continue
            score, address = scored[0]
            out[name] = ("similar" if score >= min_similarity else "missing", address, score, 0)
        return out


def scan(args):
    data = json.load(open(args.signatures))
    meta, image = load(args.target)
    matcher = Matcher(meta, image)
    matcher.pdata_flags = {sig["name"]: sig.get("pdata_start", True) for sig in data["signatures"]}
    items = [(sig["name"], int(sig["reference_address"], 16),
              [int(x, 16) for x in sig["pattern"]], [int(x, 16) for x in sig["mask"]])
             for sig in data["signatures"]]
    # Each site's containing function is matched too, even if not profiled.
    known = {ref for _, ref, _, _ in items}
    for site in data.get("sites", []):
        function = int(site["function"], 16)
        if function not in known:
            known.add(function)
            items.append(("(container) 0x%08X" % function, function,
                          [int(x, 16) for x in site["function_pattern"]],
                          [int(x, 16) for x in site["function_mask"]]))
    matches = matcher.resolve(items, args.min_similarity)
    by_reference = {ref: (name, matches[name]) for name, ref, _, _ in items}
    patterns = {name: (pattern, mask) for name, _, pattern, mask in items}
    offsets = defaultdict(lambda: defaultdict(int))
    counts = defaultdict(int)
    report = {}
    for name, ref, pattern, mask in sorted(items, key=lambda x: x[1]):
        if name.startswith("(container)"): continue
        status, found, score, exact_count = matches[name]
        diff = []
        if found is not None and status != "unique":
            words = function_words(meta, image, matcher.lengths, found, len(pattern))
            for ref_disp, target_disp in displacement_pairs(pattern, words):
                offsets[ref_disp][target_disp] += 1
            m = difflib.SequenceMatcher(None, masked(pattern, mask), masked(words, [0xFFFFFFFF] * len(words)), autojunk=False)
            for tag, i1, i2, j1, j2 in m.get_opcodes():
                if tag == "equal": continue
                for k in range(max(i2 - i1, j2 - j1)):
                    left = disassemble(pattern[i1 + k]) if i1 + k < i2 else ""
                    right = disassemble(words[j1 + k]) if j1 + k < j2 else ""
                    diff.append("%-7s ref %-30s target %s" % (tag, left, right))
        counts[status] += 1
        report[name] = {"status": status, "reference": "0x%08X" % ref,
                        "target": "0x%08X" % found if found is not None else None,
                        "similarity": round(score, 3), "exact_matches": exact_count, "diff": diff}
        print("%-9s %-40s ref=0x%08X -> %s (similarity %.3f)" %
              (status, name, ref, report[name]["target"] or "-", score))
        if args.diff:
            for line in diff[:args.diff]: print("            " + line)
    print("summary:", dict(counts))
    site_report, site_counts = {}, defaultdict(int)
    for site in data.get("sites", []):
        function = int(site["function"], 16)
        container_name, (cstatus, ctarget, _, _) = by_reference[function]
        context = [int(x, 16) for x in site["context"]]
        reference = int(site["reference_address"], 16)
        lead = (reference - max(function, reference - 32)) // 4
        context_mask = mask_words(context)
        best = None
        if ctarget is not None:
            length = matcher.lengths.get(ctarget) or MAX_WORDS
            for i in range(length):
                at = ctarget + 4 * i
                begin = at - 4 * lead
                if begin < ctarget: continue
                words = words_at(meta, image, begin, len(context))
                same = sum(1 for w, p, m in zip(words, context, context_mask) if (w & m) == (p & m)) / len(context)
                witness = words[lead] == int(site["word"], 16)
                score = same + (0.5 if witness else 0)
                if best is None or score > best[0]: best = (score, at, same, witness)
        if best is None:
            status, found = "missing", None
        else:
            status = "exact" if best[2] == 1.0 and best[3] else ("witness" if best[3] and best[2] >= 0.75 else "uncertain")
            found = "0x%08X" % best[1]
        site_counts[status] += 1
        site_report[site["name"]] = {"status": status, "reference": site["reference_address"], "target": found,
                                     "container": cstatus, "context_similarity": round(best[2], 3) if best else 0,
                                     "reference_word": site["word"]}
        print("site %-10s %-34s ref=%s -> %s (container %s)" %
              (status, site["name"], site["reference_address"], found or "-", cstatus))
    if site_report: print("site summary:", dict(site_counts))
    shifts = []
    for ref_disp in sorted(offsets):
        target_disp, votes = max(offsets[ref_disp].items(), key=lambda x: x[1])
        shifts.append({"reference": "0x%X" % ref_disp, "target": "0x%X" % target_disp,
                       "delta": target_disp - ref_disp, "votes": votes})
    if shifts:
        print("displacement changes seen in inexact matches (reference -> target, delta):")
        for item in shifts:
            print("  %(reference)8s -> %(target)8s  %(delta)+6d  (%(votes)d)" % item)
    if args.out:
        json.dump({"functions": report, "sites": site_report, "displacements": shifts}, open(args.out, "w"), indent=1)

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
sub = parser.add_subparsers(dest="command", required=True)
e = sub.add_parser("extract"); e.add_argument("reference"); e.add_argument("profile"); e.add_argument("out")
s = sub.add_parser("scan"); s.add_argument("signatures"); s.add_argument("target")
s.add_argument("--min-similarity", type=float, default=0.85); s.add_argument("--out")
s.add_argument("--diff", type=int, default=0, help="print up to N differing words per inexact match")
args = parser.parse_args()
extract(args) if args.command == "extract" else scan(args)
