"""Requires a graphical session and the materialized Aurora bridge."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class StorageRollover(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('GEKKOAOT_RUN_GPU_TESTS') == '1', 'opt-in GPU test')
    def test_segmented_storage_with_fog(self):
        with tempfile.TemporaryDirectory(prefix='gekkoaot-storage-') as tmp:
            result = subprocess.run([sys.executable, str(Path(__file__).with_name('storage_rollover_probe.py'))],
                                    cwd=tmp, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45)
        self.assertEqual(result.returncode, 0, result.stdout[-12000:])
        self.assertIn('STORAGE_STRESS_COMPLETED', result.stdout)
        rollovers = [line for line in result.stdout.splitlines() if 'phase=rollover ' in line]
        self.assertEqual(len(rollovers), 2, result.stdout[-12000:])
        for line in rollovers:
            self.assertIn('from_segment=0 to_segment=1', line)
            self.assertIn('segment_capacity=67108864', line)
        for failure in ('unexpected-overflow', 'rollover-rejected', '[fatal]', 'Validation Error'):
            self.assertNotIn(failure, result.stdout)


if __name__ == '__main__':
    unittest.main()
