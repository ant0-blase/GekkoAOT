"""Verify exact memory restart and live caller state in both generated ABIs."""
from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[2]
build = root / '.gekkoaot/diagnostics/byte-reverse-tests'
subprocess.run(['cmake', '-S', str(root / 'tests/hardware/byte_reverse'), '-B', str(build),
                '-G', 'Ninja', f'-DDOLRECOMP_SOURCE={root / ".gekkoaot/src/DolRecomp"}',
                '-DCMAKE_BUILD_TYPE=Release',
                f'-DLLVM_DIR={os.environ.get("GEKKOAOT_LLVM_DIR", "/usr/lib/llvm20")}/lib/cmake/llvm'], check=True)
subprocess.run(['cmake', '--build', str(build), '--target',
                *[f'store_restart_check_{mode}' for mode in range(4)], '-j4'], check=True)
for mode in range(4):
    subprocess.run([str(build / f'store_restart_check_{mode}')], check=True)
