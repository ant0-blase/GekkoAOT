#include "cpu/cpu.h"
#include <stdio.h>
#include <stdlib.h>

void func_80014000(CPUState*);
void func_80014100(CPUState*);
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static unsigned writes;
static bool interrupt = true;
static void store(CPUState* cpu, u32 address, u64 value, u8 width) {
  CHECK(address == 0xcc008000 && width == 4 && value == 0x1234);
  CHECK(cpu->pc == 0x80014104);
  CHECK(cpu->gpr[20] == 0x1234 && cpu->gpr[30] == 0x81234560);
  CHECK(cpu->fpr[20] == 1.25);
  if (interrupt) {
    cpu->srr0 = cpu->pc;
    cpu->srr1 = cpu->msr;
    cpu->msr &= ~0x8000u;
    cpu->pc = 0x500;
    cpu->exception |= 0x40;
    interrupt = false;
    return;
  }
  ++writes;
}
int main(void) {
  CPUState cpu;
  CHECK(cpu_init(&cpu));
  for (unsigned i = 0; i < 32; ++i) { cpu.gpr[i] = 0x2000 + i; cpu.fpr[i] = i + 0.25; }
  cpu.pc = 0x80014000;
  cpu.lr = 0x81234560;
  cpu.gpr[4] = 0xcc008000;
  cpu.fpr[1] = 1.25;
  cpu.msr = 0xb032;
  cpu.downcount = 100000;
  cpu.external_write = store;
  func_80014000(&cpu);
  CHECK(writes == 0 && cpu.pc == 0x500 && cpu.exception == 0x40);
  CHECK(cpu.srr0 == 0x80014104 && cpu.srr1 == 0xb032);
  CHECK(cpu.lr == 0x80014010);
  CHECK(cpu.gpr[20] == 0x1234 && cpu.fpr[20] == 1.25);
  CHECK(cpu.gpr[21] == 0x2015); // instruction after the rejected store did not run
  cpu.pc = cpu.srr0; cpu.msr = cpu.srr1; cpu.exception = 0;
  func_80014100(&cpu); // RFI resumes the exact memory instruction
  CHECK(writes == 1 && cpu.pc == 0x80014010 && cpu.gpr[21] == 0x1236);
  func_80014000(&cpu); // resume caller continuation after the interrupted callee
  CHECK(writes == 1 && cpu.pc == 0x81234560 && cpu.lr == 0x81234560);
  CHECK(cpu.gpr[20] == 0x1235 && cpu.fpr[20] == 1.25);
  for (unsigned i = 0; i < 32; ++i) {
    if (i != 3 && i != 4 && i != 20 && i != 21 && i != 30) CHECK(cpu.gpr[i] == 0x2000 + i);
    if (i != 1 && i != 20) CHECK(cpu.fpr[i] == i + 0.25);
  }
  cpu_free(&cpu);
  puts("AOT external-store interruption, live caller state and exact RFI restart passed");
}
