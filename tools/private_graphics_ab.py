#!/usr/bin/env python3
"""Compare graphics queue variants in serial private sessions with launcher saves.

The numerical report is provisional until captures show the same gameplay scene.
This wrapper never sends keys to the desktop and never writes the launcher cards.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import subprocess
import sys

import corpus_boot as boot


def summarize(result, tail_seconds):
    samples = json.loads(Path(result['process_samples']).read_text())['samples']
    tail = [r for r in samples if r[0] >= samples[-1][0] - tail_seconds] if samples else []
    cpu = (100 * (tail[-1][1] - tail[0][1]) / (tail[-1][0] - tail[0][0])
           if len(tail) > 1 else None)
    fps = [float(m.group(1)) for line in result['perf_lines']
           if (m := re.search(r'\bguest_fps=([0-9.]+)', line))]
    return dict(result=result['result_path'], status=result['status'],
                tail_seconds=tail[-1][0] - tail[0][0] if len(tail) > 1 else None,
                tail_cpu_percent_one_core=cpu,
                tail_rss_mean_bytes=sum(r[2] for r in tail) / len(tail) if tail else None,
                last_guest_fps=fps[-10:],
                cards_sha256={Path(r['source']).name: r['sha256']
                              for r in result['launcher_card_copy']},
                runner_sha256=result['runner_sha256'],
                bridge_sha256=result['bridge_sha256'],
                module_sha256=result['module_sha256'],
                screenshots=result['screenshots'][-3:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--selection', type=Path, required=True)
    parser.add_argument('--native-selection', type=Path,
                        help='compare reference/native/reference compiled modules with the same graphics settings')
    parser.add_argument('--runner', type=Path, required=True)
    parser.add_argument('--results', type=Path, required=True)
    parser.add_argument('--after-status', type=Path)
    parser.add_argument('--seconds', type=int, default=110)
    parser.add_argument('--input-seconds', type=int, default=65)
    parser.add_argument('--tail-seconds', type=int, default=25)
    args = parser.parse_args()
    metadata = json.loads(args.selection.read_text())
    boot.validate_launch(metadata)
    if args.native_selection:
        native_metadata = json.loads(args.native_selection.read_text())
        boot.validate_launch(native_metadata)
        library = ctypes.CDLL(native_metadata['module'])
        try:
            binder = library.gekkoaot_bind_native_hardware
        except AttributeError as exc:
            raise ValueError('native module does not export its hardware binding') from exc
        binder.argtypes = [ctypes.c_void_p]
        binder.restype = ctypes.c_int
        if binder(None) != 0:
            raise ValueError('native module accepts an invalid hardware binding')
        if native_metadata.get('native_wgpipe_try'):
            try:
                partial_binder = library.gekkoaot_bind_native_wgpipe_try
            except AttributeError as exc:
                raise ValueError('native module does not export its partial WGPIPE binding') from exc
            partial_binder.argtypes = [ctypes.c_void_p]
            partial_binder.restype = ctypes.c_int
            if partial_binder(None) != 0:
                raise ValueError('native module accepts an invalid partial WGPIPE binding')
        for name in ('game_id', 'source', 'dol', 'sdk_manifest', 'bridge', 'native_os', 'secondary_modules'):
            if metadata[name] != native_metadata[name]:
                raise ValueError('module comparison requires identical ' + name)
        if metadata['module'] == native_metadata['module']:
            raise ValueError('module comparison requires distinct primary modules')
    args.results = args.results.resolve()
    args.results.mkdir(parents=True, exist_ok=True)
    summary = dict(schema=1, game_id=metadata['game_id'], variants=[],
                   comparison='native-hardware-modules' if args.native_selection else 'graphics-batch-slot',
                   validation='requires visual review; no global performance claim')
    for index, flag in enumerate((0, 1, 0)):
        if (args.results / 'stop.request').exists():
            break
        target = args.results / (f'{index}-module-{flag}' if args.native_selection else f'{index}-slot-{flag}')
        selection = args.native_selection if args.native_selection and flag else args.selection
        command = [sys.executable, str(boot.ROOT / 'tools/private_game_campaign.py'),
                   '--only', metadata['game_id'], '--selection', str(selection.resolve()),
                   '--runner', str(args.runner.resolve()), '--results', str(target),
                   '--seconds', str(args.seconds), '--input-seconds', str(args.input_seconds)]
        if index == 0 and args.after_status:
            command += ['--after-status', str(args.after_status.resolve())]
        env = dict(os.environ, GEKKOAOT_GX_DIRECT_BATCH_SLOT='0' if args.native_selection else str(flag),
                   GEKKOAOT_NATIVE_GX_STREAM_FIFO='1')
        with (args.results / f'{index}-supervisor.log').open('w') as log:
            subprocess.run(command, cwd=boot.ROOT, env=env, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
        state = json.loads((target / 'status.json').read_text())
        result = json.loads(Path(state['games'][0]['result']).read_text())
        if args.native_selection and flag:
            runtime_log = Path(result['result_path']).parent / 'run.log'
            if 'GEKKOAOT_NATIVE_HARDWARE_BIND_V243=1 scope=primary' not in runtime_log.read_text():
                raise ValueError('native hardware binding did not activate; CPU comparison invalid')
            if native_metadata.get('native_wgpipe_try') and 'GEKKOAOT_NATIVE_WGPIPE_TRY_BIND_V244=1 scope=primary' not in runtime_log.read_text():
                raise ValueError('partial WGPIPE binding did not activate; CPU comparison invalid')
        row = summarize(result, args.tail_seconds)
        if args.native_selection:
            row.update(selected_native_module=bool(flag), direct_batch_slot=0)
        else:
            row.update(direct_batch_slot=flag)
        summary['variants'].append(row)
        boot.atomic_json(args.results / 'summary.json', summary)
        print(json.dumps(row), flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
