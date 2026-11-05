"""Prove partial stores defer publication, then deliver IRQ and resume exactly."""
from pathlib import Path
import os, subprocess
root=Path(__file__).resolve().parents[2]
build=Path(os.environ.get('GEKKOAOT_HARDWARE_TEST_BUILD',root/'.gekkoaot/diagnostics/byte-reverse-tests'))
source=Path(os.environ.get('GEKKOAOT_HARDWARE_TEST_SOURCE',root/'.gekkoaot/src/DolRecomp'))
subprocess.run(['cmake','-S',str(root/'tests/hardware/byte_reverse'),'-B',str(build),'-G','Ninja',f'-DDOLRECOMP_SOURCE={source}','-DCMAKE_BUILD_TYPE=Release',f'-DLLVM_DIR={os.environ.get("GEKKOAOT_LLVM_DIR","/usr/lib/llvm20")}/lib/cmake/llvm'],check=True)
subprocess.run(['cmake','--build',str(build),'--target',*[f'wgpipe_partial_check_{m}_{l}' for m in range(4) for l in range(2)],'-j4'],check=True)
for m in range(4):
    out=[subprocess.check_output([str(build/f'wgpipe_partial_check_{m}_{l}')],text=True).strip() for l in range(2)]
    assert out[0]==out[1],(m,out)
    print(f'mode={m}: seven partial writes, full-line IRQ and exact retry match {out[0]}',flush=True)
