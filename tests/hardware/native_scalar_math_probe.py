#!/usr/bin/env python3
"""Exact native LLVM scalar math versus disabled AOT and pinned CPU semantics."""
from pathlib import Path
import argparse, os, subprocess
ROOT=Path(__file__).resolve().parents[2]
def run(args,**kw):return subprocess.run([str(x) for x in args],check=True,**kw)
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--source',type=Path,default=ROOT/'.gekkoaot/src/DolRecomp')
 p.add_argument('--build',type=Path,default=ROOT/'.gekkoaot/build/DolRecomp')
 p.add_argument('--output',type=Path,default=ROOT/'.gekkoaot/diagnostics/native-perf-v301/scalar-v316')
 a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
 run(['cmake','--build',a.build,'--target','dr_llvm','dr_cpu','-j4'])
 libdir=subprocess.check_output(['llvm-config-20','--libdir'],text=True).strip()
 fixture=a.output/'fixture'
 run(['c++','-O2','-std=c++17','-DDOLRECOMP_ENABLE_LLVM=1','-I'+str(a.source/'src'),
      ROOT/'tests/hardware/native_scalar_math/fixture.cpp',a.build/'libdr_llvm.a',a.build/'libdr_ir.a',a.build/'libdr_frontend.a',
      '-L'+libdir,'-Wl,-rpath,'+libdir,'-lLLVM','-lz','-o',fixture])
 ref=a.output/'reference.c';ref.write_bytes(subprocess.check_output(['git','-C',a.source,'show','HEAD:src/cpu/cpu.c']))
 common=['cc','-O2','-fno-fast-math','-ffp-contract=off','-I'+str(a.source/'src'),'-I'+str(a.source/'src/cpu')]
 run(common+[f'-Dppc_{n}=reference_{n}' for n in ['fadds','fsubs','fmuls']]+['-c',ref,'-o',a.output/'reference.o'])
 run(['objcopy']+[f'--keep-global-symbol=reference_{n}' for n in ['fadds','fsubs','fmuls']]+[a.output/'reference.o'])
 for memory in [0,1]:
  d=a.output/f'memory-{memory}';d.mkdir(exist_ok=True)
  for enabled,suffix in [(0,'off'),(1,'on')]:
   env=dict(os.environ,DOLRECOMP_NATIVE_SCALAR_MATH=str(enabled))
   output=run([fixture,d/(suffix+'.o'),d/(suffix+'.ll'),memory,'_'+suffix],env=env,stdout=subprocess.PIPE,text=True)
   assert '|native-scalar-math=316-'+suffix in output.stdout,output.stdout
   text=(d/(suffix+'.ll')).read_text();assert ('fp.f64.finite' in text)==bool(enabled)
  exe=d/'check'
  run(common+[ROOT/'tests/hardware/native_scalar_math/check.c',d/'on.o',d/'off.o',a.output/'reference.o',a.build/'libdr_cpu.a','-lm','-o',exe])
  run([exe])
if __name__=='__main__':main()
