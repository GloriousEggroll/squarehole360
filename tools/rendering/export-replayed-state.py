#!/usr/bin/env python3
"""Prepare explicitly diagnostic SDK replay bytes for isolated native tests."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

spec = importlib.util.spec_from_file_location('history', Path(__file__).with_name('import-gpu-history.py'))
history = importlib.util.module_from_spec(spec)
spec.loader.exec_module(history)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', type=Path)
    parser.add_argument('output', type=Path, help='New directory')
    args = parser.parse_args()
    report = json.loads((args.reference / 'result.json').read_text())
    metadata = json.loads((args.reference / 'gpu-state/state.json').read_text())
    if metadata.get('provenance') != 'diagnostic_replayed_gpu_state' or metadata.get('has_command_stream') is not False:
        raise ValueError('Reference is not a diagnostic replay snapshot')
    path = args.reference / 'gpu-state/state.xtr'
    if path.stat().st_size > 2 * history.MAX_MEMORY:
        raise ValueError('Diagnostic snapshot is too large')
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != report.get('diagnostic_state_sha256') or report.get('diagnostic_state_viewer_command') != -1:
        raise ValueError('Snapshot hash or complete-frame endpoint is unverified')
    ranges, edram, _ = history.initial_state(data, state_only=True)
    if not ranges or edram is None:
        raise ValueError('Diagnostic snapshot has incomplete GPU state')
    args.output.mkdir(parents=True, exist_ok=False)
    def save(blob):
        sha = hashlib.sha256(blob).hexdigest()
        (args.output / (sha + '.gpu.bin')).write_bytes(blob)
        return dict(sha256=sha, bytes=len(blob))
    manifest = dict(version=1, provenance='diagnostic_replayed_gpu_state',
                    frame=metadata['last_present_api_frame'], reference_state_sha256=digest,
                    source_trace_sha256=report['trace_sha256'], edram=save(edram),
                    ranges=[dict(address=address, **save(blob)) for address, blob in ranges])
    (args.output / 'state.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(args.output / 'state.json')


if __name__ == '__main__':
    main()
