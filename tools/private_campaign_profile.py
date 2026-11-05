#!/usr/bin/env python3
"""Collect bounded perf samples only from runners in a named private campaign."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time
import re
import shutil


def cpu_observation(pid):
    def ticks(path):
        # comm can contain spaces and parentheses; fields after its final ')'
        # start at stat field 3. CPU accounting includes user + kernel time.
        fields = path.read_text().rsplit(')', 1)[1].split()
        return int(fields[11]) + int(fields[12])
    proc = Path(f'/proc/{pid}')
    return dict(monotonic=time.monotonic(), process_ticks=ticks(proc/'stat'),
                main_ticks=ticks(proc/'task'/str(pid)/'stat'),
                rss_bytes=int((proc/'statm').read_text().split()[1])*os.sysconf('SC_PAGE_SIZE'),
                affinity=sorted(os.sched_getaffinity(pid)))


def cpu_comparison(before, after, frame_log):
    elapsed = after['monotonic'] - before['monotonic']
    hz = os.sysconf('SC_CLK_TCK')
    result = dict(elapsed_seconds=elapsed, rss_before_bytes=before['rss_bytes'],
                  rss_after_bytes=after['rss_bytes'], affinity_before=before['affinity'],
                  affinity_after=after['affinity'], host_logical_cpus=os.cpu_count())
    for scope in ('process', 'main'):
        seconds = (after[scope+'_ticks']-before[scope+'_ticks'])/hz
        result[scope+'_cpu_seconds'] = seconds
        result[scope+'_cpu_percent_one_core'] = seconds/elapsed*100
    result['process_cpu_percent_machine'] = result['process_cpu_percent_one_core']/os.cpu_count()
    values = [float(x) for x in re.findall(r'guest_fps=([0-9.]+)', frame_log)]
    result['guest_fps_samples'] = values
    if values:
        result['guest_fps_mean'] = sum(values)/len(values)
        result['guest_fps_min'] = min(values)
        result['guest_fps_max'] = max(values)
    result['note'] = 'runner CPU only; compositor/GPU work excluded; frame counts are EFB copies, not a simulation speed guarantee'
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--campaign', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--games', required=True)
    parser.add_argument('--stage', choices=('gameplay', 'boot'), default='gameplay',
                        help='gameplay requires a reviewed gameplay-proof.json in the boot directory')
    args = parser.parse_args()
    args.campaign = args.campaign.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    if os.geteuid() == 0:
        os.chown(args.output, int(os.environ.get('SUDO_UID', os.getuid())),
                 int(os.environ.get('SUDO_GID', os.getgid())))
    wanted = set(args.games.split(','))
    done = set()
    startup_deadline = time.monotonic() + 30
    while True:
        status_path = args.campaign / 'status.json'
        if not status_path.exists():
            if time.monotonic() >= startup_deadline:
                raise RuntimeError('private campaign did not publish its status within 30 seconds')
            time.sleep(0.25)
            continue
        status = json.loads(status_path.read_text())
        if status['phase'] != 'running':
            return 0
        for entry in status['games']:
            gid = entry['game_id']
            if gid not in wanted or gid in done or entry['stage'] != 'observing':
                continue
            for pidfile in (args.campaign / 'boots').glob(gid + '-*/*/pid.json'):
                if (pidfile.parent / 'result.json').exists():
                    continue
                observed = json.loads(pidfile.read_text())
                if time.time() - observed['started'] < 35:
                    continue
                proof = None
                if args.stage == 'gameplay':
                    proof_file = pidfile.parent / 'gameplay-proof.json'
                    if not proof_file.exists():
                        continue
                    proof = json.loads(proof_file.read_text())
                    capture = (pidfile.parent / proof['capture']).resolve()
                    if (proof.get('stage') != 'gameplay' or not proof.get('reviewed')
                            or capture.parent != pidfile.parent or not capture.is_file()):
                        raise ValueError('gameplay proof must name a reviewed local capture')
                pid = observed['pid']
                try:
                    cmd = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
                    env = Path(f'/proc/{pid}/environ').read_bytes().split(b'\0')
                except OSError:
                    continue
                stop = os.fsencode(str(pidfile.parent / 'stop.request'))
                if (Path(os.fsdecode(cmd[0])).name != 'gekkoaot-native-run'
                        or stop not in cmd
                        or os.fsencode('DISPLAY=' + status['display']) not in env):
                    raise RuntimeError('profiling target failed private runner identity check')
                data = args.output / (gid + '.data')
                command = ['perf', 'record', '-F', '199', '--call-graph', 'dwarf,8192',
                           '-e', 'cycles:u', '-p', str(pid), '-o', str(data), '--', 'sleep', '15']
                print('Profiling private', gid, pid, flush=True)
                start = time.time()
                before = cpu_observation(pid)
                # Native text can live on anonymous huge pages (v326). Keep
                # the owned runner's symbol map beside its profile so the
                # hotspots remain identifiable after the process has exited.
                symbol_map = Path('/tmp') / ('perf-' + str(pid) + '.map')
                if symbol_map.is_file():
                    shutil.copyfile(symbol_map, args.output / (gid + '.perf-map'))
                run_log = pidfile.parent/'run.log'
                log_offset = run_log.stat().st_size
                with (args.output / (gid + '.perf.log')).open('w') as log:
                    result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=25)
                accounting = None
                try:
                    after = cpu_observation(pid)
                    with run_log.open() as log:
                        log.seek(log_offset)
                        frame_log = log.read()
                    accounting = cpu_comparison(before, after, frame_log)
                except OSError:
                    # Exiting during a capture never counts as a CPU win.
                    accounting = dict(incomplete=True, reason='runner exited during measurement')
                record = dict(game_id=gid, pid=pid, result_directory=str(pidfile.parent),
                              command=command, started=start, finished=time.time(),
                              exit_code=result.returncode,
                              stage=args.stage, visual_proof=proof, cpu_accounting=accounting,
                              scope='perf adds sampling overhead; boot samples do not certify gameplay')
                (args.output / (gid + '.json')).write_text(json.dumps(record, indent=2) + '\n')
                # Root perf permission is local to this process; do not change
                # system perf policy. Evidence remains readable by the user.
                uid, group = int(os.environ.get('SUDO_UID', os.getuid())), int(os.environ.get('SUDO_GID', os.getgid()))
                if os.geteuid() == 0:
                    for path in args.output.glob(gid + '.*'):
                        os.chown(path, uid, group)
                done.add(gid)
                print('Recorded', gid, result.returncode, flush=True)
        if wanted <= done:
            return 0
        time.sleep(2)


if __name__ == '__main__':
    raise SystemExit(main())
