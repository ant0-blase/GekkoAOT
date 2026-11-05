// SPDX-License-Identifier: GPL-3.0-or-later
// Compile against the patched DolRecomp cpu.h with a C11 compiler.
#include "cpu/cpu.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

PPCMemWriteJournal g_mem_write_journal;
void* g_mem_write_journal_user;
static unsigned fallback_reads, fallback_writes, journal_writes;
static u32 last_fallback_address;
u32 mem_read32(CPUState* cpu, u32 address) {
  (void)cpu;
  ++fallback_reads;
  last_fallback_address = address;
  return 0x12345678u;
}
void mem_write32(CPUState* cpu, u32 address, u32 value) {
  (void)cpu; (void)value;
  ++fallback_writes;
  last_fallback_address = address;
}
static void Journal(u32 offset, u32 size, void* user) {
  (void)user;
  assert(offset == 0 && size == 4);
  ++journal_writes;
}

int main(void) {
  u8 mem1[64] = {0};
  u8 mem2[64] = {0};
  CPUState cpu = {0};
  cpu.ram = mem1; cpu.ram_size = sizeof(mem1);
  cpu.mem2 = mem2; cpu.mem2_size = sizeof(mem2);
  g_mem_write_journal = Journal;
  dolrecomp_mem_write32_fast(&cpu, GC_RAM_BASE, 0xaabbccddu);
  assert(dolrecomp_mem_read32_fast(&cpu, GC_RAM_BASE) == 0xaabbccddu);
  assert(dolrecomp_mem_read32_fast(&cpu, GC_RAM_UNCACHED) == 0xaabbccddu);
  assert(journal_writes == 1);
  cpu.reserve_valid = true;
  cpu.reserve_addr = GC_RAM_UNCACHED;
  dolrecomp_mem_write32_fast(&cpu, GC_RAM_BASE, 0x11223344u);
  assert(!cpu.reserve_valid && journal_writes == 2);
  dolrecomp_mem_write32_fast(&cpu, WII_MEM2_BASE, 0xdeadbeefu);
  assert(dolrecomp_mem_read32_fast(&cpu, WII_MEM2_UNCACHED) == 0xdeadbeefu);
  assert(fallback_reads == 0 && fallback_writes == 0);
  assert(dolrecomp_mem_read32_fast(&cpu, 0xcc008000u) == 0x12345678u);
  assert(last_fallback_address == 0xcc008000u && fallback_reads == 1);
  dolrecomp_mem_write32_fast(&cpu, 0xcc008000u, 1u);
  assert(last_fallback_address == 0xcc008000u && fallback_writes == 1);
  assert(dolrecomp_mem_read32_fast(&cpu, 0xcc006400u) == 0x12345678u);
  assert(last_fallback_address == 0xcc006400u && fallback_reads == 2);
  puts("Portable C mem32 RAM/aliases/reservation/journal/MMIO/WGPIPE passed");
  return 0;
}
