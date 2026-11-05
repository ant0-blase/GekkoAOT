// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace GekkoAOTSdk {
template<class Read>
inline bool ProveTangentCameraUpdate(std::uint32_t pc,Read read) noexcept {
  const auto* code=read(pc,508);if(!code || code[0]!=0x94 || code[1]!=0x21 || code[2]!=0xff || code[3]!=0xc0)return false;
  std::uint64_t hash=0xcbf29ce484222325ull;
  for(unsigned i=0;i<508;++i)hash=(hash^code[i])*0x100000001b3ull;
  // Full virtual update body: consumes lens fields before rebuilding cached
  // projection, combined matrix and transformed planes, including dirty flags.
  return hash==16574993122755413141ull;
}
template<class Read>
inline bool ProveTangentCameraObject(const std::uint8_t* camera,Read read) noexcept {
  if(!camera)return false;
  const auto word=[](const auto* p){return(std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];};
  const auto* table=read(word(camera),0x28);
  return table && ProveTangentCameraUpdate(word(table+0x24),read);
}
// A call-free frustum consumer: near/far at camera+0x24/28 and horizontal/
// vertical half-angle tangents at +0x2c/30. Execute the original body after
// updating these shared inputs. Full bytes include every local branch target,
// field offset and math constant reference. No title/address lookup is used.
template<class Read>
inline bool ProveTangentFrustum(std::uint32_t pc,Read read) noexcept {
  const auto* code=read(pc,1904);if(!code)return false;
  std::uint64_t hash=0xcbf29ce484222325ull;
  for(unsigned i=0;i<1904;++i)hash=(hash^code[i])*0x100000001b3ull;
  return hash==395356782759847237ull;
}
}
