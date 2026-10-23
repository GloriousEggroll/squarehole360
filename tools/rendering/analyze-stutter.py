#!/usr/bin/env python3
"""Summarize guest-frame intervals, driver compile times, and presentation logs."""
import argparse, csv, datetime as dt, json, re
from pathlib import Path
import numpy as np
p=argparse.ArgumentParser();p.add_argument('name');p.add_argument('--since-input',action='store_true');p.add_argument('--seconds',type=float,default=60);a=p.parse_args()
folder=Path('reports/performance')/a.name;log=Path('logs/performance')/(a.name+'.log')
events=json.loads((folder/'events.json').read_text()) if (folder/'events.json').exists() else {}
start=events.get('continue_input_epoch',0) if a.since_input else 0
end=start+a.seconds if start else float('inf')
frames=[];creations=[];audio=[];errors=[]
for line in log.read_text().splitlines():
 try:t=dt.datetime.fromisoformat(line[1:24]).timestamp()
 except ValueError:continue
 if not start<=t<=end:continue
 tid=re.search(r'\[t(\d+)\]',line)
 if 'GPU frame ' in line:
  m=re.search(r'GPU frame (\d+):.* submitted=(\d+).*failed=(\d+).*async_placeholder=(\d+)',line)
  if m:frames.append({'time':t,'frame':int(m[1]),'draws':int(m[2]),'failures':int(m[3]),'pending_draws':int(m[4]),'tid':int(tid[1])})
 if 'Vulkan pipeline creation:' in line:
  m=re.search(r'ms=([\d.]+) placeholder=(\w+) result=(-?\d+)',line)
  creations.append({'time':t,'ms':float(m[1]),'placeholder':m[2],'result':int(m[3]),'tid':int(tid[1])})
 if 'Audio diagnostics:' in line:
  m=re.search(r'silence_frames=(\d+).*peak=([\d.]+) overrange_samples=(\d+)',line)
  if m:audio.append({'silence_frames':int(m[1]),'peak':float(m[2]),'overrange_samples':int(m[3])})
 if '[error]' in line or '[critical]' in line:errors.append(line)
def stats(values):
 values=np.array(values,dtype=float)
 if not len(values):return None
 return {'count':len(values),'median_ms':float(np.median(values)),'p95_ms':float(np.percentile(values,95)),'p99_ms':float(np.percentile(values,99)),'max_ms':float(values.max()),'above_50ms':int(sum(values>50)),'above_100ms':int(sum(values>100))}
intervals=np.diff([f['time'] for f in frames])*1000
render_tids={f['tid'] for f in frames};gpu_compiles=[c['ms'] for c in creations if c['tid'] in render_tids];worker_compiles=[c['ms'] for c in creations if c['tid'] not in render_tids]
presents=[]
for file in folder.glob('gow3_*.csv'):
 if file.name.endswith('_summary.csv'):continue
 try:origin=dt.datetime.strptime(file.stem,'gow3_%Y-%m-%d_%H-%M-%S').timestamp()
 except ValueError:continue
 with file.open() as f:
  next(f,None);next(f,None)
  for row in csv.DictReader(f):
   try:
    if start<=origin+float(row['elapsed'])/1e9<=end:presents.append(float(row['frametime']))
   except (KeyError,ValueError,TypeError):pass
result={'name':a.name,'window_start':dt.datetime.fromtimestamp(start).isoformat() if start else None,'window_seconds':a.seconds if start else None,'guest_frame_intervals':stats(intervals),'presentation_intervals':stats(presents),'render_thread_driver_compiles':stats(gpu_compiles),'worker_driver_compiles':stats(worker_compiles),'frames_with_pending_draws':sum(f['pending_draws']>0 for f in frames),'failed_draws':sum(f['failures'] for f in frames),'pipeline_creation_failures':sum(c['result']!=0 for c in creations),'audio':{'intervals':len(audio),'silence_frames':sum(x['silence_frames'] for x in audio),'overrange_samples':sum(x['overrange_samples'] for x in audio),'peak':max((x['peak'] for x in audio),default=0)},'errors':errors[:20]}
(folder/('summary-input.json' if a.since_input else 'summary.json')).write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
