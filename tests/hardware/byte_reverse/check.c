#include "cpu/cpu.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void func_80012000(CPUState*);
void func_80012020(CPUState*);
void func_80012040(CPUState*);
void func_80012060(CPUState*);
void func_80012080(CPUState*);
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%d: %s\n", __LINE__, #condition); exit(1); } } while (0)
static void prepare(CPUState* cpu, u32 pc) {
  cpu->pc = pc;
  cpu->lr = 0x81234560u;
  cpu->gpr[0] = 0xaaaaaaaa; // RA=0 addresses do not read r0.
  cpu->gpr[4] = 0x80010002u;
  cpu->downcount = 100;
  cpu->exception = 0;
}
int main(void) {
  CPUState cpu;
  CHECK(cpu_init(&cpu));
  for (unsigned value = 0; value <= 0xffffu; ++value) {
    cpu.ram[0x10002] = (u8)value;
    cpu.ram[0x10003] = (u8)(value >> 8);
    prepare(&cpu, 0x80012000u);
    func_80012000(&cpu);
    CHECK(cpu.gpr[3] == value);
    CHECK(cpu.pc == cpu.lr);
    prepare(&cpu, 0x80012040u);
    func_80012040(&cpu);
    CHECK(cpu.gpr[4] == value);

    memset(cpu.ram + 0x10000, 0xa5, 8);
    prepare(&cpu, 0x80012020u);
    cpu.gpr[3] = 0xdead0000u | value;
    func_80012020(&cpu);
    CHECK(cpu.ram[0x10002] == (u8)value);
    CHECK(cpu.ram[0x10003] == (u8)(value >> 8));
    CHECK(cpu.ram[0x10001] == 0xa5 && cpu.ram[0x10004] == 0xa5);
  }
  // Keep the existing 32-bit reverse operations covered as well.
  prepare(&cpu, 0x80012080u);
  cpu.gpr[3] = 0x1234abcdu;
  func_80012080(&cpu);
  CHECK(memcmp(cpu.ram + 0x10002, "\xcd\xab\x34\x12", 4) == 0);
  prepare(&cpu, 0x80012060u);
  func_80012060(&cpu);
  CHECK(cpu.gpr[3] == 0x1234abcdu);
  cpu_free(&cpu);
  puts("Halfword and word byte-reverse AOT checks passed (65536 halfwords)");
  return 0;
}
