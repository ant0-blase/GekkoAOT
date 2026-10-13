#!/usr/bin/env python3
"""Inventory the requested game corpus without claiming untested playability.

Optionally prepare missing executable caches with the existing nod tool. Only
equal-sized image candidates are hashed to establish exact duplicates; game IDs
alone never merge revisions or discs. Standalone DOL/ELF files remain separate.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_IMAGES = ROOT / 'debug/Extra SDKs + Tools + Libraries + Docs/Demos/NDDEMO v1.08'
DISC_EXTENSIONS = {'.rvz', '.iso', '.gcm', '.wia'}
EXECUTABLE_EXTENSIONS = {'.dol', '.elf'}
VALIDATION = ('boot', 'gameplay', 'combat', 'level_transition', 'save_load',
              'cpu_performance', 'ram_performance', 'crash_free_session')


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def source_caches(state):
    found = {}
    for metadata in (state / 'cache/discs').glob('*/meta/disc-source.json'):
        try:
            source = Path(json.loads(metadata.read_text())['source']).resolve()
            cache = metadata.parent.parent
            if (cache / 'sys/main.dol').is_file() and (cache / 'meta/disc-id.txt').is_file():
                found.setdefault(source, []).append(cache)
        except (OSError, KeyError, ValueError):
            continue
    return found


def inventory(images, state, results, extract_missing=False, disc_tool=None):
    paths = sorted(path.resolve() for path in images.rglob('*') if path.is_file()
                   and path.suffix.lower() in DISC_EXTENSIONS | EXECUTABLE_EXTENSIONS)
    caches = source_caches(state)
    # A path hash namespaces prepared caches; it is not advertised as a content hash.
    prepared = results / 'prepared'
    for metadata in prepared.glob('*/meta/disc-source.json'):
        try:
            cache = metadata.parent.parent
            source = Path(json.loads(metadata.read_text())['source']).resolve()
            if (cache / 'sys/main.dol').is_file():
                caches.setdefault(source, []).append(cache)
        except (OSError, KeyError, ValueError):
            continue
    by_size = {}
    for path in paths:
        if path.suffix.lower() in DISC_EXTENSIONS:
            by_size.setdefault(path.stat().st_size, []).append(path)
    duplicate_candidates = {path for group in by_size.values() if len(group) > 1 for path in group}
    content_seen = {}
    rows = []
    for index, path in enumerate(paths, 1):
        print(f'[{index}/{len(paths)}] {path.name}', flush=True)
        kind = 'disc' if path.suffix.lower() in DISC_EXTENSIONS else 'standalone-executable'
        row = {'source': str(path), 'name': path.stem, 'kind': kind,
               'size_bytes': path.stat().st_size, 'sha256': None, 'duplicate_of': None,
               'game_id': None, 'disc_cache': None, 'main_dol': None,
               'aot_candidates': [], 'aot_source_matches': [],
               'preparation_error': None,
               'validation': {name: 'unverified' for name in VALIDATION}}
        if path in duplicate_candidates:
            row['sha256'] = sha256(path)
            row['duplicate_of'] = content_seen.get(row['sha256'])
            content_seen.setdefault(row['sha256'], str(path))
        existing = caches.get(path, [])
        if kind == 'disc' and not existing and extract_missing and not row['duplicate_of']:
            cache = prepared / hashlib.sha256(str(path).encode()).hexdigest()[:16]
            cache.mkdir(parents=True, exist_ok=True)
            with (cache / 'prepare.log').open('w') as log:
                try:
                    subprocess.run([str(disc_tool), str(path), str(cache)], stdout=log,
                                   stderr=subprocess.STDOUT, check=True, timeout=180)
                    if not (cache / 'sys/main.dol').is_file():
                        raise RuntimeError('disc tool did not produce main.dol')
                    existing = [cache]
                except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                    row['preparation_error'] = str(error)
        if row['duplicate_of'] and not existing:
            prior = next(item for item in rows if item['source'] == row['duplicate_of'])
            if prior['disc_cache']:
                existing = [Path(prior['disc_cache'])]
        if existing:
            cache = max(existing, key=lambda item: (item / 'sys/main.dol').stat().st_mtime)
            row['disc_cache'] = str(cache)
            row['main_dol'] = str(cache / 'sys/main.dol')
            row['game_id'] = (cache / 'meta/disc-id.txt').read_text().strip()
            modules = sorted((state / 'cache/modules' / row['game_id']).glob(
                '*/g' + row['game_id'] + '_recomp.so'))
            row['aot_candidates'] = [str(module) for module in modules if module.is_file()]
            dol_digest = sha256(cache / 'sys/main.dol')
            row['main_dol_sha256'] = dol_digest
            for module in modules:
                original = module.parent / 'dolrecomp-output/generated/main.dol'
                if original.is_file() and sha256(original) == dol_digest:
                    row['aot_source_matches'].append(str(module))
        elif kind == 'standalone-executable' and path.suffix.lower() == '.dol':
            row['main_dol'] = str(path)
            row['main_dol_sha256'] = sha256(path)
        rows.append(row)
        # Preserve completed inventory work if a later image cannot be read.
        write_results(rows, images, results)
    return rows


def write_results(rows, images, results):
    results.mkdir(parents=True, exist_ok=True)
    discs = [row for row in rows if row['kind'] == 'disc']
    report = {'schema': 1, 'source_root': str(images.resolve()),
              'validation_policy': 'all stages unverified until runtime evidence is recorded',
              'images': rows,
              'summary': {'disc_files': len(discs),
                          'exact_duplicate_files': sum(bool(row['duplicate_of']) for row in discs),
                          'distinct_disc_images': sum(not row['duplicate_of'] for row in discs),
                          'standalone_executables': len(rows) - len(discs),
                          'disc_caches_available': sum(bool(row['disc_cache']) for row in discs),
                          'images_with_aot_candidates': sum(bool(row['aot_candidates']) for row in discs),
                          'images_with_matching_dol_source': sum(bool(row['aot_source_matches']) for row in discs),
                          'source_audits_complete': sum(bool(row.get('native_port_source_audit', {}).get('complete_input_scan')) for row in rows),
                          'source_audit_failures': sum(bool(row.get('native_port_source_audit', {}).get('error')) for row in rows)}}
    temporary = results / 'corpus.json.tmp'
    temporary.write_text(json.dumps(report, indent=2) + '\n')
    temporary.replace(results / 'corpus.json')
    fields = ['name', 'kind', 'game_id', 'source', 'duplicate_of', 'aot_candidates',
              'aot_source_matches', *VALIDATION, 'preparation_error']
    with (results / 'validation.csv').open('w', newline='') as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            flat = {key: row.get(key) for key in fields if key not in VALIDATION}
            flat['aot_candidates'] = len(row['aot_candidates'])
            flat['aot_source_matches'] = len(row['aot_source_matches'])
            flat.update(row['validation'])
            writer.writerow(flat)


def audit_sources(rows, images, state, results, compiler):
    """Scan source obligations, preserving all runtime validation as unverified."""
    seen = {}
    for index, row in enumerate(rows, 1):
        dol = row.get('main_dol')
        if not dol:
            continue  # ELF analysis needs a separate container/coverage pass.
        digest = row['main_dol_sha256']
        print(f'[source audit {index}/{len(rows)}] {row["name"]}', flush=True)
        if digest in seen:
            row['native_port_source_audit'] = dict(seen[digest])
            write_results(rows, images, results)
            continue
        output = results / 'source-audits' / digest
        output.mkdir(parents=True, exist_ok=True)
        report = output / 'side-exits.json'
        report.unlink(missing_ok=True)
        env = dict(os.environ, DOLRECOMP_SIDE_EXIT_ANALYSIS_ONLY='1',
                   DOLRECOMP_SIDE_EXIT_REPORT=str(report.resolve()),
                   DOLRECOMP_STATIC_INTERCEPTS='', DOLRECOMP_LLVM_CACHE='off')
        command = [str(compiler), '--backend=llvm', '--cpu', 'gekko', '--gamecube',
                   '--state-in-memory', '--native-abi=unrestricted', '--targets=host',
                   '--semantics=exact', '--partition-instructions', '1024']
        # Content-addressed source reports use the compiler's intrinsic entry
        # discovery only. An external map must be included in the identity if
        # this pass later starts consuming per-game maps or SDK manifests.
        command += [str(dol), str(output / 'analysis')]
        result = {'report': None, 'complete_input_scan': False, 'full_native': False,
                  'sdk_coverage': 'unverified', 'runtime_coverage': 'unverified', 'error': None}
        with (output / 'analysis.log').open('w') as log:
            try:
                subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                               check=True, timeout=180)
                parsed = json.loads(report.read_text())
                if parsed['schema'] != 1 or not parsed['complete_input_scan'] or parsed['full_native']:
                    raise ValueError('invalid source-only audit result')
                result.update(report=str(report), complete_input_scan=True)
            except (OSError, KeyError, ValueError, subprocess.SubprocessError) as error:
                result['error'] = str(error)
        row['native_port_source_audit'] = result
        seen[digest] = dict(result)
        write_results(rows, images, results)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--images', type=Path, default=DEFAULT_IMAGES)
    parser.add_argument('--state', type=Path, default=ROOT / '.gekkoaot')
    parser.add_argument('--results', type=Path, default=ROOT / '.gekkoaot/compat/corpus-v232')
    parser.add_argument('--extract-missing', action='store_true')
    parser.add_argument('--disc-tool', type=Path, default=ROOT / 'bin/gekkoaot-native-disc')
    parser.add_argument('--audit-source', action='store_true')
    parser.add_argument('--compiler', type=Path, default=ROOT / '.gekkoaot/build/DolRecomp/dolrecomp')
    args = parser.parse_args()
    if not args.images.is_dir():
        parser.error('image directory does not exist')
    rows = inventory(args.images, args.state, args.results, args.extract_missing, args.disc_tool)
    if args.audit_source:
        audit_sources(rows, args.images, args.state, args.results, args.compiler)
    print(json.loads((args.results / 'corpus.json').read_text())['summary'], flush=True)
    return 1 if any(row['preparation_error'] or row.get('native_port_source_audit', {}).get('error')
                    for row in rows) else 0


if __name__ == '__main__':
    raise SystemExit(main())
