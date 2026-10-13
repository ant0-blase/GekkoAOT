#!/usr/bin/env python3
"""Reject stale or mismatched source/SDK selections before launching a game."""
import importlib.util
import json
from pathlib import Path
import tempfile

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('corpus_boot', ROOT / 'tools/corpus_boot.py')
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)

with tempfile.TemporaryDirectory(prefix='gekkoaot-boot-selection-') as temporary:
    root = Path(temporary)
    artifact = root / 'module'
    generated = artifact / 'dolrecomp-output/generated'
    generated.mkdir(parents=True)
    for name, contents in [('image.rvz', b'image'), ('main.dol', b'revision-one'),
                           ('bridge.so', b'bridge'), ('sdk.txt', b'entry-one'),
                           ('side.so', b'secondary')]:
        (root / name).write_bytes(contents)
    (artifact / 'main.so').write_bytes(b'native-module')
    (generated / 'main.dol').write_bytes(b'revision-one')
    metadata = dict(schema=1, native_os='0', source=str(root / 'image.rvz'),
                    dol=str(root / 'main.dol'), module=str(artifact / 'main.so'),
                    bridge=str(root / 'bridge.so'), sdk_manifest=str(root / 'sdk.txt'),
                    dol_fnv64=boot.fnv64(root / 'main.dol'),
                    sdk_fnv64=boot.fnv64(root / 'sdk.txt'),
                    secondary_modules=[str(root / 'side.so')])
    boot.validate_launch(json.loads(json.dumps(metadata)))

    def rejected():
        try:
            boot.validate_launch(metadata)
        except ValueError:
            return
        raise AssertionError('unsafe selection was admitted')

    (generated / 'main.dol').write_bytes(b'revision-two')
    rejected()  # Same game ID/path does not establish compiled source identity.
    (generated / 'main.dol').write_bytes(b'revision-one')
    (root / 'sdk.txt').write_bytes(b'entry-two')
    rejected()  # Same-size SDK replacement is still a different admission set.
    (root / 'sdk.txt').write_bytes(b'entry-one')
    (root / 'main.dol').write_bytes(b'revision-two')
    (generated / 'main.dol').write_bytes(b'revision-two')
    rejected()  # Both DOL copies changed after the controller's selection.
    (root / 'main.dol').write_bytes(b'revision-one')
    (generated / 'main.dol').write_bytes(b'revision-one')
    (root / 'side.so').unlink()
    rejected()  # An incomplete REL set must never become a normal launch.
print('Boot selection: revision, SDK identity, stale metadata and incomplete secondary set rejected')
