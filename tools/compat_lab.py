#!/usr/bin/env python3
"""Repeatable corpus laboratory. Boot evidence never implies gameplay validation."""
import argparse
import csv
import hashlib
import html
import json
import os
import platform
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess
import sys
import time
import uuid

import corpus_boot as boot

ROOT = Path(__file__).resolve().parents[1]
# Disc collection: --corpus, else GEKKOAOT_GAME_CORPUS, else <repo>/games.
CORPUS = Path(os.environ.get('GEKKOAOT_GAME_CORPUS') or ROOT / 'games')
STATUSES = ('NOT_TESTED', 'COMPILE_FAILED', 'BOOT_FAILED', 'BOOT_OK', 'MENU_OK',
            'GAMEPLAY_VERIFIED', 'CRASH', 'GRAPHICS_BROKEN', 'AUDIO_BROKEN', 'UNSUPPORTED_INPUT')
PROFILES = {
    'quick': ('super monkey ball', 'frontline', "luigi", 'sonic adventure 2'),
    'graphics': ('super monkey ball', 'frontline', 'rising sun', 'harry potter',
                 'luigi', 'auto modellista', 'rogue squadron', 'star fox', 'zelda', 'metroid', 'sonic', 'mario'),
}
SUPPORTED = {'.rvz', '.iso', '.gcm'}
OTHER_INPUTS = {'.wia', '.wbfs', '.nkit', '.dol', '.elf', '.ciso', '.gcz', '.zip', '.7z', '.rar'}


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def discover(directory):
    """No symlink traversal; extracted roots own their children (no duplicate DOLs)."""
    directory = directory.resolve()
    rows = []
    for current, dirs, files in os.walk(directory, followlinks=False):
        current = Path(current)
        dirs[:] = sorted(d for d in dirs if not (current / d).is_symlink())
        extracted = (current / 'sys/main.dol').is_file() and (current / 'sys/boot.bin').is_file()
        if extracted:
            candidates = [(current, 'extracted', True)]
            dirs[:] = []
        else:
            candidates = [(current / f, 'disc' if Path(f).suffix.lower() in SUPPORTED else 'resource',
                           Path(f).suffix.lower() in SUPPORTED)
                          for f in sorted(files) if Path(f).suffix.lower() in SUPPORTED | OTHER_INPUTS
                          and not (current / f).is_symlink()]
        for source, kind, supported in candidates:
            relative = str(source.relative_to(directory))
            rows.append(dict(id=digest(relative)[:16], name=source.stem, source=str(source),
                             relative=relative, kind=kind, supported=supported,
                             status='NOT_TESTED' if supported else 'UNSUPPORTED_INPUT',
                             reason='' if supported else 'Supplementary or unsupported format; not a game failure'))
    return sorted(rows, key=lambda r: r['relative'].casefold())


def source_identity(row):
    path = Path(row['source'])
    paths = sorted(p for p in path.rglob('*') if p.is_file()) if path.is_dir() else [path]
    # Stat identity is deliberately labelled; it avoids hashing terabytes on every invocation.
    return {'method': 'path-size-mtime_ns', 'digest': digest([
        (str(p.relative_to(path)) if path.is_dir() else p.name,
         p.stat().st_size, p.stat().st_mtime_ns) for p in paths])}


def prepare_extracted(source, destination):
    """Build a private sparse GCM from an extracted tree and its authoritative FST.

    Offsets are relocated, never guessed from directory ordering. Original bytes
    remain untouched; the normal controller and nod consume the private image.
    """
    source, destination = Path(source), Path(destination)
    system = source / 'sys'
    boot_data = bytearray((system / 'boot.bin').read_bytes())
    fst = bytearray((system / 'fst.bin').read_bytes())
    dol = (system / 'main.dol').read_bytes()
    if len(boot_data) != 0x440 or len(fst) < 12:
        raise ValueError('invalid extracted boot/FST size')
    count = struct.unpack_from('>I', fst, 8)[0]
    if count < 1 or count * 12 > len(fst):
        raise ValueError('invalid extracted FST entry count')
    files = []
    parents = [(count, Path())]
    for index in range(1, count):
        while parents and index >= parents[-1][0]:
            parents.pop()
        if not parents:
            raise ValueError('invalid FST directory extent')
        name_word, offset, size = struct.unpack_from('>III', fst, index * 12)
        name_at = count * 12 + (name_word & 0xffffff)
        end = fst.find(b'\0', name_at)
        if name_at >= len(fst) or end < 0:
            raise ValueError('invalid FST string')
        name = fst[name_at:end].decode('utf-8')
        if name in ('', '.', '..') or '/' in name or '\\' in name:
            raise ValueError('unsafe FST path')
        relative = parents[-1][1] / name
        if name_word >> 24:
            if not index < size <= parents[-1][0]:
                raise ValueError('invalid FST subtree')
            parents.append((size, relative))
        else:
            file = source / 'files' / relative
            if not file.is_file() or file.stat().st_size != size or not file.resolve().is_relative_to(source.resolve()):
                raise ValueError('missing/invalid extracted file: ' + str(relative))
            files.append((index, file, size))
    align = lambda n: (n + 31) & ~31
    apploader = (system / 'apploader.img').read_bytes() if (system / 'apploader.img').exists() else b''
    dol_offset = align(max(0x2440 + len(apploader), 0x3000))
    fst_offset = align(dol_offset + len(dol))
    cursor = align(fst_offset + len(fst))
    placements = []
    for index, file, size in files:
        struct.pack_into('>I', fst, index * 12 + 4, cursor)
        placements.append((cursor, file))
        cursor = align(cursor + size)
    struct.pack_into('>IIII', boot_data, 0x420, dol_offset, fst_offset, len(fst), len(fst))
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix('.tmp')
    try:
        with temporary.open('wb') as output:
            output.write(boot_data)
            bi2 = (system / 'bi2.bin').read_bytes() if (system / 'bi2.bin').exists() else bytes(0x2000)
            if len(bi2) != 0x2000:
                raise ValueError('invalid BI2 size')
            output.write(bi2)
            output.write(apploader)
            output.seek(dol_offset)
            output.write(dol)
            output.seek(fst_offset)
            output.write(fst)
            for offset, file in placements:
                output.seek(offset)
                with file.open('rb') as stream:
                    shutil.copyfileobj(stream, output)
            output.truncate(max(cursor, 0x50000))
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    return destination


def component_identity():
    paths = []
    for directory in ('runtime', 'tools', 'patches', 'src', 'tests'):
        paths.extend(p for p in (ROOT / directory).rglob('*') if p.is_file()
                     and p.suffix in {'.c', '.cpp', '.h', '.hpp', '.inc', '.def', '.py', '.patch', '.txt'}
                     and '__pycache__' not in p.parts)
    paths.extend(p for p in (ROOT / 'CMakeLists.txt', ROOT / 'version.txt') if p.is_file())
    return digest([(str(p.relative_to(ROOT)), boot.sha256(p)) for p in sorted(paths)])


def executable(name):
    suffix = '.exe' if os.name == 'nt' else ''
    return next((p for p in (ROOT / 'build/bin' / (name + suffix), ROOT / 'bin' / (name + suffix),
                             ROOT.parent.parent / 'bin' / (name + suffix))
                 if p.is_file()), ROOT / 'build/bin' / (name + suffix))


def terminate(process):
    """Stop only our process group, including compiler/compositor descendants."""
    if os.name == 'nt':
        subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'], capture_output=True)
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
            until = time.monotonic() + 5
            while process.poll() is None and time.monotonic() < until:
                time.sleep(.1)
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    if process.poll() is None:
        process.wait(timeout=10)


def bounded(command, env, log, timeout):
    kwargs = ({'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == 'nt'
              else {'start_new_session': True})
    with log.open('w') as output:
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=output,
                                   stderr=subprocess.STDOUT, **kwargs)
        try:
            return {'exit_code': process.wait(timeout=timeout), 'timed_out': False}
        except subprocess.TimeoutExpired:
            return {'exit_code': None, 'timed_out': True}
        finally:
            # Also reap descendants left behind after the group leader exits.
            terminate(process)


def selection(source):
    candidates = []
    state = Path(os.environ.get('GEKKOAOT_STATE_DIR', ROOT / '.gekkoaot'))
    for path in (state / 'cache/discs').glob('*/meta/aot-launch.json'):
        try:
            metadata = json.loads(path.read_text())
            if Path(metadata['source']).resolve() == Path(source).resolve():
                boot.validate_launch(metadata)
                candidates.append(path)
        except (OSError, ValueError, KeyError):
            continue
    return max(candidates, key=lambda p: p.stat().st_mtime_ns) if candidates else None


def parse_diagnostics(text):
    markers = {}
    # Keep the last cumulative snapshot, not a sum of repeated lifetime totals.
    for line in text.splitlines():
        if line.startswith('GEKKOAOT_'):
            marker = line.split()[0].split('=')[0]
            if any(tag in marker for tag in ('STATS', 'COVERAGE', 'EVIDENCE', 'PERF_', 'SDK_DIRECT', 'IR_STATS')):
                key = marker
                name = re.search(r'\bname=(\S+)', line)
                if name:
                    key += ':' + name[1]
                markers[key] = line
    def values(field):
        return [float(v) for v in re.findall(r'\b' + field + r'=(\d+(?:\.\d+)?)', '\n'.join(markers.values()))]
    faults, teardown_events = [], []
    stopping = False
    for line in text.splitlines():
        if 'GEKKOAOT_GRACEFUL_STOP_V20=1' in line:
            stopping = True
        if re.search(r'GEKKOAOT_(?:NATIVE_GX_(?:FIFO|DL)_ERROR|ENHANCED_INIT_ERROR)|(?:Validation Error|DeviceLost|device lost|out of memory)', line, re.I):
            if stopping and line.strip() == '[warning] [aurora::gpu] Device lost: Device was destroyed.':
                teardown_events.append(line)
            else:
                faults.append(line)
    enhanced_gpu = []
    for line in text.splitlines():
        if line.startswith('GEKKOAOT_ENHANCED_MEASUREMENT ') and 'timings_valid=1' in line:
            found = re.search(r'\bgpu_ns=([0-9,]+)', line)
            if found:
                enhanced_gpu.append([int(x) for x in found[1].split(',')])
    fps = {}
    for field in ('vps', 'guest_fps', 'present_fps', 'interp_fps'):
        data = [float(v) for v in re.findall(r'\b' + field + r'=([0-9.]+)', text)]
        fps[field] = {'samples': len(data), 'mean': sum(data)/len(data), 'min': min(data), 'max': max(data)} if data else None
    gpu = any(v > 0 for field in ('wsi_presents', 'executed_draws', 'draws_3d', 'graph_copies') for v in values(field))
    gpu = gpu or bool(fps['present_fps'] and fps['present_fps']['max'] > 0)
    loaded = 'GEKKOAOT_NATIVE_DOL_LOADED' in text
    pcs = set(re.findall(r'GEKKOAOT_NATIVE_PC_TRACE_V\d+=1[^\n]*?\bpc=([0-9a-f]+)', text))
    progress = loaded and (len(pcs) > 2 or bool(fps['guest_fps'] and fps['guest_fps']['max'] > 0))
    audio_error = bool(re.search(r'GEKKOAOT_NATIVE_AUDIO_SDL3(?:_PUT)?=0', text))
    return dict(boot_progress=progress, gpu_activity=gpu, menu='UNVERIFIED', gameplay='UNVERIFIED',
                audio_initialization='FAILED' if audio_error else 'INITIALIZED' if 'GEKKOAOT_NATIVE_AUDIO_SDL3=1' in text else 'UNVERIFIED',
                audio_quality='UNVERIFIED', gpu_execution_ms=None, frame_counters=fps,
                gpu_errors=faults, teardown_events=teardown_events, nativegx_markers=markers,
                enhanced_gpu_pass_ns=enhanced_gpu or None,
                enhanced_gpu_scope='Instrumented Enhanced Renderer passes only; not total frame GPU duration',
                performance_scope='Process CPU and runtime counters; guest/EFB/present rates are distinct; no gameplay or GPU-time inference')


def outcome(diag, code, timed_out=False, result_line=''):
    if code is not None and (code < 0 or code > 128):
        return 'CRASH'
    if diag['gpu_errors']:
        return 'GRAPHICS_BROKEN'
    if diag['audio_initialization'] == 'FAILED':
        return 'AUDIO_BROKEN'
    if timed_out or code != 0 or 'status="host requested exit"' not in result_line:
        return 'BOOT_FAILED'
    return 'BOOT_OK' if diag['boot_progress'] else 'BOOT_FAILED'


def evidence_unchanged(row):
    """Result caching must also retain the exact binaries and evidence it tested."""
    try:
        evidence = row['evidence']
        if not row.get('retained_sha256') or not all(boot.sha256(p) == h for p, h in row['retained_sha256'].items()):
            return False
        paths = dict(evidence['module_sha256'])
        paths[evidence['command'][0]] = evidence['runner_sha256']
        metadata = json.loads(Path(evidence['selection']).read_text())
        boot.validate_launch(metadata)
        paths[metadata['bridge']] = evidence['bridge_sha256']
        if metadata['module'] not in paths:
            return False
        if not Path(evidence['result_path']).is_file():
            return False
        return all(boot.sha256(path) == expected for path, expected in paths.items())
    except (OSError, KeyError, ValueError):
        return False


def gameplay_acceptance(row):
    """Never upgrade boot, enabled settings or a contributing graph into playability."""
    diag = row.get('diagnostics', {})
    measurement = diag.get('enhanced_measurement', {}).get('graph_measurement', {})
    gameplay = row.get('status') == 'GAMEPLAY_VERIFIED' and diag.get('gameplay') == 'EXPLICITLY_REVIEWED'
    contribution = measurement.get('presented_changed_samples', 0) > 0
    review = row.get('review', {}).get('enhanced', {})
    visual = review.get('outcome') == 'VALID_EFFECT' and bool(review.get('rationale'))
    enhanced = contribution and visual
    blockers = []
    if not gameplay: blockers.append('Gameplay/control scene not explicitly verified')
    if not contribution: blockers.append('No measured Enhanced contribution on a presented graph')
    if not visual: blockers.append('Enhanced visual correctness not reviewed')
    return dict(gameplay_verified=gameplay, enhanced_verified=enhanced,
                playable_with_enhanced=gameplay and enhanced, blockers=blockers)


def write_reports(directory, rows, context):
    for row in rows:
        row['acceptance'] = gameplay_acceptance(row)
    boot.atomic_json(directory / 'results.json', dict(schema=1, context=context, games=rows))
    fields = ('id', 'name', 'source', 'kind', 'status', 'reason', 'cached', 'artifact_dir',
              'cpu_percent_one_core', 'gpu_activity', 'audio_initialization', 'menu', 'gameplay')
    with (directory / 'results.csv').open('w', newline='') as output:
        writer = csv.DictWriter(output, fieldnames=fields, extrasaction='ignore')
        writer.writeheader()
        writer.writerows(dict(r, **{k: r.get('diagnostics', {}).get(k) for k in fields[-5:]}) for r in rows)
    counts = {s: sum(r['status'] == s for r in rows) for s in STATUSES}
    body = ''
    for row in rows:
        diag = row.get('diagnostics', {})
        cells = [row.get(k, '') for k in ('name', 'kind', 'status', 'reason')]
        cells.extend(diag.get(k, 'UNMEASURED') for k in ('cpu_percent_one_core', 'gpu_activity', 'audio_initialization'))
        body += '<tr>' + ''.join('<td>' + html.escape(str(v)) + '</td>' for v in cells)
        artifact = row.get('artifact_dir')
        if artifact:
            relative = os.path.relpath(artifact, directory)
            body += '<td><a href="' + html.escape(relative, quote=True) + '/">Logs / captures</a></td>'
        else:
            body += '<td></td>'
        body += '</tr>'
    page = '''<!doctype html><html lang="en"><meta charset="utf-8"><title>GekkoAOT Compatibility Lab</title>
<style>body{font:15px system-ui;margin:2rem;background:#111827;color:#e5e7eb}table{border-collapse:collapse;width:100%}td,th{padding:.6rem;border:1px solid #374151;text-align:left}input{padding:.5rem;width:30rem}td{overflow-wrap:anywhere}</style>
<h1>GekkoAOT Compatibility Lab</h1><p>Boot, menu and gameplay are separate evidence. Unsupported inputs are not game failures. CPU observations during competing workloads are not controlled benchmarks.</p>'''
    page += '<p>' + html.escape(json.dumps(counts)) + '</p><input id="filter" placeholder="Filter games / status" aria-label="Filter results">'
    page += '<table><thead><tr><th>Game<th>Kind<th>Status<th>Reason<th>CPU % (one core)<th>GPU activity<th>Audio init<th>Artifacts</tr></thead><tbody>' + body + '</tbody></table>'
    page += '''<script>document.querySelector('#filter').oninput=e=>{for(const r of document.querySelectorAll('tbody tr'))r.hidden=!r.textContent.toLowerCase().includes(e.target.value.toLowerCase())}</script></html>'''
    (directory / 'index.html').write_text(page)


def import_review(directory, review_path):
    """Only an explicit review tied to an unchanged run/capture can upgrade a scene."""
    report = json.loads((directory / 'results.json').read_text())
    review = json.loads(review_path.read_text())
    if review.get('status') not in ('MENU_OK', 'GAMEPLAY_VERIFIED') or not review.get('reviewer') or not review.get('notes'):
        raise ValueError('review needs reviewer, notes and MENU_OK or GAMEPLAY_VERIFIED status')
    row = next(r for r in report['games'] if r['id'] == review['id'])
    if row['status'] not in ('BOOT_OK', 'MENU_OK', 'GAMEPLAY_VERIFIED'):
        raise ValueError('cannot upgrade a failed or untested run')
    evidence_path = Path(row['evidence']['result_path']).resolve()
    if boot.sha256(evidence_path) != review['evidence_sha256'] or not evidence_unchanged(row):
        raise ValueError('review is not bound to current retained evidence')
    if source_identity(row) != row['identity']['source']:
        raise ValueError('source changed after observation')
    captures = {str(Path(p).resolve()) for p in row['evidence']['screenshots']}
    if not review.get('captures'):
        raise ValueError('review requires captured scene evidence')
    for capture in review['captures']:
        path = (review_path.parent / capture['path']).resolve()
        if str(path) not in captures or boot.sha256(path) != capture['sha256']:
            raise ValueError('reviewed capture is not from this run or has changed')
    row['status'] = review['status']
    row['review'] = review
    row['reason'] = 'Explicit scene review by ' + review['reviewer'] + ': ' + review['notes']
    stage = 'gameplay' if review['status'] == 'GAMEPLAY_VERIFIED' else 'menu'
    row['diagnostics'][stage] = 'EXPLICITLY_REVIEWED'
    write_reports(directory, report['games'], report['context'])
    return 0


def compare_screenshot(actual, spec, destination):
    """Optional pixel oracle; hashes/threshold/scene explicitly belong to a fixture."""
    from PIL import Image, ImageChops, ImageStat
    baseline = Path(spec['baseline'])
    if boot.sha256(baseline) != spec['sha256']:
        raise ValueError('baseline SHA-256 mismatch')
    with Image.open(actual) as a, Image.open(baseline) as b:
        if a.size != b.size:
            return dict(passed=False, reason='image dimensions differ')
        diff = ImageChops.difference(a.convert('RGB'), b.convert('RGB'))
        error = sum(ImageStat.Stat(diff).mean) / (3 * 255)
        diff.save(destination)
    threshold = float(spec.get('max_mean_error', 0))
    if not 0 <= threshold <= 1:
        raise ValueError('invalid screenshot threshold')
    return dict(passed=error <= threshold, mean_absolute_error=error, threshold=threshold,
                actual_sha256=boot.sha256(actual), baseline_sha256=spec['sha256'])


def worker(request_path):
    request = json.loads(request_path.read_text())
    metadata_path = Path(request['metadata'])
    target = Path(request['target'])
    fixture = request.get('fixture', {})
    # Reuse the production campaign isolation guard; input is never sent to the desktop.
    from private_game_campaign import PrivateKeys, require_private_session
    require_private_session()

    class RecordedKeys(PrivateKeys):
        def __init__(self):
            seed = Path(request['launcher_users']) if request.get('gameplay') else target / 'no-launcher-saves'
            super().__init__(request.get('input_seconds', request['seconds']), seed, trace_pc=False)
            self.pending = list(fixture.get('input', []))
            self.navigation_discovery = request.get('gameplay') and 'input' not in fixture
            if self.navigation_discovery:
                # Discovery attempts are recorded, never labelled verified gameplay.
                limit = request.get('input_seconds', 70)
                self.pending = [dict(at_seconds=t, keys=['Return'], hold_seconds=.15)
                                for t in range(5, limit, 30)]
                self.pending += [dict(at_seconds=t, keys=['j'], hold_seconds=.15)
                                 for t in range(9, limit, 3)]
                self.pending.sort(key=lambda event: event['at_seconds'])

        def after_capture(self, elapsed):
            # The sampler must be armed after runner startup, never before its constructor.
            super().after_capture(elapsed)
            prefix = os.environ.get('GEKKOAOT_NATIVE_SAMPLE')
            if prefix and not Path(prefix + '.start').exists():
                Path(prefix + '.start').touch()

        def tick(self, pid, elapsed):
            if (self.target / 'stop-input').exists() or not self.pending or elapsed < self.pending[0]['at_seconds']:
                return
            event = self.pending.pop(0)
            keys = event['keys']
            windows = self.command('search', '--onlyvisible', '--pid', str(pid), check=False).stdout.splitlines()
            if not windows:
                raise RuntimeError('recording target has no visible private window')
            self.command('windowfocus', '--sync', windows[0])
            if self.command('getwindowfocus', 'getwindowpid').stdout.strip() != str(pid):
                raise RuntimeError('input target PID mismatch')
            try:
                self.command('keydown', *keys)
                time.sleep(event.get('hold_seconds', .1))
            finally:
                self.command('keyup', *keys, check=False)
            self.actions.append(dict(event, actual_elapsed_seconds=elapsed))
            boot.atomic_json(self.target / 'input-actions.json', self.actions)

    evidence = boot.run(metadata_path, target / 'boots', request['seconds'],
                        Path(request['runner']), input_driver=RecordedKeys())
    text = Path(evidence['result_path']).with_name('run.log').read_text(errors='replace')
    diag = parse_diagnostics(text)
    diag['enhanced_requested_by_profile'] = request.get('profile') == 'graphics' or request.get('enhanced', False)
    from renderer_coverage_report import report as renderer_report
    enhanced = renderer_report(text.splitlines(), evidence['game_id'])
    boot.atomic_json(target / 'enhanced-coverage.json', enhanced)
    diag['enhanced_measurement'] = enhanced
    diag['aspect_requested'] = request.get('aspect', '4:3')
    diag['widescreen_validation'] = 'UNVERIFIED'
    diag['navigation_scope'] = 'Recorded discovery attempts; requires visual gameplay review' if request.get('gameplay') else 'No generic gameplay navigation requested'
    sample = target / 'native-samples.samples.json'
    diag['native_cpu_samples'] = json.loads(sample.read_text()) if sample.exists() else None
    diag.update(audio_backend='SDL dummy (initialization only; no listening validation)',
                cpu_percent_one_core=evidence['cpu_percent_one_core'],
                peak_sampled_rss_bytes=evidence['peak_sampled_rss_bytes'])
    comparisons = []
    for spec in fixture.get('screenshots', []):
        candidates = evidence['screenshots']
        index = spec.get('capture_index', -1)
        if not candidates or not -len(candidates) <= index < len(candidates):
            comparisons.append(dict(passed=False, reason='required capture unavailable'))
        else:
            comparisons.append(compare_screenshot(candidates[index], spec, target / f'diff-{len(comparisons)}.png'))
    boot.atomic_json(target / 'crash.json', dict(process_exit=evidence['process_exit'],
                     forced_stop=evidence['forced_stop'], result_line=evidence['result_line'],
                     retained_log=str(Path(evidence['result_path']).with_name('run.log')),
                     dump_files=[str(p) for p in target.rglob('core*') if p.is_file()]))
    status = outcome(diag, evidence['process_exit'], evidence['forced_stop'], evidence['result_line'] or '')
    if comparisons and not all(c['passed'] for c in comparisons):
        status = 'GRAPHICS_BROKEN'
    # A menu oracle must be explicitly designated and match. Gameplay is never inferred.
    if status == 'BOOT_OK' and comparisons and all(c['passed'] for c in comparisons) and fixture.get('scene') == 'menu' and fixture.get('reviewed_by'):
        status = 'MENU_OK'
        diag['menu'] = 'VERIFIED_BY_SCREENSHOT_FIXTURE'
    metadata = json.loads(metadata_path.read_text())
    retained = [Path(evidence['result_path']), metadata_path, Path(metadata['sdk_manifest'])]
    retained += [Path(p) for p in evidence['screenshots']]
    retained += [Path(evidence['result_path']).with_name('run.log')]
    retained_sha256 = {str(p): boot.sha256(p) for p in retained}
    boot.atomic_json(target / 'worker-result.json', dict(status=status, diagnostics=diag, retained_sha256=retained_sha256,
                     screenshot_comparisons=comparisons, evidence=evidence))
    return 0


def load_fixture(path, row):
    if path is None:
        return {}
    value = json.loads(path.read_text()).get(row['relative'], {})
    events = value.get('input', [])
    previous = -1
    for event in events:
        at = float(event['at_seconds'])
        keys = event['keys']
        if at < previous or at < 0 or not isinstance(keys, list) or not keys or any(
                not re.fullmatch(r'[A-Za-z0-9_]+', k) for k in keys):
            raise ValueError('invalid ordered input recording')
        if not 0 < float(event.get('hold_seconds', .1)) <= 2:
            raise ValueError('recorded hold must be in (0, 2] seconds')
        previous = at
    for spec in value.get('screenshots', []):
        spec['baseline'] = str((path.parent / spec['baseline']).resolve())
        if boot.sha256(spec['baseline']) != spec['sha256']:
            raise ValueError('fixture baseline has changed')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--corpus', type=Path, default=CORPUS)
    parser.add_argument('--results', type=Path, default=ROOT / '.gekkoaot/compat/lab')
    parser.add_argument('--profile', choices=('quick', 'graphics', 'full'), default='quick')
    parser.add_argument('--only', help='case insensitive path fragment')
    parser.add_argument('--entry-id', help='exact discovered entry ID for automated queues')
    parser.add_argument('--inventory-only', action='store_true')
    parser.add_argument('--compile-only', action='store_true')
    parser.add_argument('--incremental', action='store_true')
    parser.add_argument('--release', action='store_true', help='force full profile and bypass result reuse')
    parser.add_argument('--seconds', type=int, default=40)
    parser.add_argument('--compile-timeout', type=int, default=3600)
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--fixtures', type=Path)
    parser.add_argument('--runner', type=Path, default=executable('gekkoaot-native-run'))
    parser.add_argument('--controller', type=Path, default=executable('gekkoaotctl'))
    parser.add_argument('--native-samples', action='store_true')
    parser.add_argument('--gameplay', action='store_true', help='attempt menu navigation and require reviewed gameplay plus Enhanced evidence')
    parser.add_argument('--enhanced', action='store_true', help='exercise Enhanced Renderer in every selected game')
    parser.add_argument('--aspect', choices=('4:3', '16:9'), default='4:3')
    parser.add_argument('--launcher-users', type=Path, default=ROOT / '.gekkoaot/user')
    parser.add_argument('--input-seconds', type=int, default=70)
    parser.add_argument('--observe-cached', action='store_true',
                        help='diagnose a prior controller-selected build; does not validate current code compilation')
    parser.add_argument('--review', type=Path, help='import an explicit human scene review into existing results')
    parser.add_argument('--worker', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        return worker(args.worker)
    if args.review:
        return import_review(args.results.resolve(), args.review.resolve())
    if args.seconds < 20 or args.compile_timeout <= 0 or args.jobs < 1:
        parser.error('seconds >= 20, positive compile timeout and jobs required')
    if args.gameplay and (args.seconds < 90 or not 0 < args.input_seconds < args.seconds - 10):
        parser.error('gameplay observation requires >= 90 seconds and a stable tail after navigation')
    if not args.corpus.is_dir():
        parser.error('corpus directory does not exist')
    args.results = args.results.resolve()
    if args.results == args.corpus.resolve() or args.corpus.resolve() in args.results.parents:
        parser.error('results must be outside original corpus')
    args.results.mkdir(parents=True, exist_ok=True)
    if args.release and (args.observe_cached or args.only or args.entry_id or args.compile_only or args.inventory_only):
        parser.error('--release requires the complete current build and execution campaign')
    if args.release:
        args.profile, args.incremental = 'full', False
    rows = discover(args.corpus)
    context = dict(profile=args.profile, inventory_only=args.inventory_only,
                   observe_cached=args.observe_cached, require_gameplay=args.gameplay, enhanced=args.enhanced or args.gameplay, aspect=args.aspect,
                   gameplay_policy='Only separately reviewed gameplay evidence can certify playability',
                   time=time.time(), component_identity=component_identity(),
                   host=dict(platform=platform.platform(), machine=platform.machine(), logical_cpus=os.cpu_count()))
    artifacts = args.results / 'runs'
    artifacts.mkdir(exist_ok=True)
    failed = False
    prior_path = args.results / 'results.json'
    prior_rows = {r['id']: r for r in json.loads(prior_path.read_text())['games']} if prior_path.exists() else {}
    for row in rows:
        selected = args.profile == 'full' or any(s in row['relative'].casefold() for s in PROFILES[args.profile])
        selected = selected and (not args.only or args.only.casefold() in row['relative'].casefold())
        selected = selected and (not args.entry_id or args.entry_id == row['id'])
        row['selected'] = selected
        if not selected or not row['supported'] or args.inventory_only:
            prior = prior_rows.get(row['id'])
            if prior and prior.get('identity') and not args.inventory_only and row['supported']:
                stale = (prior['identity']['components'] != context['component_identity']
                         or prior['identity']['source'] != source_identity(row))
                row.update(prior, selected=False, stale=stale)
                if stale:
                    row.update(previous_status=prior['status'], status='NOT_TESTED',
                               reason='Prior observation retained; source/shared components changed')
            continue
        target = artifacts / row['id'] / (time.strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:6])
        target.mkdir(parents=True)
        row['artifact_dir'] = str(target)
        print(f"[{row['id']}] {row['name']}", flush=True)
        try:
            fixture = load_fixture(args.fixtures, row)
            identity = dict(components=context['component_identity'], source=source_identity(row),
                            seconds=args.seconds, profile=args.profile, fixture=fixture, native_samples=args.native_samples,
                            controller=boot.sha256(args.controller), runner=boot.sha256(args.runner),
                            compile_only=args.compile_only, observe_cached=args.observe_cached, gameplay=args.gameplay,
                            enhanced=args.enhanced, aspect=args.aspect, input_seconds=args.input_seconds,
                            launcher_users=str(args.launcher_users.resolve()),
                            environment={k: v for k, v in os.environ.items() if k.startswith('GEKKOAOT_')})
            cache_key = digest(identity)
            result_cache = args.results / 'cache' / (row['id'] + '.json')
            if args.incremental and result_cache.exists():
                cached = json.loads(result_cache.read_text())
                if (cached['key'] == cache_key and cached['row']['status'] in ('BOOT_OK', 'MENU_OK')
                        and evidence_unchanged(cached['row'])):
                    row.update(cached['row'], cached=True)
                    continue
            row['identity'] = identity
            compile_source = row['source']
            if row['kind'] == 'extracted':
                image = args.results / 'prepared' / (row['id'] + '-' + identity['source']['digest'][:16] + '.gcm')
                if not image.exists():
                    prepare_extracted(row['source'], image)
                compile_source = str(image)
            env = dict(os.environ, GEKKOAOT_ISO=compile_source, GEKKOAOT_JOBS=str(args.jobs),
                       GEKKOAOT_GAME='compat-' + row['id'] + '-' + identity['source']['digest'][:16])
            # Always invoke controller: it owns compiler, SDK, renderer and module cache validity.
            compile_result = (dict(exit_code=0, timed_out=False, provenance='prior-controller-selection; current compilation not tested')
                              if args.observe_cached else bounded([str(args.controller.resolve()), 'compile'], env,
                                     target / 'compile.log', args.compile_timeout))
            row['compile'] = compile_result
            previous = json.loads(result_cache.read_text()).get('row') if result_cache.exists() else None
            metadata = selection(compile_source) if compile_result['exit_code'] == 0 else None
            if not metadata:
                row.update(status='COMPILE_FAILED', reason=(f'Compilation timed out after {args.compile_timeout}s; no runtime verdict'
                           if compile_result['timed_out'] else 'Compile failed or no source-validated launch selection'))
            elif args.compile_only:
                row.update(status='NOT_TESTED', reason='Compilation verified; execution not requested', metadata=str(metadata))
            elif not shutil.which('gamescope'):
                row.update(status='NOT_TESTED', reason='Owned headless Gamescope unavailable; execution not attempted')
            else:
                request = dict(metadata=str(metadata), target=str(target), seconds=args.seconds,
                               runner=str(args.runner.resolve()), fixture=fixture, profile=args.profile,
                               gameplay=args.gameplay, enhanced=args.enhanced or args.gameplay,
                               launcher_users=str(args.launcher_users.resolve()), input_seconds=args.input_seconds, aspect=args.aspect)
                boot.atomic_json(target / 'request.json', request)
                env = dict(os.environ, GEKKOAOT_TEST_DESKTOP_DISPLAY=os.environ.get('DISPLAY', ''), SDL_AUDIODRIVER='dummy')
                for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'XAUTHORITY', 'GEKKOAOT_INPUT_CONFIG', 'GEKKOAOT_USER_DIR'):
                    env.pop(key, None)
                if args.native_samples:
                    env['GEKKOAOT_NATIVE_SAMPLE'] = str(target / 'native-samples')
                    env['GEKKOAOT_NATIVE_SAMPLE_SECONDS'] = str(max(1, args.seconds - 8))
                env['GEKKOAOT_NATIVE_GX_IR_PROFILE'] = '1'
                env['GEKKOAOT_ASPECT_MODE'] = args.aspect
                if args.profile == 'graphics' or args.enhanced or args.gameplay:
                    env['GEKKOAOT_ENHANCED_RENDERER'] = '1'
                    env['GEKKOAOT_ENHANCED_MEASURE'] = '1'
                    env['GEKKOAOT_ENHANCED_DIAGNOSTICS'] = '1'
                    if args.gameplay or args.enhanced:
                        env.update(GEKKOAOT_AO_QUALITY='2', GEKKOAOT_CONTACT_SHADOWS='1',
                                   GEKKOAOT_FRAGMENT_LIGHTING='1', GEKKOAOT_ENVIRONMENT_LIGHTING='2',
                                   GEKKOAOT_SUN_SHADOWS='2', GEKKOAOT_GI='1', GEKKOAOT_SSR='1')
                command = ['gamescope', '--backend', 'headless', '-W', '1280', '-H', '720',
                           '-w', '1280', '-h', '720', '-r', '60', '--', sys.executable,
                           str(Path(__file__).resolve()), '--worker', str(target / 'request.json')]
                execution = bounded(command, env, target / 'compositor.log', args.seconds + 90)
                row['execution'] = execution
                result_path = target / 'worker-result.json'
                if result_path.exists() and execution['exit_code'] == 0:
                    row.update(json.loads(result_path.read_text()))
                    row['reason'] = 'Automated observation; gameplay remains unverified'
                else:
                    started = list((target / 'boots').glob('*/*/pid.json'))
                    row.update(status='BOOT_FAILED' if started else 'NOT_TESTED',
                               reason='Worker/compositor failed; see compositor.log and retained boot artifacts')
            if previous:
                row['previous_status'] = previous['status']
                row['regression'] = previous['status'] in ('BOOT_OK', 'MENU_OK', 'GAMEPLAY_VERIFIED') and row['status'] in ('COMPILE_FAILED', 'BOOT_FAILED', 'CRASH', 'GRAPHICS_BROKEN', 'AUDIO_BROKEN')
            result_cache.parent.mkdir(exist_ok=True)
            boot.atomic_json(result_cache, dict(key=cache_key, row=row))
        except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
            row.update(status='NOT_TESTED', reason=f'Infrastructure error: {error}')
        failed |= row['status'] in ('COMPILE_FAILED', 'BOOT_FAILED', 'CRASH', 'GRAPHICS_BROKEN', 'AUDIO_BROKEN')
        write_reports(args.results, rows, context)
        print('  ' + row['status'] + ': ' + row.get('reason', ''), flush=True)
    write_reports(args.results, rows, context)
    if failed:
        return 1
    if args.gameplay and any(r['supported'] and r['selected'] and not r['acceptance']['playable_with_enhanced'] for r in rows):
        return 2
    expected = [r for r in rows if r['selected'] and r['supported']]
    if not args.inventory_only and not args.compile_only and (not expected or any(r['status'] == 'NOT_TESTED' for r in expected)):
        return 2
    return 0


if __name__ == '__main__':
    if os.name != 'nt':
        def interrupted(signum, frame):
            raise KeyboardInterrupt
        signal.signal(signal.SIGTERM, interrupted)
    raise SystemExit(main())
