"""Compare full guest state and yield boundaries with native SDK clock lowering."""
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
                *[f'sdk_clock_check_{mode}_{lower}' for mode in range(4) for lower in range(2)],
                '-j4'], check=True)
digests = []
for mode in range(4):
    outputs = [subprocess.check_output([str(build / f'sdk_clock_check_{mode}_{lower}')], text=True).strip()
               for lower in range(2)]
    assert outputs[0] == outputs[1], (mode, outputs)
    digests.append(outputs[0])
    print(f'mode={mode}: baseline/lowered state, cycles, yield, fallback match {outputs[0]}', flush=True)
print('All four ABI/state modes match; lowered SDK callbacks = 0', flush=True)
