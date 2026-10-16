"""Opt-in GPU integration: final pixels and real attachment/sampler properties."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import unittest

@unittest.skipUnless(os.environ.get('GEKKOAOT_RUN_GPU_TESTS') == '1' and sys.platform.startswith('linux')
                     and shutil.which('gamescope'), 'opt-in private Gamescope GPU test')
class GraphicsGPU(unittest.TestCase):
    def test_live_quality_and_pixels(self):
        for backend in ('Vulkan','OGL'):
            with self.subTest(backend=backend):
                env={**os.environ,'SDL_VIDEODRIVER':'x11','EGL_PLATFORM':'x11','GEKKOAOT_GRAPHICS_BACKEND':backend}
                for key in ('DISPLAY','WAYLAND_DISPLAY','XAUTHORITY'):env.pop(key,None)
                result=subprocess.run(['gamescope','--backend','headless','-W','640','-H','480',
                    '-w','640','-h','480','-r','120','--',sys.executable,
                    str(Path(__file__).with_name('gx_settings_probe.py'))],env=env,
                    capture_output=True,text=True,timeout=45)
                output=result.stdout+result.stderr
                # The compositor can return zero even if its child fails.
                self.assertIn('GPU live grayscale, brightness, render scale, MSAA and sampler quality passed',output,output[-8000:])
                self.assertNotIn('[fatal]',output,output[-8000:])
                self.assertNotIn('Traceback',output,output[-8000:])
