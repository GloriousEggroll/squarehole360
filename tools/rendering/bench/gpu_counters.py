#!/usr/bin/env python3
"""Summarize amdgpu_top JSON samples (one JSON document per line) taken while a
benchmark scene is held: GRBM/GRBM2 block busy %, fdinfo GFX %, clocks, power."""
import json,sys,statistics as st
rows=[]
for line in open(sys.argv[1]):
    line=line.strip()
    if not line.startswith('{'):continue
    try:rows.append(json.loads(line)['devices'][0])
    except Exception:pass
if not rows:sys.exit("no samples")
def med(get):
    v=[x for x in (get(r) for r in rows) if x is not None]
    return round(st.median(v),1) if v else None
print(f"samples={len(rows)}")
for group in ('GRBM','GRBM2'):
    for key in rows[0][group]:
        print(f"{group:5s} {key:40s} {med(lambda r:r[group][key]['value'])}")
print("gpu_activity GFX", med(lambda r:r['gpu_activity']['GFX']['value']), "Memory", med(lambda r:r['gpu_activity']['Memory']['value']))
for key in ('Average Power','SCLK','MCLK','Edge Temperature','Junction Temperature'):
    print("sensor",key,med(lambda r:(r['Sensors'].get(key) or {}).get('value')))
