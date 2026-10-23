#!/usr/bin/env python3
"""Run a diagnostic command with bounded, read-only Linux host telemetry.

This is an external test helper, not a game runtime dependency. CPU usage is
measured from interval deltas (100% = one logical CPU); command lines and other
process memory are never read. Missing sensors are recorded as unavailable.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import time
from amdgpu_metrics import read_metrics


def read(path):
    try:
        return path.read_text().strip()
    except OSError:
        return None


def processes():
    result = {}
    for entry in Path('/proc').iterdir():
        if not entry.name.isdecimal():
            continue
        value = read(entry / 'stat')
        if value is None:
            continue
        try:
            opening, closing = value.index('('), value.rindex(')')
            fields = value[closing + 1:].split()
            key = (int(entry.name), int(fields[19]))
            result[key] = (value[opening + 1:closing], int(fields[11]) + int(fields[12]))
        except (ValueError, IndexError):
            continue
    return result


def sensors():
    paths = {}
    for card in sorted(Path('/sys/class/drm').glob('card[0-9]*')):
        if '-' in card.name:
            continue
        device = card / 'device'
        for name in ('vendor', 'device', 'gpu_busy_percent', 'mem_busy_percent', 'pp_dpm_sclk', 'pp_dpm_mclk',
                     'power_dpm_force_performance_level', 'pp_power_profile_mode'):
            path = device / name
            if path.exists():
                paths[f'{card.name}/{name}'] = path
        for hwmon in (device / 'hwmon').glob('*'):
            for path in hwmon.iterdir():
                if path.name.endswith(('_input', '_average', '_cap', '_label')) and path.name.startswith(('freq', 'temp', 'power', 'fan')):
                    paths[f'{card.name}/{hwmon.name}/{path.name}'] = path
    for cpu in Path('/sys/devices/system/cpu').glob('cpu[0-9]*'):
        path = cpu / 'cpufreq/scaling_cur_freq'
        if path.exists():
            paths[f'{cpu.name}/scaling_cur_freq_khz'] = path
    return paths


def discover_gpu_fds(current):
    result = []
    for pid, start_ticks in current:
        root = Path('/proc') / str(pid)
        try:
            if root.stat().st_uid != os.getuid():
                continue
            for fd in (root / 'fd').iterdir():
                try:
                    if os.readlink(fd).startswith('/dev/dri/'):
                        result.append((pid, start_ticks, root / 'fdinfo' / fd.name))
                except OSError:
                    continue
        except OSError:
            continue
    return result


def gpu_clients(fds, current):
    # Duplicate descriptors and forked clients share the same device/client ID.
    # Keep one counter record with all observed owners, not duplicate GPU time.
    records = {}
    for pid, start_ticks, path in fds:
        owner = current.get((pid, start_ticks))
        raw = read(path) if owner else None
        if raw is None:
            continue
        fields = dict(line.split(':', 1) for line in raw.splitlines() if line.startswith('drm-') and ':' in line)
        client, device = fields.get('drm-client-id'), fields.get('drm-pdev')
        if client is None or device is None:
            continue
        engines = {}
        for key, value in fields.items():
            pieces = value.split()
            if key.startswith('drm-engine-') and len(pieces) == 2 and pieces[1] == 'ns' and pieces[0].isdecimal():
                engines[key.removeprefix('drm-engine-')] = int(pieces[0])
        if not engines:
            continue
        key = (device.strip(), client.strip())
        record = records.setdefault(key, {'device': key[0], 'client': key[1], 'owners': [], 'engine_ns': engines})
        identity = {'pid': pid, 'start_ticks': start_ticks, 'name': owner[0]}
        if identity not in record['owners']:
            record['owners'].append(identity)
    return list(records.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--interval', type=float, default=1.0)
    parser.add_argument('--gpu-clients', action='store_true', help='Read per-client DRM engine counters; discover render descriptors every five seconds')
    parser.add_argument('--amdgpu-metrics', action='store_true', help='Read bounded versioned AMD GPU clock/throttle tables; no device settings are changed')
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command or not 0.5 <= args.interval <= 10:
        parser.error('Provide a command and an interval between 0.5 and 10 seconds')
    paths = sensors()
    metric_paths = {key.removesuffix('/vendor'): path.parent / 'gpu_metrics'
                    for key, path in paths.items()
                    if key.endswith('/vendor') and read(path) == '0x1002'} if args.amdgpu_metrics else {}
    ticks = os.sysconf('SC_CLK_TCK')
    before = processes()
    previous = began = time.monotonic()
    gpu_fds = []
    next_discovery = began
    with args.output.open('x') as stream:
        def emit(value):
            value.update(utc=datetime.now(timezone.utc).isoformat(), monotonic=time.monotonic())
            stream.write(json.dumps(value, separators=(',', ':')) + '\n')
            stream.flush()
        emit({'kind': 'start', 'command': command, 'clock_ticks_per_second': ticks,
              'logical_cpus': os.cpu_count(), 'sensor_paths': {k: str(v) for k, v in paths.items()},
              'gpu_clients_enabled': args.gpu_clients,
              'gpu_metrics_paths': {key: str(path) for key, path in metric_paths.items()},
              'limits': 'Interval snapshots do not prove continuous idle conditions. Optional engine counters cover accessible same-user DRM clients and may overlap across engines.'})
        child = subprocess.Popen(command)
        try:
            while True:
                now = time.monotonic()
                current = processes()
                if args.gpu_clients and now >= next_discovery:
                    gpu_fds = discover_gpu_fds(current)
                    next_discovery = now + 5
                usage = []
                for key, (name, total) in current.items():
                    prior = before.get(key)
                    if prior is None or now <= previous:
                        continue
                    percent = 100 * (total - prior[1]) / ticks / (now - previous)
                    if percent >= 2 or name == 'gow3':
                        usage.append({'pid': key[0], 'start_ticks': key[1], 'name': name, 'cpu_percent': round(percent, 2)})
                gpu = gpu_clients(gpu_fds, current) if args.gpu_clients else None
                sensor_values = {k: read(p) for k, p in paths.items()}
                metric_values = {key: read_metrics(path) for key, path in metric_paths.items()}
                emit({'kind': 'sample', 'elapsed': now - began,
                      'process_cpu': sorted(usage, key=lambda x: -x['cpu_percent'])[:24],
                      'gpu_clients': gpu, 'collection_ms': 1000 * (time.monotonic() - now),
                      'sensors': sensor_values,
                      'amdgpu_metrics': metric_values})
                before, previous = current, now
                try:
                    code = child.wait(timeout=max(0.01, args.interval - (time.monotonic() - now)))
                    break
                except subprocess.TimeoutExpired:
                    pass
        except BaseException:
            if child.poll() is None:
                child.terminate()
                child.wait()
            raise
        emit({'kind': 'end', 'elapsed': time.monotonic() - began, 'exit_code': code})
    return code


if __name__ == '__main__':
    raise SystemExit(main())
