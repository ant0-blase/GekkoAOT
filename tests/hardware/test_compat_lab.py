#!/usr/bin/env python3
"""Laboratory invariants: isolation, truthful evidence, discovery and source safety."""
import json
import os
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import compat_lab as lab


class CompatibilityLab(unittest.TestCase):
    def test_discovery_unicode_extracted_and_unsupported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "L'été d'O'Brien.ISO").write_bytes(b'image')
            (root / 'supplement.elf').write_bytes(b'elf')
            (root / 'README.md').write_text('not a game')
            game = root / 'extrait'
            (game / 'sys').mkdir(parents=True)
            (game / 'sys/main.dol').write_bytes(b'dol')
            (game / 'sys/boot.bin').write_bytes(b'boot')
            rows = lab.discover(root)
            self.assertEqual(len(rows), 3)
            self.assertEqual(sum(r['supported'] for r in rows), 2)
            self.assertEqual(next(r for r in rows if r['kind'] == 'resource')['status'], 'UNSUPPORTED_INPUT')
            self.assertEqual(lab.discover(root), rows)

    def test_boot_is_not_menu_or_gameplay_and_errors_win(self):
        text = ('GEKKOAOT_NATIVE_DOL_LOADED\n'
                'GEKKOAOT_PERF_V49=1 vps=30 guest_fps=60 present_fps=30\n'
                'GEKKOAOT_ENHANCED_COVERAGE requested=1 draws_3d=120 wsi_presents=5\n')
        diag = lab.parse_diagnostics(text)
        self.assertEqual(lab.outcome(diag, 0, result_line='status="host requested exit"'), 'BOOT_OK')
        self.assertEqual(diag['menu'], 'UNVERIFIED')
        self.assertEqual(diag['gameplay'], 'UNVERIFIED')
        self.assertIsNone(diag['gpu_execution_ms'])
        self.assertEqual(diag['audio_initialization'], 'UNVERIFIED')
        self.assertEqual(lab.outcome(diag, -signal.SIGSEGV), 'CRASH')
        self.assertEqual(lab.outcome(diag, 0, True), 'BOOT_FAILED')
        bad = lab.parse_diagnostics(text + 'GEKKOAOT_NATIVE_GX_FIFO_ERROR action=reject\n')
        self.assertEqual(lab.outcome(bad, 0), 'GRAPHICS_BROKEN')
        bad = lab.parse_diagnostics(text + 'GEKKOAOT_NATIVE_AUDIO_SDL3=0 error="no device"\n')
        self.assertEqual(lab.outcome(bad, 0), 'AUDIO_BROKEN')
        # Configuration messages alone prove no activity.
        self.assertFalse(lab.parse_diagnostics('GEKKOAOT_NATIVE_GX_IR=1')['gpu_activity'])

    def test_expected_gpu_teardown_is_not_a_rendering_fault(self):
        message = '[warning] [aurora::gpu] Device lost: Device was destroyed.\n'
        self.assertTrue(lab.parse_diagnostics(message)['gpu_errors'])
        diag = lab.parse_diagnostics('GEKKOAOT_GRACEFUL_STOP_V20=1\n' + message)
        self.assertFalse(diag['gpu_errors'])
        self.assertEqual(len(diag['teardown_events']), 1)
        self.assertTrue(lab.parse_diagnostics('GEKKOAOT_GRACEFUL_STOP_V20=1\nDevice lost: hardware fault\n')['gpu_errors'])
        diag = lab.parse_diagnostics('GEKKOAOT_ENHANCED_MEASUREMENT timings_valid=0 gpu_ns=99,88\n'
                                    'GEKKOAOT_ENHANCED_MEASUREMENT timings_valid=1 gpu_ns=42,81\n')
        self.assertEqual(diag['enhanced_gpu_pass_ns'], [[42, 81]])

    def test_cumulative_markers_not_summed(self):
        diag = lab.parse_diagnostics('GEKKOAOT_NATIVE_SDK_DIRECT_V371 name=memcpy accepted_calls=4\n'
                                    'GEKKOAOT_NATIVE_SDK_DIRECT_V371 name=memcpy accepted_calls=9\n')
        self.assertEqual(len(diag['nativegx_markers']), 1)
        self.assertIn('accepted_calls=9', next(iter(diag['nativegx_markers'].values())))

    @unittest.skipIf(os.name == 'nt', 'POSIX process group accounting')
    def test_timeout_kills_owned_descendants(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            script = tmp / "process with spaces.py"
            child = tmp / 'child.pid'
            script.write_text('import subprocess,sys,time\n'
                              'from pathlib import Path\n'
                              'p=subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"])\n'
                              'Path(sys.argv[1]).write_text(str(p.pid))\n'
                              'time.sleep(60)\n')
            result = lab.bounded([sys.executable, str(script), str(child)], os.environ.copy(), tmp / 'log', .5)
            self.assertTrue(result['timed_out'])
            pid = int(child.read_text())
            time.sleep(.1)
            stat = Path(f'/proc/{pid}/stat')
            self.assertTrue(not stat.exists() or stat.read_text().rsplit(') ', 1)[1].startswith('Z'))

    def test_extracted_image_relocates_fst_without_mutating_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / 'original'
            (root / 'sys').mkdir(parents=True)
            (root / 'files').mkdir()
            (root / 'sys/boot.bin').write_bytes(bytes(0x440))
            (root / 'sys/main.dol').write_bytes(b'DOL fixture')
            fst = bytearray(struct.pack('>IIIIII', 0x01000000, 0, 2, 0, 0xdead, 5) + b"O'Brien.txt\0")
            (root / 'sys/fst.bin').write_bytes(fst)
            (root / "files/O'Brien.txt").write_bytes(b'hello')
            before = lab.source_identity({'source': str(root)})
            image = lab.prepare_extracted(root, Path(tmp) / 'prepared/test.gcm')
            data = image.read_bytes()
            dol, offset, size, _ = struct.unpack_from('>IIII', data, 0x420)
            self.assertEqual(data[dol:dol+11], b'DOL fixture')
            file_offset = struct.unpack_from('>I', data, offset+16)[0]
            self.assertEqual(data[file_offset:file_offset+5], b'hello')
            self.assertEqual(lab.source_identity({'source': str(root)}), before)
            (root / 'sys/fst.bin').write_bytes(fst[:-12] + b'../outside\0')
            with self.assertRaises(ValueError):
                lab.prepare_extracted(root, Path(tmp) / 'unsafe.gcm')

    def test_reports_escape_names_and_retain_status(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            row = dict(id='1', name='<script>bad()</script>', source="a'b.iso", kind='disc', status='NOT_TESTED')
            lab.write_reports(path, [row], {})
            self.assertNotIn('<script>bad()', (path / 'index.html').read_text())
            self.assertEqual(json.loads((path / 'results.json').read_text())['games'][0]['status'], 'NOT_TESTED')
            self.assertIn("a'b.iso", (path / 'results.csv').read_text())

    def test_fixture_validation_and_identity_invalidation(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'source.iso'
            source.write_bytes(b'first')
            a = lab.source_identity({'source': str(source)})
            source.write_bytes(b'second revision')
            self.assertNotEqual(a, lab.source_identity({'source': str(source)}))
            fixture = Path(tmp) / 'fixtures.json'
            fixture.write_text(json.dumps({'game.iso': {'input': [{'at_seconds': 1, 'keys': ['Return;bad']}]}}))
            with self.assertRaises(ValueError):
                lab.load_fixture(fixture, {'relative': 'game.iso'})

    def test_review_cannot_upgrade_a_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            lab.write_reports(tmp, [dict(id='a', status='CRASH', name='game')], {})
            review = tmp / 'review.json'
            review.write_text(json.dumps(dict(id='a', status='GAMEPLAY_VERIFIED', reviewer='tester', notes='scene')))
            with self.assertRaises(ValueError):
                lab.import_review(tmp, review)

    @unittest.skipIf(os.name == 'nt', 'the fake controller is a shebang script')
    def test_compile_only_uses_controller_selected_exact_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            corpus = root / "Games d'été"
            corpus.mkdir()
            source = corpus / "O'Brien.iso"
            source.write_bytes(b'disc')
            selection = root / '.gekkoaot/cache/discs/test/meta'
            selection.mkdir(parents=True)
            dol = selection.parent / 'sys/main.dol'
            dol.parent.mkdir()
            dol.write_bytes(b'DOL')
            module = root / 'module/gTEST01_recomp.so'
            module.parent.mkdir()
            module.write_bytes(b'module')
            generated = module.parent / 'dolrecomp-output/generated/main.dol'
            generated.parent.mkdir(parents=True)
            generated.write_bytes(dol.read_bytes())
            bridge, sdk = root / 'bridge.so', root / 'sdk.txt'
            bridge.write_bytes(b'bridge')
            sdk.write_text('manifest')
            metadata = dict(schema=1, source=str(source), dol=str(dol), module=str(module),
                            bridge=str(bridge), sdk_manifest=str(sdk), native_os='0', secondary_modules=[],
                            dol_fnv64=lab.boot.fnv64(dol), sdk_fnv64=lab.boot.fnv64(sdk))
            (selection / 'aot-launch.json').write_text(json.dumps(metadata))
            controller = root / 'controller with spaces'
            controller.write_text('#!' + sys.executable + '\nimport sys\nsys.exit(0)\n')
            controller.chmod(0o755)
            (root / 'CMakeLists.txt').write_text('fixture')
            (root / 'version.txt').write_text('fixture')
            argv = ['lab', '--corpus', str(corpus), '--results', str(root / 'results'),
                    '--profile', 'full', '--compile-only', '--controller', str(controller), '--runner', str(controller)]
            with patch.object(lab, 'ROOT', root), patch.object(sys, 'argv', argv):
                self.assertEqual(lab.main(), 0)
            row = json.loads((root / 'results/results.json').read_text())['games'][0]
            self.assertEqual(row['compile']['exit_code'], 0)
            self.assertEqual(row['status'], 'NOT_TESTED')
            self.assertEqual(source.read_bytes(), b'disc')
            # A mismatched original DOL invalidates the selection, never picked by mtime.
            generated.write_bytes(b'wrong revision')
            with patch.object(lab, 'ROOT', root):
                self.assertIsNone(lab.selection(source))

    def test_screenshot_comparison_and_stale_baseline(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('optional Pillow unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            actual, baseline = tmp / 'actual.png', tmp / 'baseline.png'
            Image.new('RGB', (8, 8), 'red').save(actual)
            Image.new('RGB', (8, 8), 'red').save(baseline)
            spec = dict(baseline=str(baseline), sha256=lab.boot.sha256(baseline), max_mean_error=0)
            self.assertTrue(lab.compare_screenshot(actual, spec, tmp / 'diff.png')['passed'])
            Image.new('RGB', (8, 8), 'blue').save(actual)
            self.assertFalse(lab.compare_screenshot(actual, spec, tmp / 'diff.png')['passed'])
            baseline.write_bytes(b'tampered')
            with self.assertRaises(ValueError):
                lab.compare_screenshot(actual, spec, tmp / 'diff.png')

    def test_playability_needs_gameplay_and_reviewed_presented_enhanced(self):
        row = dict(status='BOOT_OK', diagnostics=dict(gameplay='UNVERIFIED'))
        self.assertFalse(lab.gameplay_acceptance(row)['playable_with_enhanced'])
        row['status'] = 'GAMEPLAY_VERIFIED'
        row['diagnostics']['gameplay'] = 'EXPLICITLY_REVIEWED'
        self.assertFalse(lab.gameplay_acceptance(row)['playable_with_enhanced'])
        row['diagnostics']['enhanced_measurement'] = dict(graph_measurement=dict(presented_changed_samples=2))
        self.assertFalse(lab.gameplay_acceptance(row)['playable_with_enhanced'])
        row['review'] = dict(enhanced=dict(outcome='VALID_EFFECT', rationale='Reviewed same-scene lighting and geometry'))
        self.assertTrue(lab.gameplay_acceptance(row)['playable_with_enhanced'])
        row['status'] = 'MENU_OK'
        self.assertFalse(lab.gameplay_acceptance(row)['playable_with_enhanced'])

    def test_aspect_launches_never_certify_widescreen(self):
        import compat_aspect_campaign as aspects
        row=dict(status='GAMEPLAY_VERIFIED',acceptance=dict(gameplay_verified=True,enhanced_verified=True))
        pair=aspects.pair_result(row,row)
        self.assertTrue(pair['gameplay_both'])
        self.assertEqual(pair['widescreen_correctness'],'UNVERIFIED')
        self.assertFalse(aspects.pair_result(row,None)['gameplay_both'])

    def test_inventory_cli(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            corpus = root / 'corpus'
            corpus.mkdir()
            (corpus / 'Super Monkey Ball.iso').write_bytes(b'not executed')
            result = subprocess.run([sys.executable, str(ROOT / 'tools/compat_lab.py'), '--corpus', str(corpus),
                                     '--results', str(root / 'reports'), '--inventory-only'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            rows = json.loads((root / 'reports/results.json').read_text())['games']
            self.assertEqual(rows[0]['status'], 'NOT_TESTED')


if __name__ == '__main__':
    unittest.main()
