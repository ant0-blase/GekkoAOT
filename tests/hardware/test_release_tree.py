#!/usr/bin/env python3
"""Regression checks for the source/package publication boundary."""
import importlib.util
import io
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('release_tree', ROOT / 'tools/check_release_tree.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ReleaseTreeTests(unittest.TestCase):
    def test_required_sources_are_allowed(self):
        for name in ('build-linux.sh', 'build-windows.cmd', 'runtime/module/split-text.ld',
                     'patches/dolrecomp/gekkoaot-native-address-lowering-v287.patch',
                     'runtime/gx/aurora/aurora_bind_group_cache_v297.py',
                     'tools/private_campaign_profile.py', 'docs/OPTIMIZATIONS.md'):
            with self.subTest(name=name):
                self.assertIsNone(module.forbidden_reason(Path(name)))

    def test_profiles_and_backups_are_rejected(self):
        for name in ('.gekkoaot/diagnostics/run.json', 'build-linux/output.o',
                     'debug/game.dol', 'perf.data', 'perf.data.old',
                     'run.perf.data', 'run.profraw', 'merged.profdata',
                     'gekkoaot-nativehw-threads.data.old', 'gekkoaot-nativehw-hotspots.txt',
                     'runtime/native/runtime.cpp.v209-backup-123', 'patches/old.patch.bak',
                     'docs/GameCube-register-audit-v2.csv', 'docs/NATIVE_GX_V317.md'):
            with self.subTest(name=name):
                self.assertIsNotNone(module.forbidden_reason(Path(name)))

    def test_packages_allow_tools_but_reject_measurements(self):
        for name in ('bin/gekkoaot.exe', 'share/gekkoaot/toolchain/bin/dolrecomp',
                     'share/gekkoaot/tools/compat/gekkoaot-perf.sh',
                     'share/gekkoaot/toolchain/licenses/LICENSE.txt'):
            self.assertIsNone(module.forbidden_reason(Path(name), package=True))
        for name in ('share/gekkoaot/runtime/run.profdata',
                     'share/gekkoaot/runtime/cache.log',
                     'share/gekkoaot/patches/old.patch.bak',
                     'share/gekkoaot/diagnostics/sample.json'):
            self.assertIsNotNone(module.forbidden_reason(Path(name), package=True))

    def test_deleted_document_link_is_detected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'README.md').write_text('[Current](docs/ARCHITECTURE.md)\n'
                                          '[Old](docs/NATIVE_GX_V317.md)\n'
                                          '[External](https://example.com/doc)\n'
                                          '[Anchor](#build)\n', encoding='utf-8')
            (root / 'docs').mkdir()
            (root / 'docs/ARCHITECTURE.md').write_text('# Architecture\n', encoding='utf-8')
            errors = module.document_errors(root, [Path('README.md')])
            self.assertEqual(len(errors), 1)
            self.assertIn('NATIVE_GX_V317.md', errors[0])

    def test_source_archive_retains_build_scripts(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / '.gitattributes').write_bytes((ROOT / '.gitattributes').read_bytes())
            required = ('build-linux.sh', 'build-windows.cmd', 'scripts/build-windows.ps1')
            local = ('build-test/cache.log', 'debug/local.bin', '.gekkoaot/run.json')
            for name in (*required, *local):
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('synthetic archive fixture\n', encoding='utf-8')
            def git(*args):
                return subprocess.run(['git', '-C', str(root), *args], check=True,
                                      capture_output=True).stdout
            git('init', '-q')
            git('add', '-A')
            git('-c', 'user.name=Release test', '-c', 'user.email=test@example.invalid',
                '-c', 'commit.gpgsign=false', 'commit', '-qm', 'Archive fixture')
            with tarfile.open(fileobj=io.BytesIO(git('archive', '--format=tar', 'HEAD'))) as archive:
                names = set(archive.getnames())
            self.assertTrue(set(required) <= names)
            self.assertFalse(set(local) & names)


if __name__ == '__main__':
    unittest.main()
