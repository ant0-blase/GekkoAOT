"""Exercise O3 -> IR instrumentation -> profile use, including C helpers.

Requires the prepared LLVM DolRecomp build. Artifacts remain in .gekkoaot.
"""
from pathlib import Path
import os
import subprocess
import re

ROOT=Path(__file__).resolve().parents[2]
SOURCE=ROOT/'.gekkoaot/src/DolRecomp'
LIBS=Path(os.environ.get('GEKKOAOT_TEST_DOLRECOMP_BUILD',ROOT/'.gekkoaot/build/DolRecomp'))
OUT=ROOT/'.gekkoaot/diagnostics/runtime-opt/pgo-probe'
LLVM=Path(os.environ.get('GEKKOAOT_TEST_LLVM','/usr/lib/llvm20'))

def run(args, **kw):
    return subprocess.run([str(a) for a in args],check=True,**kw)

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    fixture=OUT/'fixture'
    run(['c++','-O2','-I',SOURCE/'src',ROOT/'tests/hardware/pgo/fixture.cpp',
         LIBS/'libdr_llvm.a',LIBS/'libdr_ir.a',LIBS/'libdr_frontend.a',
         '-L',LLVM/'lib',f'-Wl,-rpath,{LLVM}/lib','-lLLVM','-lz','-o',fixture])
    env=dict(os.environ);env['DOLRECOMP_ADAPTIVE_HOT_FUNCTIONS']=''
    env['DOLRECOMP_CROSSGAME_HOT_FUNCTIONS']=''
    env['GEKKOAOT_DIRECT_SCALAR_FP']='1'
    for mode in ('off','gen','use'):
        raw=OUT/'counters.profraw';profile=OUT/'counters.profdata'
        obj=OUT/(mode+'.o');ir=OUT/(mode+'.ll');exe=OUT/mode
        run([fixture,obj,mode,raw if mode=='gen' else profile,ir],env=env)
        cmd=[LLVM/'bin/clang','-O3','-fno-fast-math','-ffp-contract=off','-I',SOURCE/'src',
             ROOT/'tests/hardware/pgo/check.c',SOURCE/'src/cpu/cpu.c',obj,'-lm','-o',exe]
        if mode=='gen': cmd+=['-fprofile-generate']
        if mode=='use': cmd+=[f'-fprofile-use={profile}']
        run(cmd)
        child=dict(env);child['LLVM_PROFILE_FILE']=str(raw)
        run([exe],env=child)
        if mode=='gen': run([LLVM/'bin/llvm-profdata','merge','-o',profile,raw])
    counts=subprocess.check_output([str(LLVM/'bin/llvm-profdata'),'show','--all-functions','--counts',str(profile)],text=True)
    (OUT/'counts.txt').write_text(counts)
    assert 'func_80010000' in counts and 'cpu_init' in counts and 'ppc_fma' in counts, 'translated/C counters missing'
    text=(OUT/'use.ll').read_text()
    assert 'function_entry_count' in text and 'branch_weights' in text, 'PGO annotations missing'
    # An external structural prior guides standard O3, but must not change
    # instrumentation/profile-use IR or take precedence over measured counts.
    prior=OUT/'crossgame-hot.txt';prior.write_text('func_80010000_budget\n')
    prior_env=dict(env);prior_env['DOLRECOMP_CROSSGAME_HOT_FUNCTIONS']=str(prior)
    for mode in ('off','gen','use'):
        ir=OUT/f'prior-{mode}.ll';obj=OUT/f'prior-{mode}.o'
        result=run([fixture,obj,mode,raw if mode=='gen' else profile,ir],env=prior_env,
                   stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        if mode=='off':
            assert re.search(r'attributes #\d+ = \{[^\n]*\bhot\b',ir.read_text())
            run([LLVM/'bin/clang','-O3','-fno-fast-math','-ffp-contract=off','-I',SOURCE/'src',
                 ROOT/'tests/hardware/pgo/check.c',SOURCE/'src/cpu/cpu.c',obj,'-lm','-o',OUT/'prior-off'])
            run([OUT/'prior-off'],env=env)
        else:
            assert 'CrossGameDB optimizer hot hints=' not in result.stderr
            assert ir.read_text()==(OUT/f'{mode}.ll').read_text(), f'{mode} IR changed by prior'
    print('CrossGameDB: O3 hot hint consumed; gen/use IR unchanged')
    # Old post-training translation policy is a negative control: confirm its
    # changed CFG cannot consume the standard training function's profile.
    hot=OUT/'legacy-hot.txt';hot.write_text('80010000\n')
    legacy=dict(env);legacy['DOLRECOMP_ADAPTIVE_HOT_FUNCTIONS']=str(hot)
    result=run([fixture,OUT/'legacy.o','use',profile,OUT/'legacy.ll'],env=legacy,
               stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    (OUT/'legacy-profile-use.log').write_text(result.stderr)
    assert 'hash mismatch' in result.stderr and 'count discarded' in result.stderr
    print('PGO standard O3 round trip passed; legacy diagnostics:',result.stderr.strip())
    # Both producers must sample the same fraction: finalization rescales the
    # entire profile. A full-rate C helper mixed with sampled guest IR would
    # otherwise dominate compiler hotness merely because of its collection mode.
    sampled=dict(env);sampled['DOLRECOMP_LLVM_OPTIONS']='-enable-sampled-instrumentation -sampled-instr-period=16 -sampled-instr-burst-duration=4'
    raw=OUT/'sampled.profraw';profile=OUT/'sampled.profdata'
    run([fixture,OUT/'sampled-gen.o','gen',raw,OUT/'sampled-gen.ll'],env=sampled)
    run([LLVM/'bin/clang','-O3','-fno-fast-math','-ffp-contract=off','-I',SOURCE/'src',
         ROOT/'tests/hardware/pgo/check.c',SOURCE/'src/cpu/cpu.c',OUT/'sampled-gen.o','-lm',
         '-fprofile-generate','-mllvm','-enable-sampled-instrumentation',
         '-mllvm','-sampled-instr-period=16','-mllvm','-sampled-instr-burst-duration=4',
         '-o',OUT/'sampled-gen'])
    sampled['LLVM_PROFILE_FILE']=str(raw);run([OUT/'sampled-gen'],env=sampled)
    run([LLVM/'bin/llvm-profdata','merge','-o',profile,raw])
    data=subprocess.check_output([str(LLVM/'bin/llvm-profdata'),'show','--all-functions','--counts',str(profile)],text=True)
    (OUT/'sampled-counts.txt').write_text(data)
    def score(text,name):
        match=re.search(r'^  '+name+r':\n.*?Block counts: \[([^\]]*)\]',text,re.M|re.S)
        assert match, name
        return max(int(x) for x in re.findall(r'\d+',match.group(1)))
    for name in ('func_80010000_budget','ppc_fma'):
        ratio=score(data,name)/score(counts,name)
        assert 0.20 < ratio < 0.30, (name,ratio)
        print(f'{name}: sampled/full ratio={ratio:.4f}')
    run([fixture,OUT/'sampled-use.o','use',profile,OUT/'sampled-use.ll'],env=env)
    run([LLVM/'bin/clang','-O3','-fno-fast-math','-ffp-contract=off','-I',SOURCE/'src',
         ROOT/'tests/hardware/pgo/check.c',SOURCE/'src/cpu/cpu.c',OUT/'sampled-use.o','-lm',
         f'-fprofile-use={profile}','-o',OUT/'sampled-use'])
    run([OUT/'sampled-use'],env=env)

if __name__=='__main__': main()
