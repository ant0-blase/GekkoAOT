// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"

#include <cstdlib>
#include <iostream>
#include <memory>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

using GekkoAOT::Native::HostRuntime;

int main() {
  auto runtime = std::make_unique<HostRuntime>();
  auto& cpu = runtime->Cpu();

  // An indirect jump can enter an instruction inside a translated function,
  // rather than at one of its compiled basic-block labels. MKDD uses a table
  // that targets a standalone blr at the end of a short setter.
  cpu.pc = 0x80010000;
  cpu.ctr = 0x80020013;
  cpu.lr = 0x80010004;
  cpu.instruction_fallback(&cpu, 0x4e800420, cpu.pc); // bctr
  CHECK(cpu.exception == 0 && cpu.pc == 0x80020010);
  CHECK(cpu.ctr == 0x80020013 && cpu.lr == 0x80010004);
  cpu.instruction_fallback(&cpu, 0x4e800020, cpu.pc); // interior blr
  CHECK(cpu.exception == 0 && cpu.pc == 0x80010004);

  cpu.pc = 0x80011000;
  cpu.ctr = 0x80021003;
  cpu.lr = 0x80001000;
  cpu.instruction_fallback(&cpu, 0x4e800421, cpu.pc); // bctrl
  CHECK(cpu.exception == 0 && cpu.pc == 0x80021000);
  CHECK(cpu.lr == 0x80011004 && cpu.ctr == 0x80021003);
  cpu.instruction_fallback(&cpu, 0x4e800020, cpu.pc); // blr
  CHECK(cpu.exception == 0 && cpu.pc == 0x80011004);

  // bclrl tests the specified CR bit, reads the old LR as its target, then
  // writes the link address even when the branch is not taken.
  cpu.pc = 0x80020100;
  cpu.lr = 0x80030003;
  cpu.ctr = 7;
  cpu.cr = 0x20000000; // CR0.EQ = 1 (BI=2)
  cpu.instruction_fallback(&cpu, 0x4d820021, cpu.pc); // bclrl BO=12, BI=2
  CHECK(cpu.exception == 0 && cpu.pc == 0x80030000);
  CHECK(cpu.lr == 0x80020104 && cpu.ctr == 7);

  cpu.pc = 0x80020110;
  cpu.lr = 0x80030003;
  cpu.cr = 0;
  cpu.instruction_fallback(&cpu, 0x4d820021, cpu.pc);
  CHECK(cpu.exception == 0 && cpu.pc == 0x80020114);
  CHECK(cpu.lr == 0x80020114 && cpu.ctr == 7);

  // BO=0 decrements CTR and branches while CTR is nonzero and CR0.LT is 0.
  cpu.pc = 0x80020200;
  cpu.lr = 0x80031003;
  cpu.ctr = 2;
  cpu.instruction_fallback(&cpu, 0x4c000020, cpu.pc); // bclr BO=0, BI=0
  CHECK(cpu.exception == 0 && cpu.pc == 0x80031000 && cpu.ctr == 1);
  cpu.pc = 0x80020210;
  cpu.ctr = 1;
  cpu.instruction_fallback(&cpu, 0x4c000020, cpu.pc);
  CHECK(cpu.exception == 0 && cpu.pc == 0x80020214 && cpu.ctr == 0);

  // BCCTR with a decrementing BO is invalid in the PowerPC decoder.
  cpu.pc = 0x80020300;
  cpu.instruction_fallback(&cpu, 0x4c000420, cpu.pc);
  CHECK((cpu.exception & 0x80000000u) != 0);

  std::cout << "AOT interior indirect branch/return tests passed\n";
}
