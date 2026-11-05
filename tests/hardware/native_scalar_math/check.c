#include "cpu/cpu.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define DECL(pc) void func_##pc##_on(CPUState*);void func_##pc##_off(CPUState*);
#define GROUP(pc) DECL(pc##20) DECL(pc##30) DECL(pc##40) DECL(pc##50) DECL(pc##60) DECL(pc##70)
GROUP(800100) GROUP(800101) GROUP(800102) DECL(80010400)
#define ROW(pc,suffix) {func_##pc##20##suffix,func_##pc##30##suffix,func_##pc##40##suffix,func_##pc##50##suffix,func_##pc##60##suffix,func_##pc##70##suffix}
static void (*functions[2][3][6])(CPUState*)={
 {ROW(800100,_off),ROW(800101,_off),ROW(800102,_off)},
 {ROW(800100,_on),ROW(800101,_on),ROW(800102,_on)}};
void reference_fadds(CPUState*,u8,u8,u8);
void reference_fsubs(CPUState*,u8,u8,u8);
void reference_fmuls(CPUState*,u8,u8,u8);
static void (*reference[3])(CPUState*,u8,u8,u8)={reference_fadds,reference_fsubs,reference_fmuls};
static uint64_t rng=0x31647648123ab87ull;
static uint64_t random64(void){rng^=rng<<13;rng^=rng>>7;return rng^=rng<<17;}
static f64 value(uint64_t bits){f64 v;memcpy(&v,&bits,8);return v;}
static f64 single(uint32_t bits){f32 v;memcpy(&v,&bits,4);return (f64)v;}
static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static int compare(CPUState* got,CPUState* expected,unsigned cases){
 // Account for the fixture's instruction retirement/return, independently
 // checked against the disabled AOT version. All other bytes must match.
 expected->pc=got->pc;expected->downcount=got->downcount;
 if(!memcmp(got,expected,sizeof(*got)))return 0;
 fprintf(stderr,"NATIVE_SCALAR_FAIL case=%u fpscr=%08x/%08x cr=%08x/%08x\n",cases,got->fpscr,expected->fpscr,got->cr,expected->cr);
 for(unsigned i=0;i<sizeof(*got);++i)if(((uint8_t*)got)[i]!=((uint8_t*)expected)[i]){fprintf(stderr,"first differing CPU byte=%u\n",i);break;}
 return 1;
}
int main(void){
 static const uint64_t edges[]={0,0x8000000000000000ull,0x3ff0000000000000ull,0xbff0000000000000ull,1,
  0x0010000000000000ull,0x000fffffffffffffull,0x7fefffffffffffffull,0x7ff0000000000000ull,
  0xfff0000000000000ull,0x7ff8000000123456ull,0x7ff0000000123456ull,0xfff800123456789aull,
  0xfff000123456789aull,0x3810000000000000ull,0x380fffffc0000000ull,0x36a0000000000000ull,
  0x47efffffe0000000ull,0x47effffff0000000ull,0x3ff0000010000000ull,0x3feffffff0000000ull};
 CPUState initial,ref,off,on;unsigned cases=0,n=sizeof(edges)/sizeof(*edges);
 for(unsigned mode=0;mode<32;++mode)for(unsigned op=0;op<3;++op)for(unsigned i=0;i<40000;++i){
  memset(&initial,0,sizeof(initial));initial.downcount=100000;initial.msr=1u<<13;initial.lr=0x81234564;initial.cr=0x87654321;
  initial.fpr[1]=0.25;initial.ps1[1]=-7.25;initial.ps1[2]=17.125;initial.ps1[3]=-19.0625;
  initial.fpr[2]=i<n*n?value(edges[i%n]):(i&1?single((uint32_t)random64()):value(random64()));
  initial.fpr[3]=i<n*n?value(edges[i/n%n]):(i&1?single((uint32_t)random64()):value(random64()));
  initial.fpscr=mode<8?mode:((uint32_t)random64()&~7u)|(mode&7u);
  ppc_fpscr_control_updated(&initial);
  unsigned variant=i%6,d=1+variant/2,rc=variant%2;
  initial.pc=0x80010000+op*0x100+d*0x20+rc*0x10;
  memcpy(&ref,&initial,sizeof(ref));memcpy(&off,&initial,sizeof(off));memcpy(&on,&initial,sizeof(on));
  reference[op](&ref,d,2,3);if(rc)ref.cr=(ref.cr&~0x0f000000u)|((ref.fpscr>>28u)<<24u);
  functions[0][op][variant](&off);ppc_fpscr_control_updated(&initial);functions[1][op][variant](&on);
  ++cases;
  if(compare(&off,&ref,cases)||compare(&on,&ref,cases)||memcmp(&off,&on,sizeof(off)))return 1;
 }
 for(unsigned i=0;i<100000;++i){
  memset(&initial,0,sizeof(initial));initial.downcount=100000;initial.msr=1u<<13;initial.lr=0x81234564;initial.pc=0x80010400;
  initial.fpr[2]=value(random64());initial.fpr[3]=i&1?single(random64()):value(random64());
  initial.fpscr=(uint32_t)random64();ppc_fpscr_control_updated(&initial);
  memcpy(&ref,&initial,sizeof(ref));memcpy(&off,&initial,sizeof(off));memcpy(&on,&initial,sizeof(on));
  reference[0](&ref,2,2,3);reference[1](&ref,3,2,3);reference[2](&ref,2,2,3);
  func_80010400_off(&off);ppc_fpscr_control_updated(&initial);func_80010400_on(&on);
  ++cases;if(compare(&off,&ref,cases)||compare(&on,&ref,cases)||memcmp(&off,&on,sizeof(off)))return 1;
 }
 for(unsigned op=0;op<3;++op)for(unsigned enabled=0;enabled<2;++enabled){
  memset(&on,0,sizeof(on));on.msr=1u<<13;on.lr=0x81234564;on.pc=0x80010020+op*0x100;ppc_fpscr_control_updated(&on);
  double begin=seconds();
  for(unsigned i=0;i<3000000;++i){on.downcount=100000;on.pc=0x80010020+op*0x100;on.fpr[2]=0.71875001234+(i&1023)*0.00001;on.fpr[3]=0.34375151234;functions[enabled][op][0](&on);}
  printf("NATIVE_SCALAR_BENCH op=%u enabled=%u ns=%.3f\n",op,enabled,(seconds()-begin)*1e9/3000000);
 }
 printf("NATIVE_SCALAR_PASS cases=%u full_cpu_state=1\n",cases);return 0;
}
