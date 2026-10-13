#!/usr/bin/env python3
"""Execute LLVM paired regions with/without coalescing against exact helpers.

Run after preparing DolRecomp with the v292 patch. The fixture preserves first
FP restart PCs and interior entries, and emits x86-v2 and AArch64 objects too.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]


def run(args, env=None, **kwargs):
    return subprocess.run([str(a) for a in args], env=env, check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT/'.gekkoaot/src/DolRecomp')
    parser.add_argument('--build', type=Path, default=ROOT/'.gekkoaot/build/DolRecomp')
    parser.add_argument('--output', type=Path,
                        default=ROOT/'.gekkoaot/diagnostics/native-perf-v292/fp-region')
    parser.add_argument('--llvm-config', default='llvm-config-20')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    assert 'GEKKOAOT_FP_REGION_COALESCE_V292' in (
        args.source/'src/backend/llvm/register_state.cpp').read_text(), 'v292 not prepared'
    run(['cmake', '--build', args.build, '--target', 'dr_llvm', 'dr_cpu', '-j2'])
    try:
        libdir = subprocess.check_output([args.llvm_config, '--libdir'], text=True).strip()
    except FileNotFoundError:
        libdir = subprocess.check_output(['llvm-config', '--libdir'], text=True).strip()
    fixture = args.output/'fixture'
    run([os.environ.get('CXX','c++'), '-O2', '-std=c++17', '-DDOLRECOMP_ENABLE_LLVM=1',
         '-I'+str(args.source/'src'), ROOT/'tests/hardware/fp_region/fixture.cpp',
         args.build/'libdr_llvm.a', args.build/'libdr_ir.a', args.build/'libdr_frontend.a',
         '-L'+libdir, '-Wl,-rpath,'+libdir, '-lLLVM', '-lz', '-o', fixture])
    base_env = dict(os.environ, DOLRECOMP_FP_STATE_SSA='0',
                    DOLRECOMP_NATIVE_ADDRESS_LOWERING='0')
    base_env.pop('DOLRECOMP_ADAPTIVE_HOT_FUNCTIONS', None)
    results = []
    for memory in (0,1):
        for all_entries in (0,1):
            directory = args.output/f'memory{memory}-all{all_entries}'
            directory.mkdir(exist_ok=True)
            counts = {}
            for enabled, suffix in ((0,'off'),(1,'on')):
                env = dict(base_env, DOLRECOMP_FP_REGION_COALESCE=str(enabled),
                           DOLRECOMP_REL_ALL_INSTRUCTION_ENTRYPOINTS=str(all_entries))
                emitted = run([fixture,directory/(suffix+'.o'),directory/(suffix+'.ll'),memory,
                               '_'+suffix],env=env,stdout=subprocess.PIPE,text=True)
                assert '|fp-region-coalesce=292-'+suffix in emitted.stdout, emitted.stdout
                text = (directory/(suffix+'.ll')).read_text()
                body = re.search(r'define [^\n]*@func_80010000_budget_'+suffix+
                                 r'\(.*?\n\}',text,re.S).group()
                counts[suffix] = len(re.findall(r'call.*@ppc_fp_available', body))
                long_body = re.search(r'define [^\n]*@func_80010700_budget_'+suffix+
                                      r'\(.*?\n\}',text,re.S).group()
                long_count = len(re.findall(r'call.*@ppc_fp_available', long_body))
                assert long_count == (65 if all_entries or not enabled else 3), long_count
            assert counts['off'] == 21, counts
            assert counts['on'] == (21 if all_entries else 2), counts
            executable = directory/'check'
            run([os.environ.get('CC','cc'),'-O2','-fno-fast-math','-ffp-contract=off',
                 '-I'+str(args.source/'src'),ROOT/'tests/hardware/fp_region/check.c',
                 directory/'on.o',directory/'off.o',args.build/'libdr_cpu.a','-lm',
                 '-o',executable])
            result = run([executable],stdout=subprocess.PIPE,text=True)
            print(result.stdout.strip(),flush=True)
            results.append({'state_memory':memory,'all_entries':all_entries,
                            'fp_gate_sites':counts,'result':result.stdout.strip()})
    (args.output/'results.json').write_text(json.dumps(results,indent=2)+'\n')


if __name__ == '__main__':
    main()
