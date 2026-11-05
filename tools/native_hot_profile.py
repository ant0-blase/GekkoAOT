#!/usr/bin/env python3
"""Export source-bound LLVM register-residency hints from reviewed gameplay perf data."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import corpus_boot

def extract(report, primary, minimum):
    result = {}
    for line in report.splitlines():
        fields = line.split('|')
        if len(fields) < 3 or fields[1].strip() != primary:
            continue
        symbol = re.fullmatch(r'\[\.\] func_([0-9A-Fa-f]{8})(?:_budget)?', fields[2].strip())
        if not symbol:
            continue
        weight = float(fields[0].strip().rstrip('%'))
        if weight >= minimum:
            address = int(symbol[1], 16)
            result[address] = result.get(address, 0.0) + weight
    return dict(sorted(result.items()))

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--profile', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--minimum', type=float, default=1.0)
    args = ap.parse_args()
    if not 0 < args.minimum <= 100:
        raise ValueError('minimum must be in (0, 100]')
    metadata = json.loads(args.profile.with_suffix('.json').read_text())
    if metadata['stage'] != 'gameplay' or metadata['exit_code'] != 0:
        raise ValueError('only successful reviewed gameplay profiles are accepted')
    proof = metadata['visual_proof']
    boot = Path(metadata['result_directory'])
    capture = (boot/proof['capture']).resolve()
    if (proof['stage'] != 'gameplay' or not proof['reviewed'] or
            capture.parent != boot.resolve() or not capture.is_file()):
        raise ValueError('missing reviewed local gameplay capture')
    launch = json.loads((boot/'command.json').read_text())
    selected = json.loads(Path(launch['selection']).read_text())
    corpus_boot.validate_launch(selected)
    if selected['game_id'] != metadata['game_id']:
        raise ValueError('profile game identity differs from launch')
    for module, expected in launch['module_sha256'].items():
        if hashlib.sha256(Path(module).read_bytes()).hexdigest() != expected:
            raise ValueError('profile module changed after sampling')
    report = subprocess.check_output(['perf', 'report', '-i', str(args.profile),
        '--stdio', '--no-children', '--call-graph', 'none', '--percent-limit', '0',
        '--sort', 'dso,symbol', '--field-separator', '|'], text=True)
    functions = extract(report, Path(selected['module']).name, args.minimum)
    if not functions:
        raise ValueError('no hot primary-module AOT functions found')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text('# gekkoaot-native-hot-v285 dol-fnv64=' + selected['dol_fnv64'] + '\n' +
                          ''.join(f'{pc:08x}\n' for pc in functions))
    args.output.with_suffix('.json').write_text(json.dumps(dict(
        schema=1, game_id=selected['game_id'], dol=selected['dol'],
        dol_fnv64=selected['dol_fnv64'], profile=str(args.profile.resolve()),
        scene=proof, functions={f'{pc:08x}':weight for pc,weight in functions.items()},
        policy='LLVM residency hints only; full state at observation boundaries'), indent=2)+'\n')
    print(f'Exported {len(functions)} native residency hints to {args.output}')

if __name__ == '__main__':
    main()
