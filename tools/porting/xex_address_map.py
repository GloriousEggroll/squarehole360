#!/usr/bin/env python3
"""Map guest addresses between two builds of the same title (e.g. disc and a
title update), from loaded images written by gow3-xex-image-dump.

Functions are paired through .pdata: each function is reduced to its
instruction words with branch targets and 16-bit immediates masked, and the
two ordered function lists are aligned. A code address maps through its
function (aligning instructions when the function changed). A data address
maps through the lis/low-half references of paired code: for each referenced
base address the target build's address is voted, and an address between
references takes the delta of the nearest one in the same section.

  xex_address_map.py OLD_PREFIX NEW_PREFIX ADDRESS...   print old -> new
  xex_address_map.py OLD_PREFIX NEW_PREFIX --file F      addresses from F
"""
import argparse
import bisect
import difflib
import hashlib
import json
import re
import sys
from collections import Counter, defaultdict

import numpy as np


class Image:
    def __init__(self, prefix):
        meta = json.load(open(prefix + ".json"))
        self.base = meta["base"]
        self.words = np.frombuffer(open(prefix + ".bin", "rb").read(), dtype=">u4").astype(np.uint32)
        self.sections = meta["sections"]
        self.functions = self._pdata()
        self.starts = [f[0] for f in self.functions]
        self.masked = mask(self.words)

    def section(self, address):
        for s in self.sections:
            if s["address"] <= address < s["address"] + s["size"]:
                return s
        return None

    def word_index(self, address):
        return (address - self.base) >> 2

    def _pdata(self):
        pdata = next(s for s in self.sections if s["name"] == ".pdata")
        first = self.word_index(pdata["address"])
        entries = self.words[first:first + pdata["size"] // 4].reshape(-1, 2)
        functions = []
        for begin, data in entries:
            size = int((data >> 8) & 0x3FFFFF) * 4
            if begin and size:
                functions.append((int(begin), size))
        functions.sort()
        # Code .pdata doesn't describe (functions without unwind data, or
        # their tails) becomes pseudo-functions spanning each gap.
        gaps = []
        for s in self.sections:
            if not s["executable"]:
                continue
            end_of_section = s["address"] + s["size"]
            at = s["address"]
            for start, size in functions:
                if start + size <= s["address"] or start >= end_of_section:
                    continue
                if start > at:
                    gaps.append((at, start - at))
                at = max(at, start + size)
            if at < end_of_section:
                gaps.append((at, (end_of_section - at) & ~3))
        return sorted(functions + [g for g in gaps if g[1] >= 4])

    def function_at(self, address):
        i = bisect.bisect_right(self.starts, address) - 1
        if i >= 0 and address < self.functions[i][0] + self.functions[i][1]:
            return i
        return None

    def body(self, i, masked=True):
        start, size = self.functions[i]
        a = self.word_index(start)
        return (self.masked if masked else self.words)[a:a + size // 4]


def mask(words):
    op = words >> 26
    out = words.copy()
    out[op == 18] &= 0xFC000003  # b/bl: target
    out[op == 16] &= 0xFFFF0003  # bc: target
    # addi, addis, ori, and D/DS-form loads and stores: immediates
    imm = (op == 14) | (op == 15) | (op == 24) | ((op >= 32) & (op <= 55)) | (op == 58) | (op == 62)
    out[imm] &= 0xFFFF0000
    return out


def digest(body):
    return hashlib.blake2b(body.tobytes(), digest_size=12).digest()


class AddressMap:
    def __init__(self, old, new):
        self.old, self.new = old, new
        a = [digest(old.body(i)) for i in range(len(old.functions))]
        b = [digest(new.body(i)) for i in range(len(new.functions))]
        self.pairs = {}      # old function index -> new function index
        self.identical = set()
        matcher = difflib.SequenceMatcher(None, a, b, autojunk=False)
        for tag, i1, i2, j1, j2 in matcher.get_opcodes():
            if tag == "equal":
                for k in range(i2 - i1):
                    self.pairs[i1 + k] = j1 + k
                    self.identical.add(i1 + k)
            elif tag == "replace":
                # Changed functions between paired neighbours: pair those
                # whose masked bodies are most alike, in order.
                self._pair_changed(range(i1, i2), range(j1, j2))
        self._data_votes()

    def _pair_changed(self, olds, news):
        news = list(news)
        at = 0
        for i in olds:
            best, best_ratio = None, 0.6
            for k in range(at, min(len(news), at + 8)):
                j = news[k]
                s1, s2 = self.old.body(i), self.new.body(j)
                if max(len(s1), len(s2)) > 4 * min(len(s1), len(s2)) + 8:
                    continue
                ratio = difflib.SequenceMatcher(None, s1.tolist(), s2.tolist(), autojunk=False).ratio()
                if ratio > best_ratio:
                    best, best_ratio = k, ratio
            if best is not None:
                self.pairs[i] = news[best]
                at = best + 1

    def _references(self, image, i):
        """(word offset, referenced address) for lis rX,hi then a low-half use of rX."""
        words = image.body(i, masked=False)
        hi = {}
        out = []
        for k, w in enumerate(words.tolist()):
            op = w >> 26
            rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 15 and ra == 0:
                hi[rd] = imm << 16
                continue
            if ra in hi and ra != 0 and (op in (14, 24) or 32 <= op <= 55 or op in (58, 62)):
                low = imm & ~3 if op in (58, 62) else imm
                address = (hi[ra] + (low if op == 24 else (low - 0x10000 if low & 0x8000 else low))) & 0xFFFFFFFF
                out.append((k, address))
            if op == 15:
                hi.pop(rd, None)
            elif op in (14, 24) or 32 <= op <= 55 and op not in (36, 37, 38, 39, 44, 45, 52, 53, 54, 55):
                hi.pop(rd, None)
        return out

    def _data_votes(self):
        votes = defaultdict(Counter)
        for i in self.identical:
            j = self.pairs[i]
            r1, r2 = self._references(self.old, i), self._references(self.new, j)
            if len(r1) != len(r2):
                continue
            for (k1, a1), (k2, a2) in zip(r1, r2):
                if k1 == k2:
                    votes[a1][a2] += 1
        self.data = {}
        for a1, counter in votes.items():
            (a2, n), = counter.most_common(1)
            self.data[a1] = a2
        self.data_keys = sorted(self.data)

    def code(self, address):
        i = self.old.function_at(address)
        if i is None or i not in self.pairs:
            return None, "no paired function"
        j = self.pairs[i]
        offset = address - self.old.functions[i][0]
        if i in self.identical:
            return self.new.functions[j][0] + offset, "identical function"
        s1, s2 = self.old.body(i).tolist(), self.new.body(j).tolist()
        k = offset // 4
        for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, s1, s2, autojunk=False).get_opcodes():
            if i1 <= k < i2:
                if tag == "equal":
                    return self.new.functions[j][0] + 4 * (j1 + k - i1) + (offset & 3), "changed function, aligned"
                return None, "changed function, instruction %s" % tag
        return None, "outside function"

    def datum(self, address):
        if address in self.data:
            return self.data[address], "referenced"
        section = self.old.section(address)
        i = bisect.bisect_left(self.data_keys, address)
        best = None
        for k in (i - 1, i):
            if 0 <= k < len(self.data_keys):
                a = self.data_keys[k]
                if self.old.section(a) is section and (best is None or abs(a - address) < abs(best - address)):
                    best = a
        if best is None:
            return None, "no reference in section"
        return address + self.data[best] - best, "nearest reference %08X (%+d)" % (best, address - best)

    def map(self, address):
        section = self.old.section(address)
        if section is None:
            return None, "outside image"
        if section["executable"]:
            return self.code(address)
        return self.datum(address)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("old")
    parser.add_argument("new")
    parser.add_argument("addresses", nargs="*")
    parser.add_argument("--file")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    addresses = [int(a, 16) for a in args.addresses]
    if args.file:
        addresses += [int(a, 16) for a in re.findall(r"0x(8[23][0-9A-Fa-f]{6})\b", open(args.file).read())]
    old, new = Image(args.old), Image(args.new)
    amap = AddressMap(old, new)
    print("functions %d -> %d, paired %d (identical %d), data references %d" % (
        len(old.functions), len(new.functions), len(amap.pairs), len(amap.identical), len(amap.data)),
        file=sys.stderr)
    result = {}
    for a in dict.fromkeys(addresses):
        mapped, how = amap.map(a)
        result["%08X" % a] = None if mapped is None else "%08X" % mapped
        if not args.json:
            print("%08X -> %s  %s" % (a, "--------" if mapped is None else "%08X" % mapped, how))
    if args.json:
        json.dump(result, sys.stdout, indent=1)


if __name__ == "__main__":
    main()
