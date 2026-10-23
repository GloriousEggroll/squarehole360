# bands.py JOURNAL...: interval medians by operation-count band over the whole run
import csv,sys,statistics as st
for p in sys.argv[1:]:
    rows=[r for r in csv.DictReader(l for l in open(p) if not l.startswith('#')) if r['interval_valid']=='1']
    out=[]
    for lo,hi in [(0,500),(500,1000),(1000,1500),(1500,2500),(2500,99999)]:
        v=[float(r['interval_ms']) for r in rows if lo<=int(r['operations'])<hi]
        out.append(f"{lo}-{hi if hi<99999 else '+'}: n={len(v)} {st.median(v):.2f}" if v else f"{lo}-: n=0")
    print(p.split('/')[-1], ' | '.join(out))
