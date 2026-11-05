"""Opt-in regression for every integer GX framebuffer logic operation."""
import os
from pathlib import Path
import subprocess
import sys
import unittest


class GxLogic(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('GEKKOAOT_RUN_GPU_TESTS') == '1', 'opt-in GPU test')
    def test_all_logic_ops_and_state_after_copy(self):
        result = subprocess.run([sys.executable, str(Path(__file__).with_name('gx_logic_probe.py'))],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=120)
        output = result.stdout[-12000:]
        self.assertEqual(result.returncode, 0, output)
        self.assertIn('GX_LOGIC_PIXELS_PASSED', output)
        self.assertNotIn('GPU Validation Error', output)


if __name__ == '__main__':
    unittest.main()
