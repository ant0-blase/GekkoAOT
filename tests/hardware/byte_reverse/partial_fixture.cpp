#include "backend/llvm/llvm_backend.h"
#include "ir/dolir_builder.h"
#include "cpu/cpu.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
int main(int argc, char** argv) {
  if (argc != 3) return 1;
  const int mode=std::atoi(argv[2]);
  std::vector<u32> words={0x7fc802a6,0x3a801234,0xfe800890,0x3c80cc00,0x60848000};
  for (unsigned n=0;n<8;++n) { words.push_back(0x90640000); words.push_back(0x3a940001); }
  words.push_back(0x7fc803a6);words.push_back(0x4e800020);
  std::vector<PPCInst> decoded;
  for (unsigned n=0;n<words.size();++n) decoded.push_back(ppc_decode(words[n],0x80017000+n*4));
  DolIRModule module;dolir_module_init(&module);
  if (!dolir_build_chunk(&module,decoded.data(),decoded.size(),0x80017000)) return 2;
  DolLLVMFunctionRange range{};range.start=module.functions[0].guest_start;range.end=module.functions[0].guest_end;
  if (!dolllvm_analyze_function_abi(&module.functions[0],&range)) return 3;
  dolllvm_enable_native_services(&range,1);
  DolLLVMOptions options{};options.optimization_level=2;options.verify=1;
  options.target_profile=DOLLLVM_TARGET_HOST;options.fixed_memory_layout=1;
  options.state_in_memory=mode&1;options.ram_size=GC_MAIN_RAM_SIZE;
  options.native_abi_policy=mode<2?DOLLLVM_NATIVE_ABI_DISABLED:DOLLLVM_NATIVE_ABI_UNRESTRICTED;
  options.function_ranges=&range;options.function_range_count=1;
  const bool ok=dolir_verify(&module,stderr)&&dolllvm_emit_object(&module,argv[1],&options,stderr);
  dolir_module_free(&module);return ok?0:4;
}
