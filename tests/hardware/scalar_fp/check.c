#include "cpu/cpu.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>

bool reference_fma(CPUState*, f64, f64, f64, bool, bool, bool, f64*);
#define DECL(pc) void func_##pc(CPUState*);
DECL(80010000) DECL(80010020) DECL(80010040) DECL(80010060)
DECL(80010100) DECL(80010120) DECL(80010140) DECL(80010160)
DECL(80010200) DECL(80010220) DECL(80010240) DECL(80010260)
static void (*functions[2][4])(CPUState*) = {
  {func_80010000,func_80010020,func_80010040,func_80010060},
  {func_80010100,func_80010120,func_80010140,func_80010160}};
static uint64_t rng=0x48705353464d4101ull;
static uint64_t random64(void) { rng^=rng<<13; rng^=rng>>7; return rng^=rng<<17; }
static f64 value(uint64_t bits) { f64 x; memcpy(&x,&bits,8); return x; }
static uint64_t bits(f64 x) { uint64_t b; memcpy(&b,&x,8); return b; }
static f64 single_value(uint32_t bits) { f32 x; memcpy(&x,&bits,4); return (f64)x; }
static void prepare(CPUState* cpu, u32 fpscr) {
  cpu->msr=1u<<13; cpu->exception=0; cpu->program_exception=0;
  cpu->fpscr=fpscr; cpu->pc=0x80010000; cpu->lr=0x81234560;
  cpu->cr=0x12345678; cpu->ctr=3;
  cpu->fpr[1]=value(0x400123456789abcdull); cpu->ps1[1]=-7.25;
  ppc_fpscr_control_updated(cpu);
}
static void reference(CPUState* cpu, bool single, unsigned variant,
                      unsigned a, unsigned c, unsigned b) {
  f64 out=cpu->fpr[1];
  if (reference_fma(cpu,cpu->fpr[a],cpu->fpr[c],cpu->fpr[b],single,
                    variant&1,variant>=2,&out)) {
    cpu->fpr[1]=out; if(single) cpu->ps1[1]=out;
  }
}
static int compare(CPUState* got,CPUState* ref,unsigned test,
                   bool single,unsigned variant) {
  if(bits(got->fpr[1])==bits(ref->fpr[1]) &&
     bits(got->ps1[1])==bits(ref->ps1[1]) && got->fpscr==ref->fpscr &&
     got->exception==ref->exception && got->cr==ref->cr) return 0;
  fprintf(stderr,"FAIL case=%u single=%d variant=%u inputs=%016llx,%016llx,%016llx result=%016llx/%016llx ps1=%016llx/%016llx FPSCR=%08x/%08x\n",
          test,single,variant,(unsigned long long)bits(ref->fpr[2]),
          (unsigned long long)bits(ref->fpr[3]),(unsigned long long)bits(ref->fpr[4]),
          (unsigned long long)bits(got->fpr[1]),(unsigned long long)bits(ref->fpr[1]),
          (unsigned long long)bits(got->ps1[1]),(unsigned long long)bits(ref->ps1[1]),
          got->fpscr,ref->fpscr); return 1;
}
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }
int main(void) {
  CPUState got,ref; if(!cpu_init(&got)||!cpu_init(&ref)) return 2;
  static const uint64_t edge[] = {
    0,0x8000000000000000ull,0x3ff0000000000000ull,0xbff0000000000000ull,
    0x0010000000000000ull,1,0x000fffffffffffffull,0x7fefffffffffffffull,
    0x7ff0000000000000ull,0xfff0000000000000ull,0x7ff8000000123456ull,
    0x7ff0000000123456ull,0xfff800123456789aull,0xfff000123456789aull,
    0x3810000000000000ull,0x380fffffc0000000ull,0x36a0000000000000ull,
    0x47efffffe0000000ull,0x47effffff0000000ull,0x3ff0000010000000ull,
    0x3feffffff0000000ull,0x3e60000000000000ull};
  static const u32 controls[]={0,1,2,3,4,5,6,7,0x80,0x81,0x84,0xf8,
                              0x0207f000,0x8207f080};
  unsigned cases=0;
  if(getenv("GEKKOAOT_FP_BENCH_ONLY")) goto benchmark;
  for(unsigned mode=0;mode<sizeof(controls)/sizeof(*controls);++mode)
    for(unsigned single=0;single<2;++single)
      for(unsigned variant=0;variant<4;++variant)
        for(unsigned i=0;i<16000;++i) {
          prepare(&ref,controls[mode]);
          if(i<sizeof(edge)/sizeof(*edge)*sizeof(edge)/sizeof(*edge)*sizeof(edge)/sizeof(*edge)) {
            unsigned n=sizeof(edge)/sizeof(*edge);
            ref.fpr[2]=value(edge[i%n]); ref.fpr[3]=value(edge[i/n%n]); ref.fpr[4]=value(edge[i/n/n%n]);
          } else if(i&1) {
            ref.fpr[2]=single_value((u32)random64());
            ref.fpr[3]=single_value((u32)random64()); ref.fpr[4]=single_value((u32)random64());
          } else {
            ref.fpr[2]=value(random64()); ref.fpr[3]=value(random64()); ref.fpr[4]=value(random64());
          }
          memcpy(got.fpr,ref.fpr,sizeof(ref.fpr)); memcpy(got.ps1,ref.ps1,sizeof(ref.ps1));
          prepare(&got,controls[mode]);
          got.pc=0x80010000+single*0x100+variant*0x20;
          functions[single][variant](&got);
          ppc_fpscr_control_updated(&ref); reference(&ref,single,variant,2,3,4);
          if(compare(&got,&ref,++cases,single,variant)) return 1;
        }
  for(unsigned i=0;i<20000;++i) {
    prepare(&ref,controls[i%(sizeof(controls)/sizeof(*controls))]);
    for(unsigned r=2;r<=6;++r) ref.fpr[r]=i&1?single_value((u32)random64()):value(random64());
    memcpy(got.fpr,ref.fpr,sizeof(ref.fpr)); memcpy(got.ps1,ref.ps1,sizeof(ref.ps1));
    prepare(&got,ref.fpscr); got.pc=0x80010200; func_80010200(&got);
    ppc_fpscr_control_updated(&ref); reference(&ref,true,0,2,3,4); reference(&ref,true,3,1,5,6);
    if(compare(&got,&ref,++cases,true,4)) return 1;
  }
  // A suppressed first operation must not make the unchanged old FPR a
  // proven single input to the next finite operation.
  prepare(&ref,0x80); ref.fpr[2]=0; ref.fpr[3]=INFINITY; ref.fpr[4]=0;
  ref.fpr[5]=0.5; ref.fpr[6]=0.25;
  memcpy(got.fpr,ref.fpr,sizeof(ref.fpr)); memcpy(got.ps1,ref.ps1,sizeof(ref.ps1));
  prepare(&got,ref.fpscr); got.pc=0x80010200; func_80010200(&got);
  ppc_fpscr_control_updated(&ref); reference(&ref,true,0,2,3,4); reference(&ref,true,3,1,5,6);
  if(compare(&got,&ref,++cases,true,4)) return 1;
  for(unsigned i=0;i<2000;++i) {
    u32 control=controls[i%(sizeof(controls)/sizeof(*controls))];
    u32 raw[3]={(u32)random64(),(u32)random64(),(u32)random64()};
    prepare(&ref,control); prepare(&got,control);
    got.gpr[4]=GC_RAM_BASE+0x100;
    for(unsigned j=0;j<3;++j) {
      mem_write32(&got,got.gpr[4]+4*j,raw[j]); ref.fpr[2+j]=single_value(raw[j]);
    }
    got.pc=0x80010240; func_80010240(&got);
    ppc_fpscr_control_updated(&ref); reference(&ref,true,0,2,3,4);
    if(compare(&got,&ref,++cases,true,6) || bits(got.fpr[7])!=(0xfff8000000000000ull|ref.fpscr)) return 1;
    prepare(&got,control); prepare(&ref,control);
    for(unsigned j=0;j<3;++j) got.fpr[2+j]=ref.fpr[2+j]=single_value(raw[j]);
    got.pc=0x80010260; func_80010260(&got);
    ppc_fpscr_control_updated(&ref); reference(&ref,true,0,2,3,4);
    ref.cr=(ref.cr&~0x0f000000u)|((ref.fpscr>>28)<<24);
    if(compare(&got,&ref,++cases,true,7)) return 1;
  }
  prepare(&ref,0); ref.fpr[1]=0.125; ref.fpr[3]=0.5; ref.fpr[4]=0.0625;
  prepare(&got,0); memcpy(got.fpr,ref.fpr,sizeof(ref.fpr)); got.pc=0x80010220; func_80010220(&got);
  for(unsigned i=0;i<3;++i) reference(&ref,true,0,1,3,4);
  if(compare(&got,&ref,++cases,true,5)) return 1;
benchmark:
  // Direct FMA benchmark includes guest entry/exit and FPRF publication.
  prepare(&got,getenv("GEKKOAOT_FP_BENCH_NI")?4:0); got.fpr[2]=0.75; got.fpr[3]=0.5; got.fpr[4]=0.125;
  double start=now();
  for(unsigned i=0;i<3000000;++i) { got.pc=0x80010100; func_80010100(&got); }
  printf("SCALAR_FMA_PASS cases=%u benchmark_ns=%.2f result=%.3f\n",cases,
         (now()-start)*1e9/3000000,got.fpr[1]);
  cpu_free(&got); cpu_free(&ref); return 0;
}
