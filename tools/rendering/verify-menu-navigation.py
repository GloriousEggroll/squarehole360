#!/usr/bin/env python3
"""Exercise the controller-style menu via its keyboard aliases on a test config.

Requires a running --gow3_config=reports/rendering/native-hdr/menu-apply-test.toml
game. This intentionally refuses to change the user's normal config.
"""
from pathlib import Path
import hashlib
import json
import subprocess
import time
import tomllib

root = Path(__file__).resolve().parents[2]
report = root / 'reports/rendering/native-hdr'
config = report / 'menu-apply-test.toml'
log = report / 'game-menu-navigation.log'

def key(*keys):
    subprocess.run(['wtype', *keys], check=True)

def capture(name):
    subprocess.run(['grim', '-o', 'DP-2', str(report / name)], check=True)

def acknowledged():
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        digest = hashlib.sha256(config.read_bytes()).hexdigest()
        for path in report.glob(config.name + '.runtime.*.toml'):
            status = tomllib.loads(path.read_text())
            if status['digest'] == digest and not status.get('error'):
                return status
        time.sleep(.1)
    raise AssertionError('No live settings acknowledgement')

before = tomllib.loads(config.read_text())
assert before['present_effect'] == 'fsr'
assert before['present_fsr_sharpness_reduction'] < 2
status = acknowledged()
assert Path(status['config']).resolve() == config.resolve()
assert Path('/proc', str(status['pid']), 'exe').resolve() == root / 'build/bin/gow3'
subprocess.run(['wlrctl', 'toplevel', 'focus', 'app_id:gow3'], check=True)
time.sleep(.1)
toggles = [line for line in log.read_text().splitlines()
           if 'Settings menu ' in line and f"[t{status['pid']}]" in line]
if toggles and 'opened' in toggles[-1]:
    key('-k', 'F4', '-s', '200')
key('-k', 'F4', '-s', '200', '-k', 'Down', '-s', '100', '-k', 'Return', '-s', '100',
    '-k', 'Down', '-s', '100', '-k', 'Right', '-s', '100',
    '-P', 'Control_L', '-s', '75', '-k', 'Return', '-s', '75', '-p', 'Control_L', '-s', '800')
capture('menu-controller-after-input.png')
after = tomllib.loads(config.read_text())
assert abs(after['present_fsr_sharpness_reduction'] - before['present_fsr_sharpness_reduction'] - .01) < .00001
applied = acknowledged()
capture('menu-controller-apply.png')
key('-k', 'Escape', '-s', '150')
capture('menu-controller-back.png')
# Re-entering Image must remember the softness row. Undo and apply it.
key('-k', 'Return', '-s', '150', '-k', 'Left', '-s', '100',
    '-P', 'Control_L', '-s', '75', '-k', 'Return', '-s', '75', '-p', 'Control_L', '-s', '800')
restored = tomllib.loads(config.read_text())
assert abs(restored['present_fsr_sharpness_reduction'] - before['present_fsr_sharpness_reduction']) < .00001
acknowledged()
result = {'passed': True, 'before': before['present_fsr_sharpness_reduction'],
          'applied': after['present_fsr_sharpness_reduction'],
          'restored': restored['present_fsr_sharpness_reduction'], 'runtime_ack': applied}
(report / 'menu-navigation-integration.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result))
