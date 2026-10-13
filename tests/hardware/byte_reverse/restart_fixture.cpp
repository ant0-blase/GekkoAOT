#include "backend/llvm/llvm_backend.h"
#include "ir/dolir_builder.h"
#include "cpu/cpu.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
  if (argc != 3) return 1;
  const int mode = std::atoi(argv[2]);
  DolIRModule module;
  dolir_module_init(&module);
  const u32 caller[] = {
    0x7fc802a6, // mflr r30
    0x3a801234, // li r20,0x1234
    0xfe800890, // fmr f20,f1
    0x480000f5, // bl 0x80014100
    0x3a940001, // addi r20,r20,1
    0x7fc803a6, // mtlr r30
    0x4e800020,
  };
  const u32 callee[] = {0x38601234, 0x90640000, 0x3ab40002, 0x4e800020};
  for (unsigned index = 0; index < 2; ++index) {
    const auto* words = index ? callee : caller;
    const unsigned count = index ? 4 : 7;
    const u32 pc = 0x80014000 + index * 0x100;
    PPCInst decoded[7];
    for (unsigned n = 0; n < count; ++n) decoded[n] = ppc_decode(words[n], pc + n * 4);
    if (!dolir_build_chunk(&module, decoded, count, pc)) return 2;
  }
  DolLLVMFunctionRange ranges[2]{};
  for (unsigned index = 0; index < 2; ++index) {
    ranges[index].start = module.functions[index].guest_start;
    ranges[index].end = module.functions[index].guest_end;
    if (!dolllvm_analyze_function_abi(&module.functions[index], &ranges[index])) return 3;
  }
  dolllvm_enable_native_services(ranges, 2);
  if (mode >= 2 && ((ranges[0].abi_flags & DOLLLVM_FUNCTION_ABI_NATIVE) == 0 ||
                    (ranges[1].abi_flags & DOLLLVM_FUNCTION_ABI_NATIVE) == 0)) return 5;
  DolLLVMOptions options{};
  options.optimization_level = 2;
  options.verify = 1;
  options.target_profile = DOLLLVM_TARGET_HOST;
  options.fixed_memory_layout = 1;
  options.state_in_memory = mode & 1;
  options.ram_size = GC_MAIN_RAM_SIZE;
  options.mem2_size = 0;
  options.native_abi_policy = mode < 2 ? DOLLLVM_NATIVE_ABI_DISABLED : DOLLLVM_NATIVE_ABI_UNRESTRICTED;
  options.function_ranges = ranges;
  options.function_range_count = 2;
  const bool ok = dolir_verify(&module, stderr) && dolllvm_emit_object(&module, argv[1], &options, stderr);
  dolir_module_free(&module);
  return ok ? 0 : 4;
}
