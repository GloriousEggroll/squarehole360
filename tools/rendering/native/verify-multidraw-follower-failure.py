#!/usr/bin/env python3
"""Run the intentional poisoned-group abort in an isolated real-GPU child."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess

# Preserve the real fatal exit without generating a large GPU-process core file.
if os.name != 'nt':
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

parser=argparse.ArgumentParser()
parser.add_argument('--binary',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--capacity',type=int,choices=(64,128),default=64)
parser.add_argument('--discard-dummy-fsi-attachment',action='store_true')
args=parser.parse_args()
command=[str(args.binary.resolve()),'--multi-draw-follower-failure',
         f'--async-submission-capacity={args.capacity}']
if args.discard_dummy_fsi_attachment:command.append('--discard-dummy-fsi-attachment')
completed=subprocess.run(command,text=True,capture_output=True,timeout=60)
proofs=[]
for line in completed.stdout.splitlines():
    if line.startswith('MULTIDRAW_FATAL_PROOF '):
        proofs.append(json.loads(line.removeprefix('MULTIDRAW_FATAL_PROOF ')))
expected_exit=3 if os.name=='nt' else -signal.SIGABRT
proof=proofs[0] if len(proofs)==1 else None
passed=(completed.returncode==expected_exit and proof is not None and
        proof.get('older_accepted_children')==1 and proof.get('submissions_since_first_draw')==0 and
        proof.get('has_pending_owners') is True and len(proof.get('refused_paths',[]))==14 and
        proof.get('expected_scene_destructor_termination') is True and
        (not args.discard_dummy_fsi_attachment or (proof.get('dummy_discard_requested') is True and
         proof.get('dummy_discard_installed') is True)) and
        'Native renderer shutdown completion failed:' in completed.stderr)
report={'schema':1,'passed':passed,'command':command,'returncode':completed.returncode,
        'expected_fatal_exit':expected_exit,'proof':proof,'stdout':completed.stdout,'stderr':completed.stderr,
        'scope':'Actual isolated Scene/GPU command recording and submit observation; intentional fatal destructor, no production waits or recovery.'}
args.output.parent.mkdir(parents=True,exist_ok=True)
args.output.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'passed':passed,'returncode':completed.returncode,'proof':proof}))
if not passed:raise SystemExit('Poisoned follower fixture failed its real fatal/refusal/submission proof')
