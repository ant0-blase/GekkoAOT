#!/usr/bin/env python3
"""Exercise the hybrid memory/SSA policy with upstream semantic fixtures."""
import os
import re
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
source = root / '.gekkoaot/src/DolRecomp'
build = root / '.gekkoaot/build/DolRecomp'
output = root / '.gekkoaot/diagnostics/fpssa-v228'
output.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, DOLRECOMP_FP_STATE_SSA='1')


def run(args):
    subprocess.run([str(arg) for arg in args], env=env, check=True)


run(['cmake', '--build', build, '--target', 'llvm_fixture', 'dr_cpu', '-j2'])
for mode in ('hybrid', 'profile-fp'):
    if mode == 'profile-fp':
        addresses = sorted(set(re.findall(r'@func_([0-9A-Fa-f]{8})',
                                         (output/'hybrid.ll').read_text())))
        assert addresses, 'fixture did not emit guest functions'
        manifest=output/'fixture-hot.txt'
        manifest.write_text('\n'.join(addresses)+'\n')
        env.update(DOLRECOMP_FP_STATE_SSA='0', DOLRECOMP_NATIVE_HOT_RESIDENCY='fp',
                   DOLRECOMP_ADAPTIVE_HOT_FUNCTIONS=str(manifest))
    probes=output/(mode+'.o')
    run([build/'llvm_fixture', probes, output/(mode+'.ll')])
    for fixture in ('paired_single', 'llvm_semantics'):
        executable = output / (fixture + '-'+mode)
        test_source = source / 'tests' / (fixture + '_test.c')
        if fixture == 'llvm_semantics':
            # v274 exits for pending IRQs only. This fixture has no host.
            patched = (source/'src/backend/llvm/special_instructions.cpp').read_text()
            if 'GEKKOAOT_DOLRECOMP_EE_PENDING_QUERY_V274' in patched:
                text = test_source.read_text().replace(
                    'cpu.msr == 0x8000u && cpu.pc == 0x80003D24u',
                    'cpu.msr == 0x8000u && cpu.pc == 0x81234564u')
                test_source = output/'llvm_semantics-current-policy.c'
                test_source.write_text(text)
        run([os.environ.get('CC', 'cc'), '-O2', '-I', source / 'src', test_source,
             Path(str(probes)+'.state-memory'), build/'libdr_cpu.a', '-lm', '-o', executable])
        run([executable])
        print(f'{fixture}: {mode} FPR/paired-single state passed', flush=True)
