#include "cpu/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#define GEKKOAOT_EXPORT
#include "module/native_hardware_shims.inc"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%d: %s\n",__LINE__,#x);exit(1);}}while(0)
void func_80018000(CPUState*);void func_80018100(CPUState*);void func_80018200(CPUState*);void func_80018300(CPUState*);void func_80018400(CPUState*);void func_80018500(CPUState*);
static unsigned reject,fast_allowed,fast_count;
static uint64_t digest=1469598103934665603ull;
static void hash(const void* data,size_t size){const unsigned char*p=data;while(size--){digest^=*p++;digest*=1099511628211ull;}}
static uint64_t value(uint32_t address){return (address&0x3fffffffu)<0x0c002000?0x8123:0xfedc9876;}
static uint64_t read_full(CPUState*c,uint32_t address,uint8_t size){
 CHECK(c->gpr[20]==0x1234);CHECK((c->pc&255)==12);
 if(reject){c->srr0=c->pc;c->pc=0x500;c->exception=0x40;return 0;}
 if(c->external_read_count<255)++c->external_read_count;
 return value(address);
}
static uint64_t try_read(CPUState*c,uint32_t address,uint8_t size){
 if(!fast_allowed||reject||c->exception)return 0;
 uint32_t p=address&0x3fffffffu;
 if(!((size==2&&(p==0x0c000000||p==0x0c00100e)) ||
      (size==4&&(p==0x0c003000||p==0x0c003004))))return 0;
#if !(STABLE_MODE & 1)
 CHECK(c->gpr[20]!=0x1234); // Dirty SSA registers were never published.
#endif
 CHECK((c->pc&255)==0);
 if(c->external_read_count<255)++c->external_read_count;
 ++fast_count;return (1ull<<63)|value(address);
}
int main(){
 CPUState c;CHECK(cpu_init(&c));CPUState initial=c;
 void(*functions[])(CPUState*)={func_80018000,func_80018100,func_80018200,func_80018300,func_80018400,func_80018500};
 GekkoAOTNativeStableMmio service={1,sizeof(service),try_read};
 CHECK(!gekkoaot_bind_native_stable_mmio(NULL));CHECK(gekkoaot_bind_native_stable_mmio(&service));
 service.version=2;CHECK(!gekkoaot_bind_native_stable_mmio(&service));
 for(unsigned n=0;n<6;++n)for(unsigned trial=0;trial<128;++trial){
  c=initial;c.external_read=read_full;c.pc=0x80018000+n*256;c.lr=0x80019000;
  c.gpr[4]=n==4?(trial%7==0?0xcc006400:0xcc00100e):
                     (trial%7==0?0xcc003008:0xcc003000);
  if(n>=4&&trial%11==0)c.gpr[4]=0x80001000; // Ordinary RAM must remain a direct load.
  c.gpr[20]=0xdeadbeef;c.downcount=100000;c.cycle_budget=4096;
  c.external_read_count=trial&1?255:0;fast_allowed=trial%3==0;reject=trial%5==0;
  functions[n](&c);
  unsigned fault=reject&&!(n>=4&&trial%11==0);
  CHECK(c.gpr[20]==(fault?0x1234:0x1235));
  CHECK(c.pc==(fault?0x500:c.lr));
  hash(&c,offsetof(CPUState,external_read));hash(&c.downcount,sizeof(c.downcount));
 }
#if STABLE_LOWERED
 CHECK(fast_count>0);
#else
 CHECK(fast_count==0);
#endif
 fprintf(stderr,"mode=%d lower=%d fast=%u\n",STABLE_MODE,STABLE_LOWERED,fast_count);
 printf("%016llx\n",(unsigned long long)digest);cpu_free(&c);
}
