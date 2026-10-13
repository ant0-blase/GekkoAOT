#include "cpu/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#define GEKKOAOT_EXPORT
#include "module/native_hardware_shims.inc"
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"%d: %s\n",__LINE__,#c);exit(1); } } while(0)
void func_80017000(CPUState*);
static unsigned accepted, rejected, full_calls, partial_calls;
static u64 digest=1469598103934665603ull, seed=0x244123456789ull;
static u8 gathered[32];
static u64 random64(void) { seed ^= seed<<13;seed ^= seed>>7;return seed ^= seed<<17; }
static void hash(const void* p,size_t n) { const u8* b=p;for(size_t i=0;i<n;++i){digest^=b[i];digest*=1099511628211ull;} }
static void state(const CPUState* c) {
 hash(c,offsetof(CPUState,external_read));hash(&c->downcount,sizeof(c->downcount));
 hash(&c->cycle_budget,sizeof(c->cycle_budget));hash(&c->ram_size,sizeof(c->ram_size));hash(&c->exram_size,sizeof(c->exram_size));
}
static void append(u64 v) {
 CHECK(accepted<8);for(unsigned i=0;i<4;++i)gathered[accepted*4+i]=(u8)(v>>(24-i*8));++accepted;
}
static void write_full(CPUState*c,u32 ea,u64 v,u8 width) {
 ++full_calls;
 CHECK(ea==0xcc008000 && width==4 && v==0x1234);
 CHECK(c->pc==0x80017014+accepted*8);
 CHECK(c->gpr[20]==0x1234+accepted && c->gpr[30]==0x81234560);
 CHECK(c->fpr[20]==1.25);
 if(accepted==7 && !rejected) {
  rejected=1;c->srr0=c->pc;c->srr1=c->msr;c->msr&=~0x8000u;c->pc=0x500;c->exception=0x40;return;
 }
 append(v);
 if(accepted==8 && c->external_write_count<255)++c->external_write_count;
}
static u64 read_unused(CPUState*c,u32 ea,u8 width){(void)c;(void)ea;(void)width;return 0;}
static u32 try_write(CPUState*c,u32 ea,u64 v,u8 width,u32 pc) {
 if(accepted>=7 || c->exception) return 0;
 CHECK(ea==0xcc008000 && width==4 && v==0x1234);
 CHECK(pc==0x80017014+accepted*8 && c->pc!=pc);
 ++partial_calls;append(v);return 1;
}
int main(void) {
 CPUState c;CHECK(cpu_init(&c));const CPUState initial=c;
 const GekkoAOTNativeHardware hw={1,sizeof(hw),read_unused,read_unused,write_full,write_full};
 const GekkoAOTNativeWgpipeTry partial={1,sizeof(partial),try_write};
 CHECK(gekkoaot_bind_native_hardware(&hw));CHECK(gekkoaot_bind_native_wgpipe_try(&partial));
 for(unsigned trial=0;trial<4096;++trial) {
  c=initial;for(unsigned n=0;n<32;++n){c.gpr[n]=random64();u64 raw=random64();memcpy(&c.fpr[n],&raw,8);raw=random64();memcpy(&c.ps1[n],&raw,8);}
  c.pc=0x80017000;c.lr=0x81234560;c.msr=0xb032;c.fpr[1]=1.25;c.gpr[3]=0x1234;
  c.downcount=100000;c.cycle_budget=200000;c.timebase=random64();
  c.external_write_count=(u8[]){0,1,254,255}[trial%4];const u8 epoch=c.external_write_count;
  c.external_write=write_full;c.external_read=read_unused;
  accepted=rejected=full_calls=partial_calls=0;memset(gathered,0,sizeof(gathered));
  func_80017000(&c);
  CHECK(accepted==7 && rejected==1 && c.pc==0x500 && c.exception==0x40);
  CHECK(c.srr0==0x8001704c && c.srr1==0xb032 && c.gpr[20]==0x123b);
  CHECK(c.external_write_count==epoch && c.fpr[20]==1.25);
#if PARTIAL_LOWERED
  CHECK(partial_calls==7 && full_calls==1);
#else
  CHECK(partial_calls==0 && full_calls==8);
#endif
  state(&c);hash(gathered,sizeof(gathered));
  c.pc=c.srr0;c.msr=c.srr1;c.exception=0;
  func_80017000(&c);
  CHECK(accepted==8 && c.pc==c.lr && c.lr==0x81234560 && c.gpr[20]==0x123c);
  CHECK(c.external_write_count==(epoch==255?255:epoch+1));
  for(unsigned n=0;n<8;++n)CHECK(gathered[n*4]==0 && gathered[n*4+1]==0 && gathered[n*4+2]==0x12 && gathered[n*4+3]==0x34);
  state(&c);hash(gathered,sizeof(gathered));
 }
 printf("%016llx\n",(unsigned long long)digest);cpu_free(&c);
}
