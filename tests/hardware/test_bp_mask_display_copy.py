"""Opt-in GPU integration test for BP mask semantics at GXCopyDisp."""
import os
from pathlib import Path
import subprocess
import sys
import unittest


class BpMaskDisplayCopy(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('GEKKOAOT_RUN_GPU_TESTS') == '1', 'opt-in GPU test')
    def test_display_copy_consumes_one_shot_bp_mask(self):
        probe = Path(__file__).with_name('bp_mask_display_copy_probe.py')
        result = subprocess.run([sys.executable, str(probe)], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45)
        output = result.stdout[-12000:]
        self.assertEqual(result.returncode, 0, output)
        self.assertIn('BP_MASK_DISPLAY_COPY_PROBE_COMPLETED', output)
        self.assertIn('GEKKOAOT_NATIVE_XFB_CACHE_V12=1 copy=1 xfb=00010000', output)
        self.assertIn('GEKKOAOT_NATIVE_XFB_CACHE_V12=1 copy=2 xfb=00030000', output)


if __name__ == '__main__':
    unittest.main()
