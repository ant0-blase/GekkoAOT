"""Opt-in GPU regression for immutable and mutated indexed array snapshots."""
import os
from pathlib import Path
import re
import subprocess
import sys
import unittest


class GxArrays(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('GEKKOAOT_RUN_GPU_TESTS') == '1', 'opt-in GPU test')
    def test_same_frame_mutation_and_exact_reuse(self):
        for adaptive in ('0', '1'):
            with self.subTest(adaptive=adaptive):
                env = dict(os.environ, GEKKOAOT_NATIVE_GX_ADAPTIVE_ARRAY_UPLOAD=adaptive,
                           GEKKOAOT_NATIVE_GX_CONTENT_ARRAY_CACHE='1')
                result = subprocess.run([sys.executable, str(Path(__file__).with_name('gx_array_probe.py'))],
                                        env=env, text=True, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, timeout=60)
                output = result.stdout[-18000:]
                self.assertEqual(result.returncode, 0, output)
                self.assertIn('GX_ARRAY_PIXELS_PASSED', output)
                self.assertNotIn('GPU Validation Error', output)
                if adaptive == '0':
                    hits = re.search(r'content_hits=(\d+)', output)
                    self.assertIsNotNone(hits, output)
                    self.assertGreaterEqual(int(hits[1]), 3, output)


if __name__ == '__main__':
    unittest.main()
