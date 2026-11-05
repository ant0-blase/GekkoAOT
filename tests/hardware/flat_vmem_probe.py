"""Differential LLVM VM lowering, scalar/PSQ, late revocation and fallback."""
from pathlib import Path
import os
import subprocess
root=Path(__file__).resolve().parents[2]
build=root/'.gekkoaot/diagnostics/byte-reverse-tests'
subprocess.run(['cmake','-S',str(root/'tests/hardware/byte_reverse'),'-B',str(build),'-G','Ninja',
    f'-DDOLRECOMP_SOURCE={root/".gekkoaot/src/DolRecomp"}','-DCMAKE_BUILD_TYPE=Release',
    f'-DLLVM_DIR={os.environ.get("GEKKOAOT_LLVM_DIR","/usr/lib/llvm20")}/lib/cmake/llvm'],check=True)
subprocess.run(['cmake','--build',str(build),'--target',
    *[f'flat_vmem_check_{m}_{l}' for m in range(4) for l in range(2)],'-j4'],check=True)
for mode in range(4):
    values=[subprocess.check_output([str(build/f'flat_vmem_check_{mode}_{lower}')],text=True).strip() for lower in range(2)]
    assert values[0]==values[1],(mode,values)
    print(f'mode={mode}: reference/native scalar+PSQ+revocation {values[0]} matches',flush=True)
