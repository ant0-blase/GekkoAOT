#include "backend/llvm/llvm_backend.h"
#include "ir/dolir_builder.h"
#include "cpu/cpu.h"
#include <cstdlib>
#include <cstdio>

// Synthetic nested SDK pattern: preserve a nonvolatile register and the
// interrupt-enable result while a clock HLE exits through both callers.
int main(int argc, char** argv) {
  if (argc != 3) return 1;
  const int mode = std::atoi(argv[2]);
  const u32 outer[] = {
    0x9421ffe0, 0x7c0802a6, 0x90010024, 0x93e1001c,
    0x480000f1, 0x7c7f1b78, 0x480001e9,
    0x7fe3fb78, 0x480002e1,
    0x83e1001c, 0x80010024, 0x7c0803a6, 0x38210020, 0x4e800020};
  const u32 disable[] = {0x7c6000a6, 0x5464045e, 0x7c800124, 0x54638ffe, 0x4e800020};
  const u32 inner[] = {0x9421fff0, 0x7c0802a6, 0x90010014,
                       0x480001f5, 0x7c741b78, 0x7c952378,
                       0x80010014, 0x7c0803a6, 0x38210010, 0x4e800020};
  const u32 restore[] = {0x2c030000, 0x7c8000a6, 0x4182000c,
                         0x60858000, 0x48000008, 0x5485045e,
                         0x7ca00124, 0x54838ffe, 0x4e800020};
  const u32 clock[] = {0x38601234, 0x4e800020};
  const u32* words[] = {outer, disable, inner, restore, clock};
  const u32 counts[] = {14, 5, 10, 9, 2};
  DolIRModule module;
  dolir_module_init(&module);
  DolLLVMFunctionRange ranges[5]{};
  for (unsigned i = 0; i < 5; ++i) {
    const u32 pc = 0x80018000 + i * 0x100;
    PPCInst decoded[14];
    for (unsigned j = 0; j < counts[i]; ++j)
      decoded[j] = ppc_decode(words[i][j], pc + j * 4);
    if (!dolir_build_chunk(&module, decoded, counts[i], pc)) return 2;
    ranges[i].start = module.functions[i].guest_start;
    ranges[i].end = module.functions[i].guest_end;
    if (!dolllvm_analyze_function_abi(&module.functions[i], &ranges[i])) return 3;
  }
  if (mode >= 4) {
    // Mixed chains occur when an unknown/custom control path fences just the
    // caller out of the Native ABI while its statically known leaves remain
    // native. Exercise both kinds of generated wrapper in one call stack.
    ranges[0].abi_flags = 0;
    ranges[0].abi_blockers |= DOLLLVM_ABI_BLOCK_CONTROL;
    ranges[2].abi_flags = 0;
    ranges[2].abi_blockers |= DOLLLVM_ABI_BLOCK_CONTROL;
  }
  dolllvm_enable_native_services(ranges, 5);
  DolLLVMOptions options{};
  options.optimization_level = 2;
  options.verify = 1;
  options.target_profile = DOLLLVM_TARGET_HOST;
  options.fixed_memory_layout = 1;
  options.ram_size = GC_MAIN_RAM_SIZE;
  options.state_in_memory = mode & 1;
  options.native_abi_policy = (mode & 3) < 2 ? DOLLLVM_NATIVE_ABI_DISABLED : DOLLLVM_NATIVE_ABI_UNRESTRICTED;
  options.function_ranges = ranges;
  options.function_range_count = 5;
  const bool ok = dolir_verify(&module, stderr) &&
      dolllvm_emit_object(&module, argv[1], &options, stderr);
  dolir_module_free(&module);
  return ok ? 0 : 4;
}
