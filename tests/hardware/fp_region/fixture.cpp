#include "backend/llvm/llvm_backend.h"
#include "cpu/cpu.h"
#include "ir/dolir_builder.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static u32 paired(u32 xo, u32 d, u32 a, u32 b, u32 c = 0) {
  return (4u << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) |
         (xo << 1);
}

static bool add(DolIRModule &module, const std::vector<u32> &words, u32 pc) {
  std::vector<PPCInst> decoded;
  for (u32 word : words) {
    decoded.push_back(ppc_decode(word, pc));
    pc += 4;
  }
  return dolir_build_chunk(&module, decoded.data(), decoded.size(),
                          pc - words.size() * 4);
}

int main(int argc, char **argv) {
  if (argc != 5) return 1;
  DolIRModule module;
  dolir_module_init(&module);
  // Independent temporaries become exact pairs before the multiply/FMA chain.
  // No source register, address or symbol is special-cased by the optimization.
  std::vector<u32> chain = {
      paired(21, 1, 2, 3), paired(20, 4, 5, 6), paired(21, 7, 8, 9),
      paired(25, 10, 1, 0, 4), paired(29, 11, 10, 7, 4),
      paired(28, 12, 11, 7, 4), paired(30, 13, 12, 7, 4),
      paired(31, 14, 13, 7, 4), paired(21, 15, 14, 11),
      paired(20, 16, 15, 12), paired(25, 17, 16, 0, 4),
      paired(29, 18, 17, 7, 4), paired(28, 19, 18, 7, 4),
      paired(30, 20, 19, 7, 4), paired(31, 21, 20, 7, 4),
      paired(21, 22, 21, 11), paired(20, 23, 22, 12),
      paired(25, 24, 23, 0, 4), paired(29, 25, 24, 7, 4),
      paired(21, 26, 25, 11), paired(20, 27, 26, 12), 0x4e800020u};
  if (!add(module, chain, 0x80010000u) ||
      !add(module, {paired(21,1,2,3), 0x7c600124u, paired(21,4,1,3),
                    paired(25,5,4,0,3), 0x4e800020u}, 0x80010200u) ||
      !add(module, {paired(21,1,2,3), 0x80640000u, paired(21,4,1,3),
                    paired(25,5,4,0,3), 0x4e800020u}, 0x80010300u) ||
      !add(module, {paired(21,1,1,2), paired(25,3,1,0,2),
                    0x4200fff8u, 0x4e800020u}, 0x80010400u) ||
      !add(module, {paired(21,1,2,3), 0x48000008u, paired(25,4,1,0,3),
                    paired(21,5,1,3), paired(25,6,5,0,3), 0x4e800020u},
                    0x80010500u) ||
      !add(module, {paired(21,1,2,3), 0x4e800420u, paired(25,4,1,0,3),
                    paired(21,5,1,3), 0x4e800020u}, 0x80010600u))
    return 2;
  std::vector<u32> longRun(65, paired(21, 1, 1, 2));
  longRun.push_back(0x4e800020u);
  if (!add(module,longRun,0x80010700u)) return 2;
  DolLLVMOptions options{};
  options.optimization_level = 3;
  options.verify = 1;
  options.target_profile = DOLLLVM_TARGET_HOST;
  options.fixed_memory_layout = 1;
  options.state_in_memory = std::atoi(argv[3]);
  options.ram_size = GC_MAIN_RAM_SIZE;
  options.mem2_size = WII_MEM2_SIZE;
  options.symbol_suffix = argv[4];
  const u32 entries[] = {0x80010020u};
  options.entry_points = entries;
  options.entry_point_count = 1;
  options.emit_ir = 1;
  options.ir_path = argv[2];
  char fingerprint[1024];
  if (!dolllvm_codegen_fingerprint(&options, fingerprint, sizeof(fingerprint)))
    return 4;
  std::printf("FP_REGION_FINGERPRINT=%s\n",fingerprint);
  bool ok = dolir_verify(&module, stderr) &&
            dolllvm_emit_object(&module, argv[1], &options, stderr);
  options.emit_ir = 0;
  for (auto target : {DOLLLVM_TARGET_X86_64_V2,
                      DOLLLVM_TARGET_AARCH64_GENERIC}) {
    options.target_profile = target;
    const std::string output = std::string(argv[1]) + ".target" +
                               std::to_string(target);
    ok = ok && dolllvm_emit_object(&module, output.c_str(), &options, stderr);
  }
  dolir_module_free(&module);
  return ok ? 0 : 3;
}
