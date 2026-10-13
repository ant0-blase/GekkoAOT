#include "backend/llvm/llvm_backend.h"
#include "ir/dolir_builder.h"
#include "cpu/cpu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main(int argc, char** argv) {
  if (argc != 4) return 1;
  const int mode = std::atoi(argv[2]);
  const u32 operations[] = {
    0xa0640000, 0xa8640000, 0x90640000, 0x98640000, 0xb0640000,
    0x90640000, 0xd8240000, 0xf0240000, 0xa0640000, 0xa0640000,
    0x90640000, 0xd0240000, 0xa0640000, 0xa0640000,
  };
  const u32 addresses[] = {
    0xcc000000, 0xcc00202e, 0xcc003004, 0xcc008000, 0xcc00801f,
    0x8c008000, 0xcc008000, 0xcc008000, 0xcc006000, 0xcc000001,
    0xcc008000, 0xcc008000, 0x0c000000, 0xcc002076,
  };
  DolIRModule module; dolir_module_init(&module);
  DolLLVMFunctionRange ranges[14]{};
  for (unsigned index = 0; index < 14; ++index) {
    const u32 pc = 0x80015000 + index * 0x100;
    u32 words[] = {0x3c800000u | (addresses[index] >> 16),
                   0x60840000u | (addresses[index] & 0xffffu), operations[index],
                   0x3a940001, 0x4e800020};
    if (index == 10) words[0] = words[1] = 0x60000000; // unknown EA
    PPCInst decoded[5];
    for (unsigned n = 0; n < 5; ++n) decoded[n] = ppc_decode(words[n], pc + n * 4);
    if (!dolir_build_chunk(&module, decoded, 5, pc)) return 2;
    ranges[index].start = module.functions[index].guest_start;
    ranges[index].end = module.functions[index].guest_end;
    if (!dolllvm_analyze_function_abi(&module.functions[index], &ranges[index])) return 3;
  }
  dolllvm_enable_native_services(ranges, 14);
  DolLLVMOptions options{};
  options.optimization_level = 2; options.verify = 1;
  options.target_profile = DOLLLVM_TARGET_HOST; options.fixed_memory_layout = 1;
  options.state_in_memory = mode & 1; options.ram_size = GC_MAIN_RAM_SIZE;
  options.native_abi_policy = mode < 2 ? DOLLLVM_NATIVE_ABI_DISABLED : DOLLLVM_NATIVE_ABI_UNRESTRICTED;
  options.function_ranges = ranges; options.function_range_count = 14;
  char fingerprint[1024];
  if (!dolllvm_codegen_fingerprint(&options, fingerprint, sizeof(fingerprint))) return 6;
  const bool lowered = std::strcmp(argv[3], "0") != 0;
  if ((std::strstr(fingerprint, "native-hardware-lowering=243") != nullptr) != lowered) return 7;
  if ((std::strstr(fingerprint, "native-wgpipe-try=244") != nullptr) !=
      (std::strcmp(argv[3], "2") == 0)) return 8;
  const bool ok = dolir_verify(&module, stderr) && dolllvm_emit_object(&module, argv[1], &options, stderr);
  dolir_module_free(&module); return ok ? 0 : 4;
}
