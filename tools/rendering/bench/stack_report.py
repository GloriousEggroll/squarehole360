#!/usr/bin/env python3
"""Inclusive/self breakdown of perf-script stacks (ip,sym) for one thread."""
import sys,re,collections
stacks=[];cur=[]
for l in open(sys.argv[1]):
    l=l.rstrip()
    if not l:
        if cur: stacks.append(cur); cur=[]
        continue
    p=l.split(None,1)
    if len(p)==2: cur.append(p[1])
if cur: stacks.append(cur)
n=len(stacks)
def short(s): return re.sub(r'\(.*','',s)[-100:]
root=sys.argv[2] if len(sys.argv)>2 else 'Observe('
sel=[s for s in stacks if any(root in f for f in s)]
print(f'samples={n} under {root}: {100*len(sel)/n:.1f}%')
inc=collections.Counter()
for s in sel:
    for f in set(short(f) for f in s): inc[f]+=1
print('-- inclusive'); [print(f'{100*v/n:5.1f}%  {k}') for k,v in inc.most_common(45)]
print('-- self'); [print(f'{100*v/n:5.1f}%  {k}') for k,v in collections.Counter(short(s[0]) for s in sel).most_common(30)]
