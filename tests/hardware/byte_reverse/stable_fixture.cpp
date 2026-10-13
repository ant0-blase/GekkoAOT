#include "backend/llvm/llvm_backend.h"
#include "ir/dolir_builder.h"
#include "cpu/cpu.h"
#include <cstdio>
#include <cstdlib>
int main(int argc,char** argv) {
  if(argc!=3)return 1;
  int mode=std::atoi(argv[2]);
  const u32 addresses[]={0xcc000000,0xcc00100e,0xcc003000,0xcc003004,0xcc00100e,0xcc003000};
  DolIRModule module;dolir_module_init(&module);DolLLVMFunctionRange ranges[6]{};
  for(unsigned n=0;n<6;++n) {
    u32 pc=0x80018000+n*256;
    u32 words[]={0x3a801234,0x3c800000|(addresses[n]>>16),0x60840000|(addresses[n]&0xffff),
                 (n<2 || n==4)?0xa0640000u:0x80640000u,0x3a940001,0x4e800020};
    if(n>=4)words[1]=words[2]=0x60000000; // EA passed in r4, never statically known.
    PPCInst decoded[6];for(unsigned i=0;i<6;++i)decoded[i]=ppc_decode(words[i],pc+i*4);
    if(!dolir_build_chunk(&module,decoded,6,pc))return 2;
    auto& f=module.functions[module.function_count-1];ranges[n].start=f.guest_start;ranges[n].end=f.guest_end;
    if(!dolllvm_analyze_function_abi(&f,&ranges[n]))return 3;
  }
  dolllvm_enable_native_services(ranges,6);
  DolLLVMOptions options{};options.optimization_level=2;options.verify=1;
  options.target_profile=DOLLLVM_TARGET_HOST;options.fixed_memory_layout=1;
  options.state_in_memory=mode&1;options.ram_size=GC_MAIN_RAM_SIZE;
  options.native_abi_policy=mode<2?DOLLLVM_NATIVE_ABI_DISABLED:DOLLLVM_NATIVE_ABI_UNRESTRICTED;
  options.function_ranges=ranges;options.function_range_count=6;
  bool ok=dolir_verify(&module,stderr)&&dolllvm_emit_object(&module,argv[1],&options,stderr);
  dolir_module_free(&module);return ok?0:4;
}
