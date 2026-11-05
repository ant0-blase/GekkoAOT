#include "backend/llvm/llvm_backend.h"
#include "ir/dolir_builder.h"
#include "cpu/cpu.h"
#include <cstdlib>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
  if (argc != 3) return 1;
  const std::string manifest = std::string(argv[1]) + ".relocations";
  FILE* file = std::fopen(manifest.c_str(), "wb");
  if (!file) return 2;
  std::fputs("80500104 p\n80500164 a\n80500184 p\n805001a4 p\n", file);
  std::fclose(file);
  setenv("DOLRECOMP_REL_RUNTIME_TRANSLATE", "1", 1);
  setenv("DOLRECOMP_REL_POINTER_MANIFEST", manifest.c_str(), 1);
  DolIRModule module;
  dolir_module_init(&module);
  const u32 words[][5] = {
    {0x3c60805a, 0x3b833c18, 0x4e800020}, // lis r3; addi r28,r3; blr
    {0x80bc0000, 0x4e800020, 0x60000000}, // lwz r5,0(r28); blr
    {0x3c60805a, 0x3b833c18, 0x4e800020}, // same scalar, no relocation proof
    {0x3c60805a, 0x80a33c18, 0x4e800020}, // relocated lwz displacement
    {0x3c60805a, 0x3b834000, 0x80bc0000, 0x4e800020}, // materialize then dereference
    {0x3c60805a, 0x3b834000, 0x3c608001, 0x93830000, 0x4e800020}, // publish live pointer
  };
  for (unsigned i = 0; i < 6; ++i) {
    u32 pc = 0x80500100 + i * 0x20;
    PPCInst decoded[5];
    for (unsigned n = 0; n < (i == 5 ? 5u : i == 4 ? 4u : 3u); ++n) decoded[n] = ppc_decode(words[i][n], pc + n * 4);
    if (!dolir_build_chunk(&module, decoded, i == 1 ? 2 : i == 5 ? 5 : i == 4 ? 4 : 3, pc)) return 3;
  }
  DolLLVMOptions options{};
  options.optimization_level = 2;
  options.verify = 1;
  options.emit_ir = 1;
  const std::string ir = std::string(argv[1]) + ".ll";
  options.ir_path = ir.c_str();
  options.target_profile = DOLLLVM_TARGET_HOST;
  options.fixed_memory_layout = 1;
  options.state_in_memory = std::atoi(argv[2]);
  options.ram_size = GC_MAIN_RAM_SIZE;
  options.mem2_size = WII_MEM2_SIZE;
  const bool ok = dolir_verify(&module, stderr) &&
                  dolllvm_emit_object(&module, argv[1], &options, stderr);
  dolir_module_free(&module);
  return ok ? 0 : 4;
}
