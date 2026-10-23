#!/usr/bin/env python3
"""Read per-thread CPU and disk counters for a running diagnostic session."""
import argparse, csv, os, time
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('pid',type=int);p.add_argument('output',type=Path);p.add_argument('--seconds',type=int,default=180);a=p.parse_args()
root=Path('/proc')/str(a.pid);hz=os.sysconf('SC_CLK_TCK');previous={};last=time.monotonic()
with a.output.open('w') as f:
 w=csv.writer(f);w.writerow(['time','tid','name','state','cpu_percent','processor','wchan','read_bytes','write_bytes'])
 for _ in range(a.seconds):
  time.sleep(1);now=time.monotonic()
  if not root.exists():break
  io={}
  try:
   io=dict(line.split(': ') for line in (root/'io').read_text().splitlines())
  except OSError:pass
  for task in (root/'task').iterdir():
   try:
    st=(task/'stat').read_text();end=st.rfind(')');name=st[st.find('(')+1:end];v=st[end+2:].split();ticks=int(v[11])+int(v[12]);tid=int(task.name)
    if tid in previous:w.writerow([f'{time.time():.6f}',tid,name,v[0],f'{(ticks-previous[tid])/hz/(now-last)*100:.2f}',v[36],(task/'wchan').read_text().strip(),io.get('read_bytes',''),io.get('write_bytes','')])
    previous[tid]=ticks
   except (OSError,IndexError):pass
  last=now;f.flush()
