#include "backend/llvm/llvm_backend.h"
#include "ir/dolir_builder.h"
#include "cpu/cpu.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
static bool add(DolIRModule *m, std::vector<u32> words, u32 pc) {
  std::vector<PPCInst> decoded;
  for (u32 word : words) { decoded.push_back(ppc_decode(word, pc)); pc += 4; }
  return dolir_build_chunk(m, decoded.data(), decoded.size(), pc - words.size()*4);
}
static u32 fma(bool single, u32 xo, u32 d, u32 a, u32 c, u32 b) {
  return ((single ? 59u : 63u)<<26) | (d<<21) | (a<<16) | (b<<11) | (c<<6) | (xo<<1);
}
int main(int argc, char **argv) {
  if (argc != 3) return 1;
  DolIRModule m; dolir_module_init(&m);
  for (u32 single=0; single<2; ++single)
    for (u32 variant=0; variant<4; ++variant)
      if (!add(&m, {fma(single, 29u+(variant==1?-1:variant==2?2:variant==3?1:0), 1, 2, 3, 4),
                    0x4e800020}, 0x80010000 + single*0x100 + variant*0x20)) return 2;
  // Aliasing and successive FPRF updates, including a suppressed exception.
  if (!add(&m, {fma(true,29,1,2,3,4), fma(true,30,1,1,5,6), 0x4e800020}, 0x80010200)) return 2;
  // A guest backedge clears compile-time provenance; ctr is set by the test.
  if (!add(&m, {fma(true,29,1,1,3,4), 0x4200fffc, 0x4e800020}, 0x80010220)) return 2;
  // lfs-proven operands, then scalar FMA and mffs to observe deferred FPRF.
  if (!add(&m, {0xc0440000,0xc0640004,0xc0840008,fma(true,29,1,2,3,4),
               0xfce0048e,0x4e800020}, 0x80010240)) return 2;
  if (!add(&m, {fma(true,29,1,2,3,4)|1u,0x4e800020}, 0x80010260)) return 2;
  DolLLVMOptions o{}; o.optimization_level=2; o.verify=1;
  o.target_profile=DOLLLVM_TARGET_HOST; o.fixed_memory_layout=1;
  o.state_in_memory=std::atoi(argv[2]);
  o.ram_size=GC_MAIN_RAM_SIZE; o.mem2_size=WII_MEM2_SIZE;
  bool ok=dolir_verify(&m,stderr) && dolllvm_emit_object(&m,argv[1],&o,stderr);
  for (auto target : {DOLLLVM_TARGET_X86_64_V2, DOLLLVM_TARGET_AARCH64_GENERIC}) {
    o.target_profile=target;
    std::string path=std::string(argv[1])+".target"+std::to_string(target);
    ok=ok && dolllvm_emit_object(&m,path.c_str(),&o,stderr);
  }
  dolir_module_free(&m); return ok?0:3;
}
