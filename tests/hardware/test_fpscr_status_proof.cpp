// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../tools/fpscr_status_proof.h"
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::cerr << "FAIL " #x " line " << __LINE__ << '\n'; std::exit(1); } } while (0)

namespace {
using GekkoAOT::Build::FpscrStatusProof;
FpscrStatusProof Prove(std::initializer_list<std::uint32_t> words) {
  FpscrStatusProof proof;
  const std::vector<std::uint32_t> code(words);
  GekkoAOT::Build::ScanFpscrStatusReaders(code, proof);
  return proof;
}
FpscrStatusProof ProveAt(std::vector<std::uint32_t> words, std::vector<std::uint32_t> data = {},
                         std::vector<std::uint32_t> rel = {}) {
  std::vector<GekkoAOT::Build::GuestCodeSection> code{{0x80000000u, std::move(words)}};
  return GekkoAOT::Build::ProveFpscrStatus(code, data, rel);
}
// blr; PPCMffs @0x80000004; PPCMtfsf @0x8000001c; caller @0x80000028.
std::vector<std::uint32_t> GetterProgram(std::uint32_t between) {
  return {0x4e800020u,
          0x9421ffe8u, 0xdbe10010u, 0xffe0048eu, 0xdbe10008u, 0x8061000cu, 0x4e800020u,
          0x9421ffe0u, 0xfdfe058eu, 0x4e800020u,
          0x4bffffddu, 0x5463003au, between, 0x4bffffe9u};
}
}  // namespace

int main() {
  // Star Fox Adventures: __OSSaveFPUContext / __OSLoadFPUContext / NI init.
  const auto sfa = Prove({0xdbe50188u, 0xfc00048eu, 0xd8050190u, 0xc8050090u,
                          0xc8040190u, 0xfdfe058eu, 0xffa0004cu, 0xec2200b2u});
  CHECK(sfa.unobservable && sfa.mffs_saves == 1);
  // Kirby/Melee save variant: mffs f0; stfd f14..; stfd f0 into the context.
  CHECK(Prove({0xfc00048eu, 0xd9c30010u, 0xd9e30018u, 0xd8030190u}).unobservable);
  CHECK(!Prove({0xfc000080u}).unobservable);                       // mcrfs cr0,cr0
  CHECK(!Prove({0xfc22182bu}).unobservable);                       // fadd. f1,f2,f3
  CHECK(!Prove({0xec22182bu}).unobservable);                       // fadds. f1,f2,f3
  CHECK(!Prove({0x1022182bu}).unobservable);                       // ps_add. f1,f2,f3
  CHECK(Prove({0x1022182au}).unobservable);                        // ps_add f1,f2,f3
  CHECK(!Prove({0xfc00048eu, 0xfc20002au}).unobservable);         // mffs f0; fadd f1,f0,f0
  CHECK(!Prove({0xfc00048eu, 0x48000010u}).unobservable);         // mffs f0; b ...
  // Writes never expose status: constructed mtfsf, enable-setting mtfsb1/mtfsfi.
  CHECK(Prove({0xc8240190u, 0xfdfe058eu, 0xff20004cu, 0xff00610cu}).unobservable);
  // PPCMffs getter whose caller only rewrites rounding bits through PPCMtfsf.
  const auto rmw = ProveAt(GetterProgram(0x60630001u));             // ori r3,r3,1
  CHECK(rmw.unobservable && rmw.mffs_getters == 1 && rmw.getter_callers == 1);
  CHECK(!ProveAt(GetterProgram(0x2c030000u)).unobservable);         // cmpwi r3,0
  CHECK(!ProveAt(GetterProgram(0x7c641b78u)).unobservable);         // mr r4,r3
  CHECK(!ProveAt(GetterProgram(0x60630001u), {0x80000004u}).unobservable);      // pointer in data
  CHECK(!ProveAt(GetterProgram(0x60630001u), {}, {0x80000004u}).unobservable);  // REL call

  GekkoAOT::Build::ReservationProof none, used;
  const std::vector<std::uint32_t> plain{0x90010008u, 0x7c00012du};  // stw; stwcx. alone
  GekkoAOT::Build::ScanReservationUsers(plain, none);
  CHECK(none.absent && none.stwcx == 1 && none.lwarx == 0);
  const std::vector<std::uint32_t> atomic{0x7c601828u, 0x7c80192du};  // lwarx r3,0,r3; stwcx. r4,0,r3
  GekkoAOT::Build::ScanReservationUsers(atomic, used);
  CHECK(!used.absent && used.lwarx == 1 && used.stwcx == 1);
  std::cout << "fpscr status proof: ok\n";
  return 0;
}
