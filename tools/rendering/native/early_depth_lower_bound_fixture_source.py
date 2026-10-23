#!/usr/bin/env python3
"""Assemble controlled baseline only; no SPIR-V mutation or production admission."""
import argparse
import hashlib
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument("helper",type=Path)
p.add_argument("skeleton",type=Path)
p.add_argument("output",type=Path)
a=p.parse_args()
h=a.helper.read_bytes()
s=a.skeleton.read_bytes()
marker=b"// GOW3_INSERT_FSI_HELPER"
if len(h)>32768 or len(s)>8192 or s.count(marker)!=1:
    raise SystemExit("Refused bounded fixture source shape")
if hashlib.sha256(h).hexdigest()!="93f900f27cb3bf0a17e23c25dd3024e41f38f864ec1ff07bfa823599e7e4102c":
    raise SystemExit("Refused unreviewed canonical FSI helper")
r=s.replace(marker,h)
if r.replace(h,marker,1)!=s:
    raise SystemExit("Fixture full-helper restoration failed")
a.output.write_bytes(r)
print("Controlled baseline helper/source exact; candidate must come from separately audited fixture-only early-mode certificate")
