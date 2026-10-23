# perop.py JOURNAL...: fight-band (1500-2500 ops) median ms, median us/op, and mean ops
import csv,sys,statistics as st
for p in sys.argv[1:]:
    rows=[r for r in csv.DictReader(l for l in open(p) if not l.startswith('#')) if r['interval_valid']=='1']
    v=[(float(r['interval_ms']),int(r['operations'])) for r in rows if 1500<=int(r['operations'])<2500]
    print(f"{p.split('/')[-1]:28s} n={len(v)} ms={st.median(x for x,_ in v):.2f} us/op={st.median(1000*x/o for x,o in v):.3f} ops={st.mean(o for _,o in v):.0f}")
