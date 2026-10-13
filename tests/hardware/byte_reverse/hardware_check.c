#include "cpu/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#define GEKKOAOT_EXPORT
#include "module/native_hardware_shims.inc"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define DECLARE(pc) void func_##pc(CPUState*);
DECLARE(80015000) DECLARE(80015100) DECLARE(80015200) DECLARE(80015300)
DECLARE(80015400) DECLARE(80015500) DECLARE(80015600) DECLARE(80015700)
DECLARE(80015800) DECLARE(80015900) DECLARE(80015A00) DECLARE(80015B00)
DECLARE(80015C00) DECLARE(80015D00)
static void (*functions[])(CPUState*) = {func_80015000,func_80015100,func_80015200,func_80015300,
func_80015400,func_80015500,func_80015600,func_80015700,func_80015800,func_80015900,
func_80015A00,func_80015B00,func_80015C00,func_80015D00};
static unsigned calls, native_calls, generic_calls, reject, phase, try_calls, partial_possible;
static u64 digest = 1469598103934665603ull, seed = 0x243123456789ull;
static u64 random64(void) { seed ^= seed << 13; seed ^= seed >> 7; return seed ^= seed << 17; }
static void hash(const void* p, size_t size) {
  const u8* bytes = p; for (size_t n=0;n<size;++n) { digest ^= bytes[n]; digest *= 1099511628211ull; }
}
static void state(const CPUState* c) {
  hash(c, offsetof(CPUState, external_read)); hash(&c->ram_size,sizeof(c->ram_size));
  hash(&c->downcount,sizeof(c->downcount)); hash(&c->cycle_budget,sizeof(c->cycle_budget));
  hash(&c->exram_size,sizeof(c->exram_size));
}
static void event(CPUState* c, u32 ea, u64 value, u8 width, bool write) {
  CHECK(c->pc >= 0x80015008 && c->pc <= 0x80015d08 && (c->pc & 255) == 8);
  CHECK(c->gpr[20] == 0x12340000 + phase);
  ++calls; hash(&ea,sizeof(ea));hash(&value,sizeof(value));hash(&width,sizeof(width));hash(&write,sizeof(write));
  if (reject && calls == 1) {
    c->srr0=c->pc; c->srr1=c->msr; c->msr &= ~0x8000u; c->pc=0x500;c->exception=0x40;
  } else if (write) { if (!partial_possible || (ea & 0x3fffffe0u) != 0x0c008000u)
      if (c->external_write_count < 255) ++c->external_write_count; }
  else { if (c->external_read_count < 255) ++c->external_read_count; }
}
static u64 read_common(CPUState* c,u32 ea,u8 width) { event(c,ea,0,width,false);return 0xfedcba9876548001ull; }
static void write_common(CPUState*c,u32 ea,u64 value,u8 width) { event(c,ea,value,width,true); }
static u64 generic_read(CPUState*c,u32 ea,u8 width) { ++generic_calls; return read_common(c,ea,width); }
static void generic_write(CPUState*c,u32 ea,u64 v,u8 width) { ++generic_calls;write_common(c,ea,v,width); }
static u64 native_read(CPUState*c,u32 ea,u8 width) { ++native_calls;return read_common(c,ea,width); }
static void native_write(CPUState*c,u32 ea,u64 v,u8 width) { ++native_calls;write_common(c,ea,v,width); }
static u32 native_try(CPUState* c,u32 ea,u64 value,u8 width,u32 pc) {
  if (!partial_possible || reject || c->exception || (ea & 0x3fffffe0u) != 0x0c008000u) return 0;
  CHECK(width == 1 || width == 2 || width == 4 || width == 8);
  CHECK(c->pc != pc); // The full CPUState publication was actually skipped.
  CHECK(c->downcount == 100000);
  ++try_calls; ++calls;
  const bool write = true;
  hash(&ea,sizeof(ea));hash(&value,sizeof(value));hash(&width,sizeof(width));hash(&write,sizeof(write));
  return 1;
}
int main(void) {
  CPUState c;CHECK(cpu_init(&c)); const CPUState initial = c;
  CHECK(!gekkoaot_bind_native_hardware(NULL));
  CHECK(!gekkoaot_bind_native_wgpipe_try(NULL));
  for (phase=0;phase<3;++phase) {
    if (phase == 1) {
      GekkoAOTNativeHardware services={1,sizeof(services),native_read,native_read,native_write,native_write};
      CHECK(gekkoaot_bind_native_hardware(&services));
      services.version=99;CHECK(!gekkoaot_bind_native_hardware(&services));
      services.version=1;services.size=0;CHECK(!gekkoaot_bind_native_hardware(&services));
      services.size=sizeof(services);services.pi_write32=NULL;CHECK(!gekkoaot_bind_native_hardware(&services));
      GekkoAOTNativeWgpipeTry partial={1,sizeof(partial),native_try};
      CHECK(gekkoaot_bind_native_wgpipe_try(&partial));
      partial.version=99;CHECK(!gekkoaot_bind_native_wgpipe_try(&partial));
    }
    for (unsigned trial=0;trial<256;++trial) for (unsigned index=0;index<14;++index) {
      c=initial;for(unsigned n=0;n<32;++n) {
        c.gpr[n]=random64();u64 bits=random64();memcpy(&c.fpr[n],&bits,8);
        bits=random64();memcpy(&c.ps1[n],&bits,8);
      }
      c.pc=0x80015000+index*256;c.lr=0x81234560;c.msr=0xb032;c.hid2=0xa0000000;
      c.gqr[0]=0;c.gpr[4]=0xcc008000;c.gpr[20]=0x12340000+phase;
      c.downcount=100000;c.cycle_budget=200000;
      c.external_read_count=c.external_write_count=(u8[]){0,1,254,255}[trial%4];
      c.external_read=phase==2?NULL:generic_read;c.external_write=phase==2?NULL:generic_write;
      c.fpr[1]=1.25;c.ps1[1]=-2.5;
      // Resume after an interrupt with a different live address. The static
      // proof must not replace the actual register on an interior entry.
      if (trial % 16 == 0) { c.pc += 8; c.gpr[4] = 0xcc006000; }
      const bool ram_load = trial % 32 == 0 &&
          (index == 0 || index == 1 || index == 8 || index == 9 || index == 12 || index == 13);
      if (ram_load) {
        c.pc = 0x80015008 + index * 256; c.gpr[4] = 0x80010000;
        c.ram[0x10000] = 0x81; c.ram[0x10001] = 0x23;
      }
      partial_possible = trial % 4 == 0;
      const bool pending = trial % 32 == 2 && (index == 3 || index == 4 || index == 5);
      if (pending) c.exception = 0x40;
      calls=0;reject=trial&1;
      functions[index](&c);
      if (phase != 2 && !ram_load) CHECK(calls >= 1);
      else CHECK(calls == 0);
      if (ram_load) CHECK(c.gpr[3] == (index == 1 ? 0xffff8123u : 0x8123u));
      if (phase != 2 && pending) CHECK(c.pc == 0x80015008 + index * 256 && c.exception == 0x40 && c.gpr[20] == 0x12340000 + phase);
      else if (phase != 2 && reject && !ram_load) CHECK(c.pc==0x500 && c.exception==0x40 && c.gpr[20]==0x12340000+phase);
      else CHECK(c.pc==c.lr && c.gpr[20]==0x12340001+phase);
      state(&c);hash(&calls,sizeof(calls));
    }
  }
#if !HARDWARE_LOWERED
  CHECK(native_calls == 0);
#else
  CHECK(native_calls > 0);
#endif
#if HARDWARE_LOWERED == 2
  CHECK(try_calls > 0);
#else
  CHECK(try_calls == 0);
#endif
  fprintf(stderr,"mode=%d lowered=%d generic=%u direct=%u partial=%u\n",HARDWARE_MODE,HARDWARE_LOWERED,generic_calls,native_calls,try_calls);
  printf("%016llx\n",(unsigned long long)digest);cpu_free(&c);
}
