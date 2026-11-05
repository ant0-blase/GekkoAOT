#include "cpu/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void func_80500100(CPUState*);
void func_80500120(CPUState*);
void func_80500140(CPUState*);
void func_80500160(CPUState*);
void func_80500180(CPUState*);
void func_805001A0(CPUState*);
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned translations, overlap_translations;
u32 gekkoaot_rel_translate_address(CPUState* cpu, u32 address) {
  (void)cpu;
  if (address == 0x805a4000) { overlap_translations++; return 0x805a4100; }
  if (address == 0x805a4100) { overlap_translations++; return 0x805a4200; }
  if (address == 0x805a3c18) { translations++; return 0x812c0260; }
  return address;
}
u32 gekkoaot_rel_translate_published_pointer(CPUState* cpu,u32 destination,u32 value) {
  (void)destination;
  return gekkoaot_rel_translate_address(cpu,value);
}
static void prepare(CPUState* cpu, u32 pc) {
  cpu->pc = pc; cpu->lr = 0x80010000; cpu->downcount = 100;
  cpu->exception = 0;
}
int main(void) {
  CPUState cpu;
  CHECK(cpu_init(&cpu));
  memcpy(cpu.ram + 0x5a3c18, "\xc3\x12\x00\x00", 4);
  memcpy(cpu.ram + 0x12c0260, "\x81\x2c\x10\x20", 4);
  prepare(&cpu, 0x80500100); func_80500100(&cpu);
  CHECK(cpu.gpr[28] == 0x812c0260 && translations == 1);
  // Cross a native guest-function boundary. The pointer itself is already
  // architectural, even though the callee has no compile-time address proof.
  prepare(&cpu, 0x80500120); func_80500120(&cpu);
  CHECK(cpu.gpr[5] == 0x812c1020 && translations == 1);
  prepare(&cpu, 0x80500140); func_80500140(&cpu);
  CHECK(cpu.gpr[28] == 0x805a3c18); // Unrelocated scalar remains bit-exact.
  prepare(&cpu, 0x80500160); func_80500160(&cpu);
  CHECK(cpu.gpr[5] == 0x812c1020);
  // Loaded heap pointers are not relocation literals, including when their
  // numerical value lies in the synthetic compilation window.
  memcpy(cpu.ram + 0x12c0260, "\x80\x5a\x3c\x18", 4);
  prepare(&cpu, 0x80500160); func_80500160(&cpu);
  CHECK(cpu.gpr[5] == 0x805a3c18);
  memcpy(cpu.ram + 0x5a4100, "\x12\x34\x56\x78", 4);
  memcpy(cpu.ram + 0x5a4200, "\xde\xad\xc0\xde", 4);
  prepare(&cpu, 0x80500180); func_80500180(&cpu);
  CHECK(cpu.gpr[28] == 0x805a4100 && cpu.gpr[5] == 0x12345678);
  CHECK(overlap_translations == 1);
  overlap_translations = 0;
  prepare(&cpu, 0x805001a0); func_805001A0(&cpu);
  CHECK(!memcmp(cpu.ram + 0x10000, "\x80\x5a\x41\x00", 4));
  CHECK(overlap_translations == 1);
  cpu_free(&cpu);
  puts("REL pointer materialization AOT regression passed");
  return 0;
}
