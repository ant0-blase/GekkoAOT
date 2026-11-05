"""Opt-in pixel regressions for both NativeGX frontends and EFB graph order."""
import os
from pathlib import Path
import re
import signal
import shutil
import subprocess
import sys
import unittest


@unittest.skipUnless(os.environ.get('GEKKOAOT_RUN_GPU_TESTS') == '1' and
                     sys.platform.startswith('linux') and shutil.which('gamescope'),
                     'opt-in Gamescope GPU regression')
class NativeGxIrGPU(unittest.TestCase):
    def test_pixels_and_routes(self):
        directory = Path(__file__).parent
        library = os.environ.get('GEKKOAOT_NATIVE_GX_TEST_LIBRARY', str(
            directory.parents[1] / '.gekkoaot/build/NativeGX-Aurora/libgekkoaot-native-gx.so'))
        self.assertTrue(Path(library).is_file(), library)
        probes = (
            ('state', 'gx_native_state_probe.py', 'GX_NATIVE_STATE_PIXELS_PASSED'),
            ('draw', 'gx_native_draw_probe.py', 'GX_NATIVE_DRAW_PIXELS_PASSED'),
            ('matrix', 'gx_native_ir_probe.py', 'GX_NATIVE_IR_MATRIX_PIXELS_PASSED'),
            ('arrays', 'gx_array_probe.py', 'GX_ARRAY_PIXELS_PASSED'),
            ('efb', 'gx_efb_continuity_probe.py', 'GX_EFB_CONTINUITY_PIXELS_PASSED'),
            ('graphics', 'gx_settings_probe.py', 'GPU live grayscale, brightness, render scale, MSAA and sampler quality passed'),
        )
        backends = os.environ.get('GEKKOAOT_NATIVE_GX_TEST_BACKENDS', 'Vulkan,OGL').split(',')
        for backend in backends:
            for mode in ('0', '1', 'default'):
                for name, script, marker in probes:
                    if mode == 'default' and name != 'state':
                        continue
                    with self.subTest(backend=backend, fifo_ir=mode, probe=name):
                        env = {**os.environ, 'SDL_VIDEODRIVER': 'x11', 'EGL_PLATFORM': 'x11',
                               'SDL_AUDIODRIVER': 'dummy', 'GEKKOAOT_GRAPHICS_BACKEND': backend,
                               'GEKKOAOT_POST_PROCESSING': '0', 'GEKKOAOT_NATIVE_GX_DIRECT_STATE': '1',
                               'GEKKOAOT_FRAME_INTERPOLATION': '1' if name in ('state', 'draw', 'matrix', 'graphics') else '0',
                               'GEKKOAOT_NATIVE_GX_DIRECT_MATRICES': '1',
                               'GEKKOAOT_NATIVE_GX_IR_PROFILE': '1', 'GEKKOAOT_NATIVE_GX_FIFO_IR': mode,
                               'GEKKOAOT_NATIVE_GX_TEST_LIBRARY': library, 'GEKKOAOT_TEST_GX_LIBRARY': library}
                        for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'XAUTHORITY',
                                    'GEKKOAOT_GX_DIAG_FIFO_CAPTURE', 'GEKKOAOT_NATIVE_GX_FIFO_FORENSIC'):
                            env.pop(key, None)
                        if mode == 'default':
                            env.pop('GEKKOAOT_NATIVE_GX_FIFO_IR', None)
                        child = [sys.executable, str(directory / script)]
                        if name == 'arrays':
                            child.extend(['--bridge', library])
                        process = subprocess.Popen(['gamescope', '--backend', 'headless',
                            '-W', '640', '-H', '480', '-w', '640', '-h', '480', '-r', '120', '--', *child],
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            start_new_session=True)
                        try:
                            output, _ = process.communicate(timeout=40)
                        except subprocess.TimeoutExpired:
                            # Some Dawn drivers keep threads alive after a
                            # failed probe. Stop this test's whole process tree.
                            os.killpg(process.pid, signal.SIGKILL)
                            output, _ = process.communicate()
                            self.fail('GPU probe timed out: ' + output[-6000:])
                        if logs := os.environ.get('GEKKOAOT_NATIVE_GX_TEST_LOG_DIR'):
                            target = Path(logs); target.mkdir(parents=True, exist_ok=True)
                            (target / f'{name}-{backend}-ir-{mode}.log').write_text(output)
                        self.assertEqual(process.returncode, 0, output[-6000:])
                        # Gamescope can exit zero even when its child fails.
                        self.assertIn(marker, output, output[-6000:])
                        self.assertNotIn('Traceback', output, output[-6000:])
                        self.assertNotIn('GPU Validation Error', output, output[-6000:])
                        self.assertNotIn('WebGPU error ', output, output[-6000:])
                        stats = re.search(r'GEKKOAOT_NATIVE_GX_IR_STATS (.*)', output)
                        self.assertIsNotNone(stats, output[-6000:])
                        values = dict(re.findall(r'(\w+)=([\d.]+)', stats[1]))
                        expected_mode = '1' if mode == 'default' else mode
                        self.assertEqual(values['fifo_ir'], expected_mode)
                        self.assertGreater(int(values['graph_copies']), 0)
                        self.assertEqual(int(values['fifo_ops']) > 0, expected_mode == '1')
                        if name == 'matrix':
                            self.assertGreater(int(values['sdk_ops']), 0)
                            self.assertIn('GX_NATIVE_IR_PIXEL_PIXELS_PASSED', output)
                        if name == 'draw' and expected_mode == '1':
                            draws = re.search(r'GEKKOAOT_NATIVE_DRAW_IR_STATS (.*)', output)
                            self.assertIsNotNone(draws, output[-6000:])
                            counters = dict(re.findall(r'(\w+)=(\d+)', draws[1]))
                            for field in ('hits', 'compiled_draws', 'reused_draws', 'executed_draws'):
                                self.assertGreater(int(counters[field]), 0, draws[1])
                        if name == 'state' and expected_mode == '1':
                            extended = re.search(r'GEKKOAOT_NATIVE_GX_EXTENDED_STATS (.*)', output)
                            self.assertIsNotNone(extended, output[-6000:])
                            state = dict(re.findall(r'(\w+)=(\d+)', extended[1]))
                            for field in ('vertex', 'projection', 'viewport', 'transform', 'pixel',
                                          'dl_hits', 'dl_changed', 'dl_negative_hits',
                                          'dl_operations_executed', 'dl_commands_reused'):
                                self.assertGreater(int(state[field]), 0, extended[1])
                            # Three distinct mixed CP/XF/BP states (12 ops each)
                            # plus the copy/token list (2). A copy-only cache hit
                            # must not conceal a declined transform-state list.
                            self.assertEqual(int(state['dl_operations']), 38, extended[1])
                            self.assertEqual(int(state['dl_declined']), 1, extended[1])
                        for percentage in ('sdk_bypass_pct', 'fifo_decode_pct', 'state_ir_pct'):
                            self.assertGreaterEqual(float(values[percentage]), 0)
                            self.assertLessEqual(float(values[percentage]), 100)


if __name__ == '__main__':
    unittest.main()
