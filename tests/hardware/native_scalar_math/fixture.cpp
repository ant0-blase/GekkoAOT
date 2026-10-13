#include "backend/llvm/llvm_backend.h"
#include "ir/dolir_builder.h"
#include "cpu/cpu.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
static bool add(DolIRModule& module,std::vector<u32> words,u32 pc) {
  std::vector<PPCInst> decoded;
  for(auto word:words) {decoded.push_back(ppc_decode(word,pc));pc+=4;}
  return dolir_build_chunk(&module,decoded.data(),decoded.size(),pc-words.size()*4);
}
static u32 operation(unsigned op,unsigned destination,bool record=false) {
  const u32 xo[]={21,20,25};
  return (59u<<26)|(destination<<21)|(2u<<16)|
      (op==2?3u<<6:3u<<11)|(xo[op]<<1)|record;
}
int main(int argc,char** argv) {
  if(argc!=5) return 1;
  DolIRModule module;dolir_module_init(&module);
  for(unsigned op=0;op<3;++op) for(unsigned d=1;d<=3;++d) for(unsigned rc=0;rc<2;++rc)
    if(!add(module,{operation(op,d,rc!=0),0x4e800020},0x80010000+op*0x100+d*0x20+rc*0x10)) return 2;
  // Unknown f64 inputs, aliasing and repeated dependent arithmetic across
  // architectural restart points. No static lfs/f32 provenance is assumed.
  if(!add(module,{operation(0,2),operation(1,3),operation(2,2),0x4e800020},0x80010400)) return 2;
  DolLLVMOptions options{};options.optimization_level=3;options.verify=1;
  options.target_profile=DOLLLVM_TARGET_HOST;options.fixed_memory_layout=1;
  options.state_in_memory=std::atoi(argv[3]);options.symbol_suffix=argv[4];
  options.ram_size=GC_MAIN_RAM_SIZE;options.mem2_size=WII_MEM2_SIZE;
  options.emit_ir=1;options.ir_path=argv[2];
  char fingerprint[2048];
  if(!dolllvm_codegen_fingerprint(&options,fingerprint,sizeof fingerprint)) return 3;
  std::puts(fingerprint);
  bool ok=dolir_verify(&module,stderr)&&dolllvm_emit_object(&module,argv[1],&options,stderr);
  options.emit_ir=0;
  for(auto target:{DOLLLVM_TARGET_X86_64_V2,DOLLLVM_TARGET_AARCH64_GENERIC}) {
    options.target_profile=target;
    const auto name=std::string(argv[1])+".target"+std::to_string(target);
    ok=ok&&dolllvm_emit_object(&module,name.c_str(),&options,stderr);
  }
  dolir_module_free(&module);return ok?0:4;
}
