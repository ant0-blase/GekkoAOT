#!/usr/bin/env python3
"""Run J/Return game observations inside an invisible Gamescope/Xwayland session.

Only exact, source-validated AOT selections are used. Screenshots require human
review; button presses or FPS never certify gameplay. Touch a game's stop-input
file to end its key sequence, or the campaign's stop.request to stop the queue.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import re
import subprocess
import sys
import time

import corpus_boot as boot
from corpus_campaign import find_selection


def require_private_session():
    display = os.environ.get('DISPLAY', '')
    if not display or display == os.environ.get('GEKKOAOT_TEST_DESKTOP_DISPLAY'):
        raise RuntimeError('refusing keys on the desktop display')
    pid = os.getppid()
    for _ in range(12):
        try:
            cmd = Path(f'/proc/{pid}/cmdline').read_bytes().split(b'\0')
            if cmd and Path(os.fsdecode(cmd[0])).name == 'gamescope':
                backend = cmd.index(b'--backend') if b'--backend' in cmd else -1
                if backend >= 0 and cmd[backend + 1] == b'headless':
                    return pid
                break
            stat = Path(f'/proc/{pid}/stat').read_text().rsplit(') ', 1)[1].split()
            pid = int(stat[1])
            if pid <= 1:
                break
        except (OSError, ValueError, IndexError):
            break
    raise RuntimeError('input automation requires an owned headless Gamescope ancestor')


class PrivateKeys:
    def __init__(self, limit, launcher_users, trace_pc=False, keys=("j", "Return"), input_start=3.0):
        require_private_session()
        self.env = dict(os.environ)
        self.limit = limit
        self.actions = []
        self.keys = keys
        self.next_key = input_start
        self.pause_until = 0.0
        self.window = None
        self.target = None
        self.launcher_users = launcher_users.resolve()
        self.card_seed = []
        self.trace_pc = trace_pc

    def command(self, *args, check=True):
        result = subprocess.run(['xdotool', *args], env=self.env,
                                capture_output=True, text=True, timeout=3)
        if check and result.returncode:
            raise RuntimeError('private X11 input command failed: ' + result.stderr[-300:])
        return result

    def prepare(self, target, env, metadata):
        self.target = target
        self.actions = []
        # Same filename slug as MainWindow::frontendUserDir. Copy bytes rather
        # than linking: test saves must never reach the launcher's originals.
        slug = re.sub('[^A-Za-z0-9]+', '-', Path(metadata['source']).stem).strip('-')
        source_root = self.launcher_users / slug
        test_root = Path(env['GEKKOAOT_USER_DIR'])
        for relative in ('cards/MemoryCardA.raw', 'cards/MemoryCardB.raw', 'SRAM.raw'):
            source = source_root / relative
            if not source.is_file():
                continue
            for attempt in range(3):
                before = source.stat()
                data = source.read_bytes()
                after = source.stat()
                if (before.st_ino, before.st_size, before.st_mtime_ns) == (
                        after.st_ino, after.st_size, after.st_mtime_ns):
                    break
            else:
                raise RuntimeError('launcher save changed during snapshot: ' + str(source))
            destination = test_root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
            import hashlib
            self.card_seed.append(dict(source=str(source), destination=str(destination),
                                       sha256=hashlib.sha256(data).hexdigest(), bytes=len(data)))
        boot.atomic_json(target / 'launcher-card-copy.json', self.card_seed)
        config = target / 'private-keyboard.ini'
        config.write_text('[port1]\nsource=keyboard\na_key=J\nb_key=K\nx_key=U\ny_key=I\n'
                          'start_key=Return\nz_key=R\nl_key=Q\nr_key=E\n'
                          'dpad_up_key=T\ndpad_down_key=G\ndpad_left_key=F\ndpad_right_key=H\n'
                          'main_x_negative_key=A\nmain_x_positive_key=D\n'
                          'main_y_negative_key=S\nmain_y_positive_key=W\n'
                          'c_x_negative_key=Left\nc_x_positive_key=Right\n'
                          'c_y_negative_key=Down\nc_y_positive_key=Up\n'
                          '[port2]\nsource=disabled\n[port3]\nsource=disabled\n'
                          '[port4]\nsource=disabled\n')
        env['GEKKOAOT_INPUT_CONFIG'] = str(config)
        env['SDL_AUDIODRIVER'] = 'dummy'
        # Diagnostic per-25K-dispatch logs distort runtime profiling. The
        # default private pass uses FPS and captures, leaving PC traces opt-in.
        env['GEKKOAOT_NATIVE_PC_TRACE'] = '1' if self.trace_pc else '0'
        boot.atomic_json(target / 'input-actions.json', self.actions)

    def after_capture(self, elapsed):
        # Leave a stable screen for review between short input bursts.
        self.pause_until = elapsed + 3

    def capture(self, destination):
        # Vulkan WSI bypasses the X11 pixmap; import -window would show black.
        # Ask our compositor for its composed GPU image, using its own socket.
        socket = self.env.get('GAMESCOPE_WAYLAND_DISPLAY')
        if not socket:
            raise RuntimeError('private compositor socket is missing')
        env = dict(self.env, WAYLAND_DISPLAY=socket)
        subprocess.run(['gamescopectl', 'screenshot', str(destination)], env=env,
                       capture_output=True, check=True, timeout=3)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if destination.is_file() and destination.stat().st_size:
                return True
            time.sleep(0.05)
        return False

    def tick(self, pid, elapsed):
        if (elapsed >= self.limit or elapsed < max(self.next_key, self.pause_until)
                or (self.target / 'stop-input').exists()
                or (self.target / 'stop.request').exists()):
            return
        windows = self.command('search', '--onlyvisible', '--pid', str(pid), check=False)
        candidates = [w for w in windows.stdout.splitlines() if w.isdecimal()]
        if not candidates:
            self.next_key = elapsed + 1
            return
        self.window = candidates[0]
        self.command('windowfocus', '--sync', self.window)
        focused = self.command('getwindowfocus', 'getwindowpid').stdout.strip()
        if focused != str(pid):
            raise RuntimeError('private input target PID does not match the game')
        key = self.keys[len(self.actions) % len(self.keys)]
        try:
            self.command('keydown', key)
            time.sleep(0.15)
        finally:
            self.command('keyup', key, check=False)
        self.actions.append({'elapsed_seconds': round(elapsed, 3), 'key': key,
                             'hold_seconds': 0.15, 'display': self.env['DISPLAY'],
                             'window': self.window, 'pid': pid})
        boot.atomic_json(self.target / 'input-actions.json', self.actions)
        self.next_key = elapsed + 0.8

    def finish(self):
        if self.target is not None:
            self.command('keyup', *self.keys, check=False)
            boot.atomic_json(self.target / 'input-actions.json', self.actions)


def observe(args):
    compositor = require_private_session()
    rows = json.loads(args.corpus.read_text())['images']
    rows = [r for r in rows if r['kind'] == 'disc' and not r.get('duplicate_of')]
    if args.only:
        identifiers = args.only.split(',')
        rows = sorted([r for r in rows if r['game_id'] in identifiers],
                      key=lambda r: identifiers.index(r['game_id']))
    overrides = {}
    for selection in args.selection:
        value = json.loads(selection.read_text())
        boot.validate_launch(value)
        overrides[value['game_id']] = selection.resolve()
    status = dict(schema=1, pid=os.getpid(), compositor_pid=compositor,
                  display=os.environ['DISPLAY'], backend='gamescope-headless',
                  started=time.time(), phase='running', games=[],
                  user_cards_touched=False, desktop_input_sent=False)
    def save():
        boot.atomic_json(args.results / 'status.json', status)
    save()
    for row in rows:
        if (args.results / 'stop.request').exists():
            status['phase'] = 'stopped-by-request'
            break
        entry = dict(game_id=row['game_id'], name=row['name'], stage='preparing')
        status['games'].append(entry)
        save()
        try:
            selection = overrides.get(row['game_id']) or find_selection(row)
            if selection is None:
                entry.update(stage='needs-exact-launch-selection')
            else:
                value = json.loads(selection.read_text())
                if (Path(value['source']).resolve() != Path(row['source']).resolve()
                        or boot.sha256(value['dol']) != row['main_dol_sha256']):
                    raise ValueError('selection does not match corpus image')
                entry.update(stage='observing', selection=str(selection))
                save()
                evidence = boot.run(selection, args.results / 'boots', args.seconds,
                                    runner=args.runner, input_driver=PrivateKeys(
                                        args.input_seconds, args.launcher_users, args.pc_trace,
                                        keys=args.keys.split(','), input_start=args.input_start))
                entry.update(stage=evidence['status'], result=evidence['result_path'],
                             input_actions=evidence['input_actions'], validation=evidence['validation'])
        except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
            entry.update(stage='launch-or-input-failure', error=str(error))
            print(row['game_id'], str(error), flush=True)
        save()
    if status['phase'] == 'running':
        status['phase'] = 'observations-finished'
    status['finished'] = time.time()
    save()
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--corpus', type=Path, default=boot.ROOT / '.gekkoaot/compat/corpus-v232/corpus.json')
    parser.add_argument('--results', type=Path, required=True)
    parser.add_argument('--only', help='ordered comma-separated game IDs; default: every distinct disc')
    parser.add_argument('--selection', type=Path, action='append', default=[])
    parser.add_argument('--after-status', type=Path, help='start after another private campaign finishes')
    parser.add_argument('--launcher-users', type=Path, default=Path(os.environ.get(
        'GEKKOAOT_STATE_DIR', str(boot.ROOT / '.gekkoaot'))) / 'user',
        help='launcher per-game user root; cards and SRAM are copied into each test')
    parser.add_argument('--seconds', type=int, default=100)
    parser.add_argument('--runner', type=Path, default=boot.ROOT / 'bin/gekkoaot-native-run')
    parser.add_argument('--pc-trace', action='store_true', help='enable expensive diagnostic dispatch traces')
    parser.add_argument('--input-seconds', type=int, default=70)
    parser.add_argument('--keys', default='j,Return', help='comma-separated private navigation keys')
    parser.add_argument('--input-start', type=float, default=3.0, help='first key time in seconds')
    parser.add_argument('--inside', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.seconds < 20 or not 0 < args.input_seconds <= args.seconds:
        parser.error('require seconds >= 20 and 0 < input-seconds <= seconds')
    if args.input_start < 0 or not args.keys or any(key not in
            ('j', 'Return', 'k', 'u', 'i', 't', 'g', 'f', 'h', 'w', 'a', 's', 'd')
            for key in args.keys.split(',')):
        parser.error('invalid private navigation keys or start time')
    args.results = args.results.resolve()
    args.results.mkdir(parents=True, exist_ok=True)
    if args.inside:
        return observe(args)
    if args.after_status:
        while True:
            if (args.results / 'stop.request').exists():
                return 0
            prior = json.loads(args.after_status.read_text())
            if prior['phase'] == 'observations-finished':
                break
            if prior['phase'] == 'stopped-by-request':
                raise RuntimeError('prior campaign was stopped; continuation cancelled')
            os.kill(prior['pid'], 0)
            time.sleep(5)
    env = dict(os.environ, GEKKOAOT_TEST_DESKTOP_DISPLAY=os.environ.get('DISPLAY', ''),
               SDL_AUDIODRIVER='dummy')
    for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'XAUTHORITY', 'GEKKOAOT_INPUT_CONFIG'):
        env.pop(key, None)
    command = ['gamescope', '--backend', 'headless', '-W', '1920', '-H', '1080',
               '-w', '1920', '-h', '1080', '-r', '60', '--', sys.executable,
               str(Path(__file__).resolve()), '--inside', *sys.argv[1:]]
    with (args.results / 'compositor.log').open('w') as log:
        process = subprocess.Popen(command, cwd=boot.ROOT, env=env, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        boot.atomic_json(args.results / 'supervisor.json', dict(pid=os.getpid(),
                         compositor_pid=process.pid, command=command, started=time.time()))
        try:
            return process.wait()
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


if __name__ == '__main__':
    raise SystemExit(main())
