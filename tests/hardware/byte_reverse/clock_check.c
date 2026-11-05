#include "cpu/cpu.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void func_80014000(CPUState*);
void func_80014100(CPUState*);
void func_80014200(CPUState*);
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
static unsigned calls;
static u64 random_state = 0x91234567890abcdeull;
static u64 digest = 1469598103934665603ull;
static CPUState reset_state;
static u64 random64(void) {
  random_state ^= random_state << 13; random_state ^= random_state >> 7;
  return random_state ^= random_state << 17;
}
static void hash(const void* p, size_t n) {
  const unsigned char* b = p;
  for (size_t i = 0; i < n; ++i) { digest ^= b[i]; digest *= 1099511628211ull; }
}
static void hash_state(const CPUState* cpu) {
  // Every non-pointer byte (including padding initialized by cpu_reset).
  hash(cpu, offsetof(CPUState, external_read));
  hash(&cpu->ram_size, sizeof(cpu->ram_size));
  hash(&cpu->downcount, sizeof(cpu->downcount));
  hash(&cpu->cycle_budget, sizeof(cpu->cycle_budget));
  hash(&cpu->exram_size, sizeof(cpu->exram_size));
}
static bool host_call(CPUState* cpu, u32 token) {
  if (token == PPC_HOST_CALL_NATIVE_REGION_QUERY) return false;
  if (token != 0xfffc0000u && token != 0xfffc0001u) return false;
  ++calls;
  if (token == 0xfffc0000u) { cpu->gpr[3] = cpu->timebase >> 32; cpu->gpr[4] = cpu->timebase; }
  else cpu->gpr[3] = cpu->timebase;
  cpu->pc = cpu->lr;
  if (cpu->external_write_count < 255) ++cpu->external_write_count;
  return true;
}
static void setup(CPUState* cpu, u64 timebase, u8 epoch, bool intercept) {
  *cpu = reset_state;
  for (unsigned n = 0; n < 32; ++n) {
    cpu->gpr[n] = random64();
    const u64 raw = random64(); memcpy(&cpu->fpr[n], &raw, sizeof(raw));
    const u64 pair = random64(); memcpy(&cpu->ps1[n], &pair, sizeof(pair));
  }
  cpu->timebase = timebase;
  cpu->external_write_count = epoch;
  cpu->pc = 0x80014100; cpu->lr = 0x81234560;
  cpu->msr = 0xb032;
  cpu->downcount = 100000; cpu->cycle_budget = 200000;
  cpu->host_call = intercept ? host_call : NULL;
}
int main(void) {
  CPUState cpu;
  CHECK(cpu_init(&cpu));
  cpu_reset(&cpu);
  reset_state = cpu;
  const u64 edges[] = {0, 1, 0xffffffffull, 0x100000000ull,
                       0xffffffff00000000ull, 0xffffffffffffffffull};
  for (unsigned n = 0; n < 4096; ++n) {
    const u64 tb = n < 6 ? edges[n] : random64();
    const u8 epoch = n < 4 ? (u8[]){0, 1, 254, 255}[n] : random64();
    const bool intercept = (n % 5) != 4;
    setup(&cpu, tb, epoch, intercept);
    func_80014100(&cpu);
    CHECK(cpu.pc == cpu.lr);
    CHECK(cpu.gpr[3] == (intercept ? (u32)(tb >> 32) : 0x1234));
    if (intercept) CHECK(cpu.gpr[4] == (u32)tb);
    CHECK(cpu.external_write_count == (intercept && epoch != 255 ? epoch + 1 : epoch));
    hash_state(&cpu);

    setup(&cpu, tb, epoch, intercept);
    const u32 r4 = cpu.gpr[4];
    cpu.pc = 0x80014200;
    func_80014200(&cpu);
    CHECK(cpu.pc == cpu.lr && cpu.gpr[4] == r4);
    CHECK(cpu.gpr[3] == (intercept ? (u32)tb : 0x1234));
    hash_state(&cpu);

    setup(&cpu, tb, epoch, intercept);
    cpu.pc = 0x80014000; cpu.fpr[1] = 1.25;
    func_80014000(&cpu);
    if (intercept && CLOCK_MODE >= 2) {
      CHECK(cpu.pc == 0x80014010); // same whole-chain event boundary
      CHECK(cpu.gpr[20] == 0x1234 && cpu.gpr[30] == 0x81234560);
      CHECK(cpu.fpr[20] == 1.25);
      hash_state(&cpu); // publish live caller registers before yielding
      func_80014000(&cpu);
    }
    CHECK(cpu.pc == 0x81234560 && cpu.gpr[20] == 0x1235);
    CHECK(cpu.fpr[20] == 1.25);
    hash_state(&cpu);
  }
  CHECK(calls == (CLOCK_LOWERED ? 0 : 3 * (4096 - 819)));
  printf("%016llx\n", (unsigned long long)digest);
  cpu_free(&cpu);
}
