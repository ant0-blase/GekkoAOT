#include "cpu/cpu.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

void reference_fadds(CPUState*,u8,u8,u8);
void reference_fsubs(CPUState*,u8,u8,u8);
void reference_fmuls(CPUState*,u8,u8,u8);
typedef void (*Operation)(CPUState*,u8,u8,u8);
static Operation native_ops[]={ppc_fadds,ppc_fsubs,ppc_fmuls};
static Operation reference_ops[]={reference_fadds,reference_fsubs,reference_fmuls};
static uint64_t rng=0x48705353464d4101ull;
static uint64_t random64(void){rng^=rng<<13;rng^=rng>>7;return rng^=rng<<17;}
static f64 value(uint64_t bits){f64 x;memcpy(&x,&bits,8);return x;}
static f64 single(uint32_t bits){f32 x;memcpy(&x,&bits,4);return (f64)x;}
static double seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(void){
 static const uint64_t edges[]={0,0x8000000000000000ull,0x3ff0000000000000ull,0xbff0000000000000ull,
  0x0010000000000000ull,1,0x000fffffffffffffull,0x7fefffffffffffffull,0x7ff0000000000000ull,
  0xfff0000000000000ull,0x7ff8000000123456ull,0x7ff0000000123456ull,0xfff800123456789aull,
  0xfff000123456789aull,0x3810000000000000ull,0x380fffffc0000000ull,0x36a0000000000000ull,
  0x47efffffe0000000ull,0x47effffff0000000ull,0x3ff0000010000000ull,0x3feffffff0000000ull};
 CPUState ref,got;unsigned cases=0,n=sizeof(edges)/sizeof(*edges);
 for(unsigned mode=0;mode<32;++mode)for(unsigned op=0;op<3;++op)for(unsigned i=0;i<40000;++i){
  memset(&ref,0,sizeof(ref));ref.msr=1u<<13;ref.pc=0x80010000;ref.lr=0x81234564;ref.cr=0x87654321;
  ref.ps1[1]=-7.25;ref.ps1[2]=17.125;ref.ps1[3]=-19.0625;ref.fpr[1]=0.25;
  ref.fpr[2]=i<n*n?value(edges[i%n]):(i&1?single((u32)random64()):value(random64()));
  ref.fpr[3]=i<n*n?value(edges[i/n%n]):(i&1?single((u32)random64()):value(random64()));
  u32 status=mode<8?mode:((u32)random64()&~7u)|(mode&7u);
  ref.fpscr=status;ppc_fpscr_control_updated(&ref);ref.fpscr=status;
  memcpy(&got,&ref,sizeof(got));unsigned d=1+i%3;
  reference_ops[op](&ref,d,2,3);native_ops[op](&got,d,2,3);
  if(memcmp(&got,&ref,sizeof(got))){fprintf(stderr,"FINITE_RESULT_FAIL case=%u mode=%u op=%u d=%u fpscr=%08x/%08x\n",cases,mode,op,d,got.fpscr,ref.fpscr);return 1;}
  ++cases;
 }
 memset(&got,0,sizeof(got));got.fpscr=0;ppc_fpscr_control_updated(&got);
 for(unsigned op=0;op<3;++op){
  double elapsed[2];
  for(unsigned version=0;version<2;++version){
   got.fpr[2]=0.71875;got.fpr[3]=0.34375;got.fpscr=0;
   Operation fn=version?native_ops[op]:reference_ops[op];double start=seconds();
   for(unsigned i=0;i<10000000;++i){got.fpr[2]=0.71875+(i&1023)*0.00001;fn(&got,1,2,3);}
   elapsed[version]=seconds()-start;
  }
  printf("FINITE_RESULT_BENCH op=%u reference_ns=%.2f native_ns=%.2f reduction=%.2f%%\n",op,elapsed[0]*100,elapsed[1]*100,100*(1-elapsed[1]/elapsed[0]));
 }
 printf("FINITE_RESULT_PASS cases=%u complete-cpu-state=1\n",cases);return 0;
}
