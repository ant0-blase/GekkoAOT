"""Opt-in GPU regression for depth-copy format 0xB."""

import os
from pathlib import Path
import subprocess
import sys
import unittest


class Z16rCopy(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('GEKKOAOT_RUN_GPU_TESTS') == '1', 'opt-in GPU test')
    def test_depth_copy_format_0xb_uses_depth_conversion(self):
        probe = Path(__file__).with_name('z16r_copy_probe.py')
        result = subprocess.run([sys.executable, str(probe)], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45)
        output = result.stdout[-12000:]
        self.assertEqual(result.returncode, 0, output)
        self.assertIn('Z16R_COPY_PROBE_COMPLETED', output)
        self.assertIn('GEKKOAOT_NATIVE_GX_EFB_COPY_V2=1', output)
        self.assertIn('dest_phys=00010000 stride=64 fmt=59 depth=1', output)
        self.assertNotIn('GPU Validation Error', output)


if __name__ == '__main__':
    unittest.main()
