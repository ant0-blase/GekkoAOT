#pragma once
#include "card_sync_wrapper.h"
#include <array>

namespace GekkoAOTSdk {
enum class OsPrimitive { None, Queue, Mutex, MessageQueue, Priority, Context };
// Prove leaf initializers by their complete sequence of guest-visible stores.
// No calls, loads, branches, non-volatile clobbers or argument modifications are
// admitted. Register allocation for the temporary zero is immaterial.
inline OsPrimitive DecodeOsPrimitive(const std::uint8_t* code, std::size_t size) {
  if (!code || size < 8 || size > 64 || size % 4) return OsPrimitive::None;
  if (size == 8 && CardInstruction(code)==0x806302d0 && CardInstruction(code+4)==0x4e800020)
    return OsPrimitive::Priority;
  std::array<int, 32> values{}; // zero=unknown, 1=zero, 2=r4, 3=r5
  values[4]=2; values[5]=3;
  std::array<int, 8> stores{};
  for (std::size_t i=0;i<size/4;++i) {
    const auto w=CardInstruction(code+i*4);
    if (i==size/4-1) {if(w!=0x4e800020) return OsPrimitive::None; break;}
    const unsigned reg=(w>>21)&31;
    if ((w&0xfc1fffff)==0x38000000 && reg!=1 && reg!=2 && reg!=3 && reg<=12) {
      values[reg]=1; continue;
    }
    if ((w>>26)!=36 || ((w>>16)&31)!=3 || (w&0xffff)>28 || (w&3) || !values[reg])
      return OsPrimitive::None;
    const unsigned offset=(w&0xffff)/4;
    if(stores[offset]) return OsPrimitive::None;
    stores[offset]=values[reg];
  }
  if(stores==std::array<int,8>{1,1,0,0,0,0,0,0})return OsPrimitive::Queue;
  if(stores==std::array<int,8>{1,1,1,1,1,1,0,0})return OsPrimitive::Mutex;
  if(stores==std::array<int,8>{1,1,1,1,2,3,1,1})return OsPrimitive::MessageQueue;
  return OsPrimitive::None;
}
// OSInitCond and OSSignalCond are byte-identical after branch-target
// normalization. Preserve the actual direct-call target so the structural
// resolver can distinguish the two wrappers from proven callee identities.
inline bool DecodeOsThinCallWrapper(const std::uint8_t* code, std::size_t size,
                                    std::uint32_t pc, std::uint32_t* target) {
  if (!code || size != 32 || (pc & 3u)) return false;
  const std::uint32_t expected[] = {
      0x7c0802a6u, 0x90010004u, 0x9421fff8u,
      0u,
      0x8001000cu, 0x38210008u, 0x7c0803a6u, 0x4e800020u};
  for (unsigned i = 0; i < 8; ++i) {
    const auto word = CardInstruction(code + i * 4u);
    if (i == 3) {
      if ((word & 0xfc000003u) != 0x48000001u) return false;
      if (target) *target = RelativeBranchTarget(pc + 12u, word);
    } else if (word != expected[i]) {
      return false;
    }
  }
  return true;
}

inline bool DecodeOsInitContext(const std::uint8_t* code, std::size_t size,
                               std::uint32_t pc, std::uint32_t clear) {
  if (!code || size!=188) return false;
  std::array<std::uint32_t,47> expected{};
  unsigned n=0;
  for(auto word:{0x90830198u,0x90a30004u,0x39600000u,0x616b9032u,0x9163019cu,
                0x38000000u,0x90030080u,0x9003008cu,0x90430008u,0x91a30034u})
    expected[n++]=word;
  for(unsigned reg=3;reg<32;++reg) if(reg!=13) expected[n++]=0x90030000u|reg*4;
  for(unsigned i=0;i<8;++i) expected[n++]=0x900301a4u+i*4;
  expected[n++]=0x48000000u|((clear-(pc+184))&0x03fffffcu);
  if(n!=expected.size())return false;
  for(unsigned i=0;i<n;++i)if(CardInstruction(code+i*4)!=expected[i])return false;
  return true;
}
} // namespace GekkoAOTSdk
