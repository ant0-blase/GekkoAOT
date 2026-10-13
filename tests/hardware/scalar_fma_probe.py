"""Build and check generated scalar FMA against the upstream exact helper.

Run from any directory with the DolRecomp source prepared in .gekkoaot/src.
The oracle is compiled from its pinned git revision, before local optimizations.
"""
from pathlib import Path
import subprocess
import os

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / '.gekkoaot/src/DolRecomp'
BUILD = ROOT / '.gekkoaot/diagnostics/runtime-opt/fp-tests'


def run(args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    reference = BUILD / 'reference.c'
    reference.write_bytes(subprocess.check_output(
        ['git', '-C', str(SOURCE), 'show', 'HEAD:src/cpu/cpu.c']))
    obj = BUILD / 'reference.o'
    run([os.environ.get('CC', 'cc'), '-O2', '-fno-fast-math', '-ffp-contract=off',
         '-Dppc_fma=reference_fma', '-I', SOURCE / 'src/cpu', '-I', SOURCE / 'src', '-c', reference, '-o', obj])
    run(['objcopy', '--keep-global-symbol=reference_fma', obj])
    run(['cmake', '-S', ROOT / 'tests/hardware/scalar_fp', '-B', BUILD, '-G', 'Ninja',
         f'-DDOLRECOMP_SOURCE={SOURCE}', f'-DREFERENCE_OBJECT={obj}',
         '-DCMAKE_BUILD_TYPE=Release',
         f'-DLLVM_DIR={os.environ.get("GEKKOAOT_LLVM_DIR", "/usr/lib/llvm20")}/lib/cmake/llvm'])
    run(['cmake', '--build', BUILD, '--target', 'scalar_fp_check', 'scalar_fp_check_memory', '-j8'])
    run([BUILD / 'scalar_fp_check'])
    run([BUILD / 'scalar_fp_check_memory'])


if __name__ == '__main__':
    main()
