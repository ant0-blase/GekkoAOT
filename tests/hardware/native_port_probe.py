#!/usr/bin/env python3
"""Check fail-closed SideExitAnalysis and its analysis-only compiler path."""
from pathlib import Path
import json
import os
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = root / '.gekkoaot/src/DolRecomp'
compiler = root / '.gekkoaot/build/DolRecomp/dolrecomp'
with tempfile.TemporaryDirectory(prefix='gekkoaot-native-port-') as temporary:
    out = Path(temporary)
    test = out / 'analysis.cpp'
    test.write_text(r'''
#include "backend/llvm/side_exit_analysis.h"
#include <cassert>
#include <cstdlib>
int main(int argc, char** argv) {
  assert(argc == 2);
  DolLLVMFunctionRange ranges[2]{};
  ranges[0].start=0; ranges[0].end=8;
  ranges[1].start=0x80001000; ranges[1].end=0x80001014;
  DolIRInstruction insn[6]{};
  insn[0].op=DOLIR_OP_GUEST_LOAD; insn[0].aux=4;
  insn[0].address_domain=DOLIR_ADDRESS_MEM1;
  insn[0].address_lower=insn[0].address_upper=0x800000d4;
  insn[1]=insn[0]; insn[1].address_upper+=4; // non-singleton is not a proof
  insn[2]=insn[0]; insn[2].address_lower=insn[2].address_upper=0x81fffffe; // width overrun
  insn[3]=insn[0]; insn[3].address_domain=DOLIR_ADDRESS_FIFO;
  insn[4].op=DOLIR_OP_HELPER_CALL; insn[4].aux=0xdead; // new/unknown helper
  insn[5].op=DOLIR_OP_HELPER_CALL; insn[5].aux=DOLIR_HELPER_EXACT_PAIRED;
  DolIRBlock blocks[5]{};
  blocks[0].guest_address=0x80001000; blocks[0].raw=0x7c6c42e6;
  blocks[0].instructions=insn; blocks[0].instruction_count=6;
  blocks[0].terminator.kind=DOLIR_TERM_BRANCH;
  blocks[0].terminator.targets[0]=DOLIR_NO_BLOCK;
  blocks[0].terminator.target_addresses[0]=0; // address zero is a known target
  blocks[1].guest_address=0x80001004; blocks[1].raw=0x4e800020;
  blocks[1].terminator.kind=DOLIR_TERM_RETURN;
  blocks[2].terminator.kind=DOLIR_TERM_INDIRECT;
  blocks[3].terminator.kind=DOLIR_TERM_RFI;
  blocks[4].terminator.kind=DOLIR_TERM_FALLBACK;
  DolIRFunction fn{}; fn.guest_start=ranges[1].start; fn.guest_end=ranges[1].end;
  fn.blocks=blocks; fn.block_count=5;
  void* report=dolllvm_side_exit_begin(argv[1],ranges,2); assert(report);
  assert(dolllvm_side_exit_add(report,&fn));
  DolIRFunction zero{}; zero.guest_start=0; zero.guest_end=8;
  assert(dolllvm_side_exit_add(report,&zero));
  assert(dolllvm_side_exit_end(report,true));
  // A partial image, a duplicate range and an unadvertised range fail closed.
  report=dolllvm_side_exit_begin((std::string(argv[1])+".partial").c_str(),ranges,2);
  assert(report); assert(dolllvm_side_exit_add(report,&fn));
  assert(!dolllvm_side_exit_add(report,&fn));
  assert(!dolllvm_side_exit_end(report,true));
  report=dolllvm_side_exit_begin((std::string(argv[1])+".foreign").c_str(),ranges,2);
  assert(report); zero.guest_end=12;
  assert(!dolllvm_side_exit_add(report,&zero));
  assert(!dolllvm_side_exit_end(report,false));
}
'''.replace('#include <cstdlib>', '#include <cstdlib>\n#include <string>'))
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-O2', '-I', str(source / 'src'),
                    str(test), str(source / 'src/backend/llvm/side_exit_analysis.cpp'),
                    '-o', str(out / 'verify')], check=True)
    sdk = out / 'sdk.txt'
    sdk.write_text('# fixture\n0x80001000 OSGetTick 0xfffc0001\n')
    env = dict(os.environ, DOLRECOMP_STATIC_INTERCEPTS=str(sdk))
    subprocess.run([str(out / 'verify'), str(out / 'report.json')], env=env, check=True)
    report = json.loads((out / 'report.json').read_text())
    assert report['complete_input_scan'] and not report['proof_complete'] and not report['full_native']
    assert all(value is None for value in report['hardware'].values())
    counts = report['totals']
    for name, expected in {'regions': 2, 'memory_mem1_proven': 1, 'memory_unproven': 2,
                           'memory_fifo': 1, 'direct_target_known': 1, 'direct_target_unknown': 0,
                           'indirect_control': 1, 'rfi': 1, 'fallback_terminators': 1,
                           'unknown_helpers': 1, 'numeric_helpers': 1,
                           'sdk_getter_candidates': 1}.items():
        assert counts[name] == expected, (name, counts[name], expected)
    assert not (out / 'report.json.partial').exists()
    assert not (out / 'report.json.partial.tmp').exists()
    assert not (out / 'report.json.foreign').exists()
    # Exercise normal CLI setup/cleanup, with no object code or fake runnable header.
    header = bytearray(0x100)
    words = [0x7c6c42e6, 0x4e800020]
    for offset, value in [(0, 0x100), (0x48, 0x80001000), (0x90, len(words)*4), (0xe0, 0x80001000)]:
        struct.pack_into('>I', header, offset, value)
    dol = out / 'mini.dol'
    dol.write_bytes(header + struct.pack('>2I', *words))
    env.update(DOLRECOMP_SIDE_EXIT_ANALYSIS_ONLY='1', DOLRECOMP_LLVM_CACHE='off',
               DOLRECOMP_SIDE_EXIT_REPORT=str(out / 'cli.json'))
    subprocess.run([str(compiler), '--backend=llvm', '--cpu', 'gekko', '--gamecube',
                    '--native-abi=unrestricted', str(dol), str(out/'generated')], env=env, check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert json.loads((out / 'cli.json').read_text())['totals']['sdk_getter_candidates'] == 1
    assert not list((out / 'generated').rglob('*.o'))
    assert not list((out / 'generated').rglob('generated.h'))
    env.pop('DOLRECOMP_SIDE_EXIT_REPORT')
    failed = subprocess.run([str(compiler), '--backend=llvm', '--cpu', 'gekko', '--gamecube',
                             str(dol), str(out / 'missing-report')], env=env, capture_output=True)
    assert failed.returncode != 0 and b'analysis-only requires' in failed.stderr
    # Malformed SDK names/tokens must never produce a misleading complete report.
    sdk.write_text('0x80001000 OSGetTick 0xfffc0001 trailing-junk\n')
    env['DOLRECOMP_SIDE_EXIT_REPORT'] = str(out / 'bad-sdk.json')
    failed = subprocess.run([str(compiler), '--backend=llvm', '--cpu', 'gekko', '--gamecube',
                             str(dol), str(out / 'bad-sdk')], env=env, capture_output=True)
    assert failed.returncode != 0 and not (out / 'bad-sdk.json').exists()
print('native-port: bounds, address zero, unknown helpers, getter recognition, coverage, failure cleanup and analysis-only passed')
