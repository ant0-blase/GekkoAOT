#!/usr/bin/env python3
"""Inventory must not merge revisions by game ID or certify cache playability."""
from pathlib import Path
import importlib.util
import json
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('game_corpus', ROOT / 'tools/game_corpus.py')
catalog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalog)
with tempfile.TemporaryDirectory(prefix='gekkoaot-corpus-probe-') as temporary:
    base = Path(temporary)
    images = base / 'images'
    images.mkdir()
    (images / 'a.rvz').write_bytes(b'abcdefgh')
    (images / 'b.rvz').write_bytes(b'abcdefgh')  # exact duplicate
    (images / 'c.rvz').write_bytes(b'abcdEFGH')  # same size and ID, different revision
    (images / 'main.dol').write_bytes(b'standalone-code')
    state = base / 'state'
    for name in ('a', 'c'):
        cache = state / 'cache/discs' / name
        (cache / 'sys').mkdir(parents=True)
        (cache / 'meta').mkdir()
        (cache / 'sys/main.dol').write_bytes(b'cached-main-' + name.encode())
        (cache / 'meta/disc-id.txt').write_text('GTEST0\n')
        (cache / 'meta/disc-source.json').write_text(json.dumps({'source': str(images / (name+'.rvz'))}))
    artifact = state / 'cache/modules/GTEST0/variant'
    (artifact / 'dolrecomp-output/generated').mkdir(parents=True)
    (artifact / 'gGTEST0_recomp.so').write_bytes(b'candidate-only')
    (artifact / 'dolrecomp-output/generated/main.dol').write_bytes(b'cached-main-a')
    rows = catalog.inventory(images, state, base / 'results')
    a, b, c = rows[:3]
    assert b['duplicate_of'] == a['source']
    assert c['duplicate_of'] is None and c['game_id'] == a['game_id']
    assert a['aot_source_matches'] and not c['aot_source_matches']
    assert b['disc_cache'] == a['disc_cache']
    assert all(set(row['validation'].values()) == {'unverified'} for row in rows)
    summary = json.loads((base / 'results/corpus.json').read_text())['summary']
    assert summary['disc_files'] == 3 and summary['exact_duplicate_files'] == 1
    assert summary['standalone_executables'] == 1
print('game corpus: exact duplicates, revisions, DOL provenance and unverified runtime stages passed')
