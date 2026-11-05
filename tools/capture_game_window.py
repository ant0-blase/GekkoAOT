#!/usr/bin/env python3
"""Bounded, timestamped X11 game-window captures for local visual regression.

Example: python3 tools/capture_game_window.py --pid 1234 --output captures --seconds 600
Captures are evidence only; this tool never marks gameplay or image quality verified.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', required=True, type=int)
    parser.add_argument('--window', type=int, help='Known embedded game window ID (for Qt launchers)')
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--seconds', type=float, default=600)
    parser.add_argument('--interval', type=float, default=1)
    parser.add_argument('--retain', type=int, default=120, help='Rolling images plus first baseline')
    args = parser.parse_args()
    if args.pid <= 0 or args.seconds <= 0 or args.interval <= 0 or args.retain < 1 or (args.window is not None and args.window<=0):
        parser.error('PID, duration, interval and retention must be positive')
    for tool in ('xdotool', 'xwd', 'magick'):
        if not shutil.which(tool):
            parser.error(f'X11 capture requires {tool}')
    args.output.mkdir(parents=True, exist_ok=True)
    # Each invocation has its own directory; never overwrite earlier evidence.
    output = args.output / (time.strftime('%Y%m%dT%H%M%S') + '-' + str(os.getpid()))
    output.mkdir()
    start = time.monotonic()
    index = 0
    saved = []
    while time.monotonic() - start < args.seconds:
        try:
            os.kill(args.pid, 0)
        except ProcessLookupError:
            break
        if args.window is not None:
            windows=[str(args.window)]
        else:
            result = subprocess.run(['xdotool', 'search', '--onlyvisible', '--pid', str(args.pid)], capture_output=True, text=True, timeout=5)
            windows = [x for x in result.stdout.splitlines() if re.fullmatch(r'\d+', x)]
        row = dict(index=index, time=time.time(), elapsed=time.monotonic()-start, pid=args.pid)
        raw, pending = output/'capture.xwd', output/'capture.pending.png'
        try:
            if windows:
                row['window'] = windows[0]
                result = subprocess.run(['xwd', '-id', windows[0], '-silent', '-out', str(raw)], capture_output=True, timeout=5)
                if result.returncode == 0:
                    result = subprocess.run(['magick', str(raw), '-define', 'png:compression-level=1', str(pending)], capture_output=True, timeout=10)
                    if result.returncode == 0:
                        png = output / f'frame-{index:06d}.png'
                        pending.replace(png)
                        row['file'] = png.name
                        saved.append(png)
                        if len(saved) > args.retain + 1:
                            saved.pop(1).unlink(missing_ok=True)
                if result.returncode:
                    row['error'] = result.stderr.decode(errors='replace')[-500:]
            else:
                row['error'] = 'no_visible_window'
        except subprocess.TimeoutExpired:
            row['error'] = 'capture_timeout'
        finally:
            raw.unlink(missing_ok=True)
            pending.unlink(missing_ok=True)
        row['capture_seconds'] = time.monotonic()-start-row['elapsed']
        with (output/'frames.jsonl').open('a', encoding='utf-8') as log:
            log.write(json.dumps(row, ensure_ascii=False)+'\n')
        index += 1
        # Do not run catch-up bursts after an expensive/blocked capture.
        time.sleep(max(0, args.interval-(time.monotonic()-start-row['elapsed'])))
    print(output)


if __name__ == '__main__':
    main()
