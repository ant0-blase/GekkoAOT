#!/usr/bin/env python3
"""Compare the finite-result path and its disabled build against pinned PPC semantics."""
from pathlib import Path
import os
import subprocess

ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'.gekkoaot/src/DolRecomp'
BUILD=ROOT/'.gekkoaot/diagnostics/native-audit-v282/scalar-finite-tests'

def run(args,**kwargs):
    subprocess.run([str(x) for x in args],check=True,**kwargs)

def main():
    BUILD.mkdir(parents=True,exist_ok=True)
    reference=BUILD/'reference.c'
    reference.write_bytes(subprocess.check_output(['git','-C',SOURCE,'show','HEAD:src/cpu/cpu.c']))
    common=[os.environ.get('CC','cc'),'-O3','-fno-fast-math','-ffp-contract=off','-march=native','-I',SOURCE/'src','-I',SOURCE/'src/cpu']
    names=['fadds','fsubs','fmuls']
    run(common+[f'-Dppc_{n}=reference_{n}' for n in names]+['-c',reference,'-o',BUILD/'reference.o'])
    run(['objcopy']+[f'--keep-global-symbol=reference_{n}' for n in names]+[BUILD/'reference.o'])
    for enabled in [0,1]:
        obj=BUILD/f'native-{enabled}.o';exe=BUILD/f'check-{enabled}'
        run(common+[f'-DDOLRECOMP_SCALAR_FINITE_RESULT={enabled}','-c',SOURCE/'src/cpu/cpu.c','-o',obj])
        run(common+[ROOT/'tests/hardware/scalar_fp/finite_check.c',obj,BUILD/'reference.o','-lm','-o',exe])
        run([exe])

if __name__=='__main__':main()
