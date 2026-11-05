#include "cpu/cpu.h"
#include <stdio.h>
void func_80010000(CPUState *);
int main(void) {
  CPUState cpu;
  if(!cpu_init(&cpu)) return 1;
  for(unsigned i=0;i<20000;++i) {
    cpu.fpscr=0;cpu.msr=1u<<13;ppc_fpscr_control_updated(&cpu);
    double result=0, addend=(i%19)+0.25;
    if(!ppc_fma(&cpu,1.5,2.0,addend,true,false,false,&result) || result!=3.0+addend)
      return 3;
    cpu.pc=0x80010000;cpu.lr=0x81234560;cpu.ctr=1+(i%17);
    cpu.gpr[4]=0x80004000;cpu.exception=0;cpu.reserve_valid=0;
    write_be32(cpu.ram+0x4000,i);
    const unsigned expected=i+cpu.ctr;
    func_80010000(&cpu);
    if(read_be32(cpu.ram+0x4000)!=expected || cpu.exception || cpu.pc!=cpu.lr) {
      fprintf(stderr,"PGO semantic failure i=%u value=%u expected=%u pc=%08x exception=%u\n",
              i,read_be32(cpu.ram+0x4000),expected,cpu.pc,cpu.exception);
      return 2;
    }
  }
  cpu_free(&cpu);puts("PGO guest semantics: 20000 cases passed");return 0;
}
