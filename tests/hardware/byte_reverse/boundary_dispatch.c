#include "cpu/cpu.h"
void func_80018000(CPUState*);
void func_80018100(CPUState*);
void func_80018200(CPUState*);
void func_80018300(CPUState*);
void func_80018400(CPUState*);
// Synthetic indexed entry table; the actual production module superchain is
// compiled unchanged into these tests, rather than recreated by the checker.
int dolrecomp_call(CPUState* cpu, u32 pc) {
  if (pc >= 0x80018000 && pc < 0x80018038) func_80018000(cpu);
  else if (pc >= 0x80018100 && pc < 0x80018114) func_80018100(cpu);
  else if (pc >= 0x80018200 && pc < 0x80018228) func_80018200(cpu);
  else if (pc >= 0x80018300 && pc < 0x80018324) func_80018300(cpu);
  else if (pc == 0x80018400) func_80018400(cpu);
  else return 0;
  return 1;
}
