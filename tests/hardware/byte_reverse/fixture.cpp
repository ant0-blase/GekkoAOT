#include "backend/llvm/llvm_backend.h"
#include "ir/dolir_builder.h"
#include "cpu/cpu.h"
#include <cstdlib>
#include <cstdio>

int main(int argc, char** argv) {
  if (argc != 3) return 1;
  DolIRModule module;
  dolir_module_init(&module);
  const u32 words[] = {
      (31u << 26) | (3u << 21) | (4u << 11) | (790u << 1), // lhbrx r3,0,r4
      (31u << 26) | (3u << 21) | (4u << 11) | (918u << 1), // sthbrx r3,0,r4
      (31u << 26) | (4u << 21) | (4u << 11) | (790u << 1), // lhbrx r4,0,r4
      (31u << 26) | (3u << 21) | (4u << 11) | (534u << 1), // lwbrx r3,0,r4
      (31u << 26) | (3u << 21) | (4u << 11) | (662u << 1), // stwbrx r3,0,r4
  };
  for (unsigned i = 0; i < sizeof(words) / sizeof(*words); ++i) {
    const u32 pc = 0x80012000u + i * 0x20u;
    PPCInst decoded[] = {ppc_decode(words[i], pc), ppc_decode(0x4e800020u, pc + 4u)};
    if (!dolir_build_chunk(&module, decoded, 2u, pc)) return 2;
  }
  DolLLVMOptions options{};
  options.optimization_level = 2;
  options.verify = 1;
  options.target_profile = DOLLLVM_TARGET_HOST;
  options.fixed_memory_layout = 1;
  options.state_in_memory = std::atoi(argv[2]);
  options.ram_size = GC_MAIN_RAM_SIZE;
  options.mem2_size = WII_MEM2_SIZE;
  const bool ok = dolir_verify(&module, stderr) &&
                  dolllvm_emit_object(&module, argv[1], &options, stderr);
  dolir_module_free(&module);
  return ok ? 0 : 3;
}
