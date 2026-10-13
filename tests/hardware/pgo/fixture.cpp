#include "backend/llvm/llvm_backend.h"
#include "ir/dolir_builder.h"
#include "cpu/cpu.h"
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char **argv) {
  if (argc != 5) return 1;
  // Real guest loads/stores and a loop/backedge exercise the pre-PGO CFG.
  // Addresses identify this synthetic fixture only.
  const u32 words[] = {0x80640000, 0x38630001, 0x90640000, 0x4200fff4, 0x4e800020};
  std::vector<PPCInst> decoded;
  for (unsigned i=0; i<5; ++i) decoded.push_back(ppc_decode(words[i],0x80010000+4*i));
  DolIRModule m; dolir_module_init(&m);
  if (!dolir_build_chunk(&m,decoded.data(),decoded.size(),0x80010000)) return 2;
  DolLLVMOptions o{}; o.optimization_level=3; o.verify=1;
  o.target_profile=DOLLLVM_TARGET_HOST; o.state_in_memory=1;
  o.fixed_memory_layout=1; o.ram_size=GC_MAIN_RAM_SIZE;
  o.emit_ir=1; o.ir_path=argv[4];
  if (!std::strcmp(argv[2],"gen")) o.profile_generate_path=argv[3];
  else if (!std::strcmp(argv[2],"use")) o.profile_use_path=argv[3];
  else if (std::strcmp(argv[2],"off")) return 3;
  const bool ok=dolir_verify(&m,stderr) && dolllvm_emit_object(&m,argv[1],&o,stderr);
  dolir_module_free(&m);return ok?0:4;
}
