"""Opt-in regression for EFB persistence, BP alias restoration and camera tracking."""
import os
from pathlib import Path
import subprocess
import sys
import unittest


class GxEfbContinuity(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('GEKKOAOT_RUN_GPU_TESTS') == '1', 'opt-in GPU test')
    def test_efb_and_camera(self):
        result = subprocess.run([sys.executable, str(Path(__file__).with_name('gx_efb_continuity_probe.py'))],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=120)
        output = result.stdout[-12000:]
        self.assertEqual(result.returncode, 0, output)
        self.assertIn('GX_EFB_CONTINUITY_PIXELS_PASSED', output)
        self.assertIn('GX_CAMERA_SIGNATURE_PASSED', output)
        self.assertNotIn('GPU Validation Error', output)


if __name__ == '__main__':
    unittest.main()
