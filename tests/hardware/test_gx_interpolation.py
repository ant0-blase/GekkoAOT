"""Optional real GPU presentation checks, using independently authored FIFO data."""
import os
from pathlib import Path
import subprocess
import sys
import unittest

class GxInterpolation(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('GEKKOAOT_RUN_GPU_TESTS') == '1' and
                         sys.platform.startswith('linux'), 'opt-in GPU/X11 test')
    def test_motion_presentation(self):
        result = subprocess.run([sys.executable, str(Path(__file__).with_name('gx_interpolation_probe.py'))],
                                capture_output=True, text=True, timeout=40,
                                env={**os.environ, 'SDL_VIDEODRIVER': 'x11',
                                     'GEKKOAOT_FRAME_INTERPOLATION': '1',
                                     'GEKKOAOT_INTERPOLATION_MODE': 'motion'})
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output[-12000:])
        self.assertIn('GPU motion halfway translation and static HUD passed', output)
        self.assertIn('GPU motion scene-cut rejection passed', output)
        self.assertNotIn('Validation Error', output)

if __name__ == '__main__': unittest.main()
