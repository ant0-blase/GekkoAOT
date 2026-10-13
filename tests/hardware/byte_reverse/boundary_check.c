#include "cpu/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/module_abi.h"
const GekkoAOTModuleDesc* gekkoaot_get_module(void);

void func_80018000(CPUState*);
void func_80018100(CPUState*);
void func_80018200(CPUState*);
void func_80018300(CPUState*);
void func_80018400(CPUState*);
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s pc=%08x msr=%08x r31=%08x\n", __LINE__, #c, cpu.pc, cpu.msr, cpu.gpr[31]); exit(1); } } while (0)
static unsigned calls;
static bool pending;
static bool legacy;
static bool host(CPUState* cpu, u32 token) {
  if (token == PPC_HOST_CALL_NATIVE_REGION_QUERY) return false;
  if (token == 0xfffffff4u) {
    if (pending && !legacy && cpu->external_write_count != 255) ++cpu->external_write_count;
    return pending;
  }
  if (token != 0xfffc0000u) return false;
  ++calls;
  cpu->gpr[3] = cpu->timebase >> 32;
  cpu->gpr[4] = cpu->timebase;
  cpu->pc = cpu->lr;
  if (cpu->external_write_count != 255) ++cpu->external_write_count;
  return true;
}
int main(int argc, char** argv) {
  legacy = argc > 1 && strcmp(argv[1], "--no-barrier") == 0;
  CPUState cpu;
  CHECK(cpu_init(&cpu));
  for (unsigned n = 0; n < 256; ++n) {
    cpu_reset(&cpu);
    cpu.gpr[1] = 0x80030000;
    cpu.gpr[31] = 0x87650000 + n;
    cpu.gpr[20] = cpu.gpr[21] = 0xdeadbeef;
    cpu.timebase = 0x12345678abcdef00ull + n;
    const u32 initial_msr = 0x2032u | ((n & 1) ? 0x8000u : 0);
    cpu.msr = initial_msr;
    cpu.pc = 0x80018000;
    cpu.lr = 0x81000000;
    cpu.downcount = 100000;
    cpu.cycle_budget = 200000;
    cpu.host_call = host;
    pending = (n & 2) != 0;
    calls = 0;
    unsigned boundaries = 0;
    unsigned interrupt_boundaries = 0;
    while (cpu.pc != 0x81000000 && boundaries++ < 20) {
      cpu.external_write_count = cpu.external_read_count = 0;
      CHECK(gekkoaot_get_module()->dispatch(&cpu, cpu.pc));
      if (cpu.pc == 0x8001831c) {
        ++interrupt_boundaries;
        CHECK((cpu.msr & 0x8000) != 0);
        CHECK(cpu.gpr[1] == 0x8002ffe0); // caller has not unwound
        CHECK(cpu.lr == 0x80018024);
      }
    }
    CHECK(interrupt_boundaries == ((pending && (n & 1)) ? 1u : 0u));
    CHECK(cpu.pc == 0x81000000);
    CHECK(cpu.gpr[1] == 0x80030000);
    CHECK(cpu.gpr[31] == 0x87650000 + n);
    CHECK(cpu.gpr[20] == (u32)(cpu.timebase >> 32));
    CHECK(cpu.gpr[21] == (u32)cpu.timebase);
    CHECK(cpu.msr == initial_msr);
    CHECK(calls == 1);
  }
  cpu_free(&cpu);
  puts("production superchain stops exactly at pending EE enable; nested clock, stack and registers preserved");
}
