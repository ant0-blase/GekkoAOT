#include "backend/llvm/llvm_backend.h"
#include "ir/dolir_builder.h"
#include "cpu/cpu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main(int argc, char** argv) {
  if (argc != 4) return 1;
  const int mode=std::atoi(argv[2]);
  // Unsigned/signed scalars, binary F64 and every quantized paired type/scale
  // are exercised by the host checker. Last pair has a host effect between
  // accesses exercise a host effect without changing owner storage.
  const u32 words[]={0x88640000,0xa0640000,0xa8640000,0x80640000,
      0x98640000,0xb0640000,0x90640000,0xc8240000,0xd8240000,
      0xe0241000,0xf0241000,0xe0249000,0xf0249000};
  DolIRModule module; dolir_module_init(&module);
  DolLLVMFunctionRange ranges[14]{};
  for(unsigned i=0;i<14;++i) {
    const u32 pc=0x80016000u+i*0x100u;
    const u32 sequence[]={i<13?words[i]:0x80640000u,
        i<13?0x4e800020u:0x80a70000u,0x80640000u,0x4e800020u};
    PPCInst decoded[4];
    const unsigned count=i<13?2:4;
    for(unsigned j=0;j<count;++j) decoded[j]=ppc_decode(sequence[j],pc+j*4u);
    if(!dolir_build_chunk(&module,decoded,count,pc)) return 2;
    ranges[i].start=module.functions[i].guest_start; ranges[i].end=module.functions[i].guest_end;
    if(!dolllvm_analyze_function_abi(&module.functions[i],&ranges[i])) return 3;
  }
  dolllvm_enable_native_services(ranges,14);
  DolLLVMOptions o{}; o.optimization_level=2; o.verify=1;
  o.target_profile=DOLLLVM_TARGET_HOST; o.fixed_memory_layout=1;
  o.state_in_memory=mode&1; o.ram_size=GC_MAIN_RAM_SIZE;
  o.native_abi_policy=mode<2?DOLLLVM_NATIVE_ABI_DISABLED:DOLLLVM_NATIVE_ABI_UNRESTRICTED;
  o.function_ranges=ranges; o.function_range_count=14;
  char fingerprint[1024];
  if(!dolllvm_codegen_fingerprint(&o,fingerprint,sizeof(fingerprint))) return 4;
  if((std::strstr(fingerprint,"native-locked-cache=308-on")!=nullptr)!=(std::atoi(argv[3])!=0)) return 5;
  const bool ok=dolir_verify(&module,stderr)&&dolllvm_emit_object(&module,argv[1],&o,stderr);
  dolir_module_free(&module);return ok?0:6;
}
