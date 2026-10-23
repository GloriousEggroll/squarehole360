# frame_busy.py SAMPLES JOURNAL THREADS: busy fraction per thread by frame-size band.
import collections,csv,sys,bisect
names={l.split(None,1)[0]:l.split(None,1)[1].strip() for l in open(sys.argv[3]) if l.strip()}
samples=collections.defaultdict(list)
for l in open(sys.argv[1]):
    p=l.split()
    if len(p)>=2:samples[p[0]].append(float(p[1].rstrip(':'))*1e9)
rows=[r for r in csv.DictReader(l for l in open(sys.argv[2]) if not l.startswith('#')) if r['interval_valid']=='1']
for v in samples.values():v.sort()
bands=[(0,1000),(1000,2500),(2500,10**9)];acc=collections.defaultdict(lambda:[0.0,0.0,0])
for r in rows:
    end=int(r['steady_ns']);start=end-float(r['interval_ms'])*1e6;ops=int(r['operations'])
    band=next(b for b in bands if b[0]<=ops<b[1])
    for tid,v in samples.items():
        n=bisect.bisect_left(v,end)-bisect.bisect_left(v,start)
        if n==0 and not (v and v[0]<=end and v[-1]>=start):continue
        a=acc[(band,tid)];a[0]+=n*1e6;a[1]+=end-start;a[2]+=1
for band in bands:
    line=[f"{band[0]}-{band[1] if band[1]<10**9 else '+'}:"]
    for tid in samples:
        a=acc.get((band,tid))
        if a and a[1]:line.append(f"{names.get(tid,tid)[:14]}={100*a[0]/a[1]:.0f}%(n={a[2]})")
    print(' '.join(line))
