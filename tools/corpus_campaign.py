#!/usr/bin/env python3
"""Serialize compilation and boot observations for every distinct corpus disc.

No keys are sent. Boot/menu/attract observations cannot certify gameplay or
transitions. Existing inventory and user cards are preserved.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time

import corpus_boot as boot

ROOT = boot.ROOT


def find_selection(row):
    candidates = []
    for path in (ROOT / '.gekkoaot/cache/discs').glob('*/meta/aot-launch.json'):
        try:
            value = json.loads(path.read_text())
            if (Path(value['source']).resolve() != Path(row['source']).resolve()
                    or value['game_id'] != row['game_id']):
                continue
            boot.validate_launch(value)
            if boot.sha256(value['dol']) != row['main_dol_sha256']:
                continue
            candidates.append(path)
        except (KeyError, ValueError, OSError):
            continue
    return max(candidates, key=lambda p: p.stat().st_mtime) if candidates else None


def compile_image(row, entry, status, save, directory):
    if shutil.disk_usage(ROOT).free < 4 * 1024 ** 3:
        raise RuntimeError('less than 4 GiB free; compilation stopped before creating more artifacts')
    env = dict(os.environ, GEKKOAOT_ISO=row['source'], GEKKOAOT_BACKEND='llvm',
               GEKKOAOT_NATIVE_OS='0', GEKKOAOT_NATIVE_HW='1', GEKKOAOT_JOBS='4',
               GEKKOAOT_FP_STATE_SSA='0', GEKKOAOT_PGO_AUTO_USE='0')
    for key in ('DOLRECOMP_SIDE_EXIT_REPORT', 'DOLRECOMP_SIDE_EXIT_ANALYSIS_ONLY',
                'GEKKOAOT_RUN_ARGS', 'DOLRECOMP_LLVM_DUMP_IR'):
        env.pop(key, None)
    path = directory / (row['game_id'] + '-' + row['main_dol_sha256'][:12] + '.compile.log')
    entry.update(stage='compiling', compile_log=str(path))
    with path.open('w') as log:
        process = subprocess.Popen(['nice', '-n', '10', str(ROOT / 'bin/gekkoaotctl'), 'compile'],
                                   cwd=ROOT, env=env, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        entry['compile_pid'] = process.pid
        save()
        try:
            result = process.wait(timeout=10800)
        except BaseException:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            raise
    entry['compile_exit'] = result
    save()
    if result:
        raise RuntimeError(f'compile exit {result}; see {path}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--corpus', type=Path, default=ROOT / '.gekkoaot/compat/corpus-v232/corpus.json')
    parser.add_argument('--results', type=Path, default=ROOT / '.gekkoaot/compat/campaign-v233')
    parser.add_argument('--after-builds', type=Path, help='wait for an existing compilation queue first')
    parser.add_argument('--compile', action='store_true', help='build titles without exact launch selections')
    parser.add_argument('--refresh-launch', action='store_true', help='run the controller for every title to select the current compiler/renderer; cached modules can be reused')
    parser.add_argument('--seconds', type=int, default=40)
    parser.add_argument('--only', help='comma-separated game IDs')
    args = parser.parse_args()
    if args.seconds < 20:
        parser.error('--seconds must be at least 20')
    if args.refresh_launch and not args.compile:
        parser.error('--refresh-launch requires --compile')
    args.results = args.results.resolve()
    args.results.mkdir(parents=True, exist_ok=True)
    status = {'schema': 1, 'pid': os.getpid(), 'started': time.time(),
              'phase': 'waiting' if args.after_builds else 'running',
              'scope': 'serial boot observations; no input, gameplay or transition certification',
              'games': []}
    path = args.results / 'status.json'
    def save():
        boot.atomic_json(path, status)
    save()
    if args.after_builds:
        while True:
            prior = json.loads(args.after_builds.read_text())
            if prior['phase'] == 'compilation-finished':
                break
            try:
                os.kill(prior['pid'], 0)
            except ProcessLookupError:
                raise RuntimeError('prior compilation supervisor stopped without finishing; inspect its active children')
            time.sleep(10)
    corpus = json.loads(args.corpus.read_text())
    rows = [r for r in corpus['images'] if r['kind'] == 'disc' and not r.get('duplicate_of')]
    if args.only:
        identifiers = set(args.only.split(','))
        rows = [r for r in rows if r['game_id'] in identifiers]
    status.update(phase='running', distinct_disc_images=len(rows))
    save()
    for index, row in enumerate(rows, 1):
        print(f"[{index}/{len(rows)}] {row['game_id']} {row['name']}", flush=True)
        entry = dict(game_id=row['game_id'], source=row['source'], started=time.time(), stage='preparing')
        status['games'].append(entry)
        save()
        try:
            selection = find_selection(row)
            if selection is None or args.refresh_launch:
                if not args.compile:
                    raise RuntimeError('no exact controller selection; use --compile to prepare this title')
                compile_image(row, entry, status, save, args.results)
                selection = find_selection(row)
                if selection is None:
                    raise RuntimeError('compile succeeded without a valid launch selection for this DOL')
            entry.update(stage='booting', selection=str(selection))
            save()
            evidence = boot.run(selection, args.results / 'boots', args.seconds,
                                isolated_input=True)
            entry.update(stage=evidence['status'], validation=evidence['validation'],
                         result=evidence['result_path'],
                         runner_sha256=evidence['runner_sha256'], result_line=evidence['result_line'],
                         renderer_faults=evidence['renderer_faults'])
        except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
            entry.update(stage='preparation-or-launch-failure', error=str(error))
            print('  ' + str(error), flush=True)
        entry['finished'] = time.time()
        save()
    status.update(phase='boot-observations-finished', finished=time.time())
    save()
    return 0 if all(r['stage'] == 'boot-progress-observed' for r in status['games']) else 1


if __name__ == '__main__':
    raise SystemExit(main())
