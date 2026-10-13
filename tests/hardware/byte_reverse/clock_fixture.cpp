#include "backend/llvm/llvm_backend.h"
#include "ir/dolir_builder.h"
#include "cpu/cpu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
  if (argc != 4) return 1;
  const int mode = std::atoi(argv[2]);
  DolIRModule module;
  dolir_module_init(&module);
  const u32 caller[] = {0x7fc802a6, 0x3a801234, 0xfe800890,
                        0x480000f5, 0x3a940001, 0x7fc803a6, 0x4e800020};
  // Guest fallback intentionally differs from the SDK token result.
  const u32 fallback[] = {0x38601234, 0x4e800020};
  for (unsigned index = 0; index < 3; ++index) {
    const u32* words = index ? fallback : caller;
    const unsigned count = index ? 2 : 7;
    const u32 pc = 0x80014000 + index * 0x100;
    PPCInst decoded[7];
    for (unsigned n = 0; n < count; ++n) decoded[n] = ppc_decode(words[n], pc + n * 4);
    if (!dolir_build_chunk(&module, decoded, count, pc)) return 2;
  }
  DolLLVMFunctionRange ranges[3]{};
  for (unsigned index = 0; index < 3; ++index) {
    ranges[index].start = module.functions[index].guest_start;
    ranges[index].end = module.functions[index].guest_end;
    if (!dolllvm_analyze_function_abi(&module.functions[index], &ranges[index])) return 3;
  }
  dolllvm_enable_native_services(ranges, 3);
  for (const auto& range : ranges)
    if (mode >= 2 && !(range.abi_flags & DOLLLVM_FUNCTION_ABI_NATIVE)) return 5;
  DolLLVMOptions options{};
  options.optimization_level = 2;
  options.verify = 1;
  options.target_profile = DOLLLVM_TARGET_HOST;
  options.fixed_memory_layout = 1;
  options.state_in_memory = mode & 1;
  options.ram_size = GC_MAIN_RAM_SIZE;
  options.native_abi_policy = mode < 2 ? DOLLLVM_NATIVE_ABI_DISABLED : DOLLLVM_NATIVE_ABI_UNRESTRICTED;
  options.function_ranges = ranges;
  options.function_range_count = 3;
  char fingerprint[1024];
  if (!dolllvm_codegen_fingerprint(&options, fingerprint, sizeof(fingerprint))) return 6;
  const bool lowered = std::strcmp(argv[3], "1") == 0;
  if ((std::strstr(fingerprint, "sdk-clock-lowering=238") != nullptr) != lowered) return 7;
  const bool ok = dolir_verify(&module, stderr) && dolllvm_emit_object(&module, argv[1], &options, stderr);
  dolir_module_free(&module);
  return ok ? 0 : 4;
}
