#!/usr/bin/env python3
"""Run known-output shader fixtures on native Vulkan and retain every result."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixtures', type=Path, default=root / 'reports/rendering/native-renderer/semantic-fixtures')
    parser.add_argument('--compiled', type=Path, default=root / 'reports/rendering/native-renderer/compiled-fixtures')
    parser.add_argument('--output', type=Path, default=root / 'reports/rendering/native-renderer/shader-semantics.json')
    parser.add_argument('--global-compiled', type=Path, default=root / 'game/.gow3-assets/native/shaders/compiled')
    args = parser.parse_args()
    binary = root / 'build-native-renderer/gow3-native-shader-test'
    if not binary.exists():
        binary = binary.with_suffix('.exe')
    shaders = json.loads((args.fixtures / 'manifest.json').read_text())['shaders']
    sources = {s['name']: s for s in shaders}
    compiled = {s['sha256']: s for s in json.loads((args.compiled / 'manifest.json').read_text())['shaders']}
    global_compiled = {s['sha256']: s for s in json.loads((args.global_compiled / 'manifest.json').read_text())['shaders']}
    sources['constant'] = dict(name='constant', sha256='0a94610849f6cdebfbfa41fb7c840852e2562bd1f39e1dedddb2275e4cfa0b63')

    def spirv(name):
        source = sources[name]
        record = (global_compiled if name == 'constant' else compiled)[source['sha256']]
        path = (args.global_compiled if name == 'constant' else args.compiled) / record['spirv']
        if record['status'] != 'compiled' or hashlib.sha256(path.read_bytes()).hexdigest() != record['spirv_sha256']:
            raise ValueError(f'Unvalidated SPIR-V: {name}')
        return str(path)

    results = []
    runs = [(shader, shader['name']) for shader in shaders]
    runs += [(sources['texture-explicit-lod'], f'texture-upload-{endian}') for endian in range(4)]
    runs += [(sources['texture-explicit-lod'], f'texture-upload-packed10-{endian}') for endian in range(4)]
    runs += [(sources['texture-explicit-lod'], f'texture-upload-mixed{width}-{endian}')
             for width in (8,16,10) for endian in range(4)]
    runs += [(sources['texture-explicit-lod'], 'texture-upload-packed10-alias')]
    runs += [(sources['texture-explicit-lod'], name) for name in ('texture-upload-bc1', 'texture-upload-depth24', 'texture-upload-depth-float')]
    runs += [(sources['constant'], name) for name in ('constant', 'raster-state', 'color-scale', 'alpha-compare')]
    for shader, name in runs:
        if name == 'passthrough':
            continue
        if name == 'vertex-texture-1d':
            command = [str(binary), spirv('passthrough'), name, spirv(name)]
        elif name.startswith('vertex-'):
            command = [str(binary), spirv('passthrough'), 'vertex-fetch', spirv(name), str(args.fixtures / (name + '.cases'))]
        else:
            command = [str(binary), spirv(shader['name']), name]
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            entry = dict(name=name, shader_sha256=shader['sha256'], passed=result.returncode == 0,
                         exit_code=result.returncode, stdout=result.stdout, stderr=result.stderr)
        except subprocess.TimeoutExpired:
            entry = dict(name=name, shader_sha256=shader['sha256'], passed=False, error='GPU check exceeded 30 seconds')
        results.append(entry)
        print(f"{'PASS' if entry['passed'] else 'FAIL'}: {name}", flush=True)
        if not entry['passed']:
            print(entry)
    report = dict(binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                  passed=sum(r['passed'] for r in results), failed=sum(not r['passed'] for r in results),
                  scope='Native Vulkan known-output instruction tests; not scene equivalence', results=results)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    raise SystemExit(bool(report['failed']))


if __name__ == '__main__':
    main()
