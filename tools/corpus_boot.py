#!/usr/bin/env python3
"""Boot a controller-selected AOT module without writing the user's cards.

This records boot progress, never certifies gameplay from FPS or an intro.
Pass the meta/aot-launch.json produced by gekkoaotctl compile.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def fnv64(path):
    value = 1469598103934665603
    for byte in Path(path).read_bytes():
        value = ((value ^ byte) * 1099511628211) & 0xffffffffffffffff
    return f'{value:x}'


def validate_launch(metadata):
    if metadata.get('schema') != 1 or metadata.get('native_os') not in ('0', '1'):
        raise ValueError('expected controller selection with an explicit NativeOS mode')
    for key in ('source', 'dol', 'module', 'bridge', 'sdk_manifest'):
        if not Path(metadata[key]).is_file():
            raise ValueError(f'{key} missing: {metadata[key]}')
    if fnv64(metadata['dol']) != metadata['dol_fnv64']:
        raise ValueError('DOL changed since controller selection')
    if fnv64(metadata['sdk_manifest']) != metadata['sdk_fnv64']:
        raise ValueError('SDK manifest changed since controller selection')
    module = Path(metadata['module'])
    compiled_dol = module.parent / 'dolrecomp-output/generated/main.dol'
    if not compiled_dol.exists() or sha256(compiled_dol) != sha256(metadata['dol']):
        raise ValueError('module source does not match selected DOL')
    for secondary in metadata['secondary_modules']:
        if not Path(secondary).is_file():
            raise ValueError(f'secondary module missing: {secondary}')


def sample_process(pid):
    try:
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(') ', 1)[1].split()
        cpu = (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')
        rss = int(fields[21]) * os.sysconf('SC_PAGE_SIZE')
        return cpu, rss
    except (OSError, ValueError, IndexError):
        return None


def screenshot(pid, destination):
    try:
        windows = subprocess.run(['xdotool', 'search', '--onlyvisible', '--pid', str(pid)],
                                 capture_output=True, text=True, timeout=3)
        for window in windows.stdout.splitlines():
            captured = subprocess.run(['import', '-window', window, str(destination)],
                                      capture_output=True, timeout=5)
            if captured.returncode == 0 and destination.is_file():
                return True
    except (OSError, subprocess.TimeoutExpired):
        pass
    return False


def atomic_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n')
    temporary.replace(path)


def run(metadata_path, results, seconds, runner=ROOT / 'bin/gekkoaot-native-run', isolated_input=False,
        input_driver=None):
    metadata = json.loads(metadata_path.read_text())
    validate_launch(metadata)
    dol_hash = sha256(metadata['dol'])
    target = results / (metadata['game_id'] + '-' + dol_hash[:12])
    target.mkdir(parents=True, exist_ok=True)
    # Each execution owns a fresh directory; interrupted evidence is preserved.
    target = target / time.strftime('%Y%m%d-%H%M%S', time.gmtime())
    target.mkdir()
    stop = target / 'stop.request'
    snapshot = target / 'snapshot.request'
    command = [str(runner.resolve()), '--module', metadata['module'],
               '--dol', metadata['dol'], '--game-id', metadata['game_id'],
               '--disc-image', metadata['source'], '--native-gx-bridge', metadata['bridge'],
               '--sdk-manifest', metadata['sdk_manifest'], '--stop-file', str(stop),
               '--snapshot-file', str(snapshot)]
    for name in ('boot', 'fst', 'bi2'):
        path = Path(metadata['dol']).parent / (name + '.bin')
        if path.exists():
            command += ['--' + name + '-bin', str(path)]
    for path in metadata['secondary_modules']:
        command += ['--secondary-module', path]
    env = dict(os.environ, GEKKOAOT_NATIVE_OS=metadata['native_os'], GEKKOAOT_NATIVE_HW='1',
               GEKKOAOT_RUNTIME_PARALLEL='1', GEKKOAOT_NATIVE_PC_TRACE='1',
               GEKKOAOT_USER_DIR=str(target / 'user'), SDL_VIDEODRIVER='x11',
               GEKKOAOT_FRAME_INTERPOLATION='0',
               GEKKOAOT_NATIVE_GX_WIDTH=os.environ.get('GEKKOAOT_NATIVE_GX_WIDTH','1920'),
               GEKKOAOT_NATIVE_GX_HEIGHT=os.environ.get('GEKKOAOT_NATIVE_GX_HEIGHT','1080'),
               GEKKOAOT_NATIVE_GX_FULLSCREEN='0',
               GEKKOAOT_NATIVE_GX_FIFO_TEXTURE_CACHE=os.environ.get('GEKKOAOT_NATIVE_GX_FIFO_TEXTURE_CACHE','0'),
               GEKKOAOT_NATIVE_GX_PRESENT_ON_COPY='0',
               GEKKOAOT_NATIVE_GX_PRESENT_FALLBACK_VI='1')
    # A diagnostic launch must not inherit developer hooks or another card path.
    for key in ('GEKKOAOT_INPUT_CONFIG', 'GEKKOAOT_SHADOW_OUTPUT', 'GEKKOAOT_RUN_ARGS',
                'GEKKOAOT_RUN_PREFIX', 'LLVM_PROFILE_FILE'):
        env.pop(key, None)
    if isolated_input:
        # Keep port 1 connected, but assign no physical keys or controller.
        # XQueryKeymap otherwise makes background tests react to user typing.
        config = target / 'input-observation.ini'
        config.write_text('[port1]\nsource=keyboard\n[port2]\nsource=disabled\n'
                          '[port3]\nsource=disabled\n[port4]\nsource=disabled\n')
        env['GEKKOAOT_INPUT_CONFIG'] = str(config)
    if input_driver is not None:
        if isolated_input:
            raise ValueError('neutral input and an input driver are mutually exclusive')
        input_driver.prepare(target, env, metadata)
    modules = [metadata['module']] + metadata['secondary_modules']
    evidence = {'schema': 1, 'stage': 'boot-observation', 'source': metadata['source'],
                'game_id': metadata['game_id'], 'selection': str(metadata_path.resolve()),
                'selection_provenance': metadata['provenance'], 'dol_sha256': dol_hash,
                'module_sha256': {path: sha256(path) for path in modules},
                'runner_sha256': sha256(runner),
                'bridge_sha256': sha256(metadata['bridge']), 'command': command,
                'environment': {k: v for k, v in env.items()
                                if k.startswith('GEKKOAOT_') or k == 'SDL_VIDEODRIVER'},
                'input_sent': False, 'physical_input_isolated': isolated_input or input_driver is not None,
                'private_graphical_session': input_driver is not None,
                'user_cards_touched': False,
                'validation': {k: 'unverified' for k in
                               ('boot', 'gameplay', 'combat', 'level_transition',
                                'save_load', 'cpu_performance', 'ram_performance',
                                'crash_free_session')}}
    atomic_json(target / 'command.json', evidence)
    start = time.monotonic()
    samples = []
    captures = []
    scheduled = [5, 15, max(20, seconds - 3)]
    if input_driver is not None:
        scheduled = sorted(set([5] + list(range(10, seconds, 10)) + [seconds - 3]))
    if os.environ.get('GEKKOAOT_TEST_EARLY_CAPTURES') == '1':
        scheduled = sorted(set(scheduled + [1, 2, 3, 4, 7, 8]))
    forced_stop = False
    process = None
    try:
        with (target / 'run.log').open('w') as log:
            process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            atomic_json(target / 'pid.json', {'pid': process.pid, 'started': time.time()})
            while process.poll() is None and time.monotonic() - start < seconds:
                elapsed = time.monotonic() - start
                measurement = sample_process(process.pid)
                if measurement:
                    samples.append((elapsed, *measurement))
                if scheduled and elapsed >= scheduled[0]:
                    instant = scheduled.pop(0)
                    snapshot.touch()
                    destination = target / f'screen-{instant}.png'
                    captured = (input_driver.capture(destination) if input_driver is not None
                                else screenshot(process.pid, destination))
                    if captured:
                        captures.append(str(destination))
                        if input_driver is not None:
                            input_driver.after_capture(elapsed)
                if input_driver is not None:
                    input_driver.tick(process.pid, elapsed)
                time.sleep(0.25)
            if process.poll() is None:
                stop.touch()
                try:
                    process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    forced_stop = True
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait()
    finally:
        if input_driver is not None:
            try:
                input_driver.finish()
            except (OSError, RuntimeError, subprocess.TimeoutExpired):
                # A lost compositor must never prevent cleanup of the runner.
                pass
        if process is not None and process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        snapshot.unlink(missing_ok=True)
        # Screens and register logs are retained; 32 MiB snapshots are redundant
        # on normal timer stops. Preserve them when debugging a fault/hang.
        log_text = (target / 'run.log').read_text(errors='replace')
        if 'status="host requested exit"' in log_text and not forced_stop:
            snapshot.with_suffix('.request.mem1.bin').unlink(missing_ok=True)
    result = re.findall(r'GEKKOAOT_NATIVE_RUN_RESULT[^\n]*', log_text)
    frame_rates = re.findall(r'GEKKOAOT_PERF_V49=1[^\n]*', log_text)
    pcs = set(re.findall(r'GEKKOAOT_NATIVE_PC_TRACE_V\d+=1[^\n]*?\bpc=([0-9a-f]+)', log_text))
    guest_fps = [float(value) for value in re.findall(r'\bguest_fps=([0-9.]+)', '\n'.join(frame_rates))]
    progress = (any(value > 0 for value in guest_fps)
                and (env.get('GEKKOAOT_NATIVE_PC_TRACE') != '1' or len(pcs) > 2)
                and 'GEKKOAOT_NATIVE_DOL_LOADED' in log_text)
    renderer_faults = re.findall(r'GEKKOAOT_NATIVE_GX_FIFO_ERROR[^\n]*', log_text)
    status = ('renderer-failure' if renderer_faults else
              'forced-stop' if forced_stop else 'boot-progress-observed'
              if result and 'status="host requested exit"' in result[-1] and progress
              else 'no-boot-progress' if result and 'status="host requested exit"' in result[-1]
              else 'guest-or-host-failure')
    steady = [row for row in samples if row[0] >= 10]
    cpu_percent = None
    if len(steady) > 1:
        cpu_percent = 100 * (steady[-1][1] - steady[0][1]) / (steady[-1][0] - steady[0][0])
    evidence.update(status=status, process_exit=process.returncode, forced_stop=forced_stop,
                    result_path=str(target / 'result.json'),
                    elapsed_seconds=round(time.monotonic() - start, 3),
                    result_line=result[-1] if result else None, pc_values=len(pcs),
                    renderer_faults=renderer_faults,
                    perf_lines=frame_rates, screenshots=captures,
                    peak_sampled_rss_bytes=max((row[2] for row in samples), default=0),
                    cpu_percent_one_core=cpu_percent,
                    cpu_percent_machine=None if cpu_percent is None else cpu_percent / (os.cpu_count() or 1),
                    performance_scope='boot/menu only; no gameplay verification; competing builds may run')
    atomic_json(target / 'process-samples.json', dict(
        columns=['elapsed_seconds', 'process_cpu_seconds', 'rss_bytes'], samples=samples))
    evidence['process_samples'] = str(target / 'process-samples.json')
    if input_driver is not None:
        evidence.update(input_sent=bool(input_driver.actions),
                        input_actions=str(target / 'input-actions.json'),
                        input_action_count=len(input_driver.actions),
                        launcher_card_copy=input_driver.card_seed,
                        private_display=input_driver.env['DISPLAY'],
                        performance_scope='scripted private-session observation; scene must be reviewed before gameplay/performance claims')
    atomic_json(target / 'result.json', evidence)
    print(json.dumps({'game_id': metadata['game_id'], 'status': status,
                      'result': str(target / 'result.json')}), flush=True)
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--metadata', type=Path, required=True)
    parser.add_argument('--results', type=Path, default=ROOT / '.gekkoaot/compat/boot-v233')
    parser.add_argument('--seconds', type=int, default=40)
    parser.add_argument('--runner', type=Path, default=ROOT / 'bin/gekkoaot-native-run')
    parser.add_argument('--isolated-input', action='store_true',
                        help='connected neutral pad, unaffected by typing/gamepads during automated observations')
    args = parser.parse_args()
    if args.seconds < 20:
        parser.error('--seconds must be at least 20')
    return 0 if run(args.metadata, args.results.resolve(), args.seconds, args.runner,
                   isolated_input=args.isolated_input)['status'] == 'boot-progress-observed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
