"""Differential native device lowering: all non-pointer CPU state and events."""
from pathlib import Path
import os
import subprocess
root=Path(__file__).resolve().parents[2]
build=Path(os.environ.get('GEKKOAOT_HARDWARE_TEST_BUILD',root/'.gekkoaot/diagnostics/byte-reverse-tests'))
source=Path(os.environ.get('GEKKOAOT_HARDWARE_TEST_SOURCE',root/'.gekkoaot/src/DolRecomp'))
subprocess.run(['cmake','-S',str(root/'tests/hardware/byte_reverse'),'-B',str(build),'-G','Ninja',
               f'-DDOLRECOMP_SOURCE={source}', '-DCMAKE_BUILD_TYPE=Release',
               f'-DLLVM_DIR={os.environ.get("GEKKOAOT_LLVM_DIR","/usr/lib/llvm20")}/lib/cmake/llvm'],check=True)
subprocess.run(['cmake','--build',str(build),'--target',
               *[f'native_hardware_check_{m}_{l}' for m in range(4) for l in range(3)],
               *[f'stable_mmio_check_{m}_{l}' for m in range(4) for l in range(2)],'-j4'],check=True)
for mode in range(4):
    outputs=[subprocess.check_output([str(build/f'native_hardware_check_{mode}_{lower}')],text=True).strip() for lower in range(3)]
    assert outputs[0]==outputs[1]==outputs[2],(mode,outputs)
    symbols=subprocess.check_output(['nm','-u',str(build/f'hardware-{mode}-1.o')],text=True)
    baseline=subprocess.check_output(['nm','-u',str(build/f'hardware-{mode}-0.o')],text=True)
    assert 'gekkoaot_' not in baseline
    for name in ['try_stable_mmio_read','vi_read16','pi_write32','wgpipe_write']:
        assert 'gekkoaot_'+name in symbols,(mode,name,symbols)
    partial_symbols=subprocess.check_output(['nm','-u',str(build/f'hardware-{mode}-2.o')],text=True)
    assert 'gekkoaot_try_wgpipe_write' in partial_symbols
    print(f'mode={mode}: reference / native MMIO / partial WG state/event digest {outputs[0]} matches',flush=True)

for mode in range(4):
    outputs=[subprocess.check_output([str(build/f'stable_mmio_check_{mode}_{lower}')],text=True).strip() for lower in range(2)]
    assert outputs[0]==outputs[1],(mode,outputs)
    print(f'mode={mode}: stable MMIO / fully published CPU-state digest {outputs[0]} matches',flush=True)
