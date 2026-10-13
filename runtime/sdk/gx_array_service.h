// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace GekkoAOTSdk {
// Full retail SDK leaf. Only the two identical SDA relocations may vary,
// using either SDK small-data base r2 or r13. Neither is read by admitted
// vertex-array calls; matrix/light shadow updates remain in the AOT fallback.
// Debug assertions, changed control flow and different side effects decline.
inline constexpr std::array<std::uint32_t,35> kGxSetArrayBody{
  0x2c030019,0x40820008,0x3860000a,0x38000008,0x38c3fff7,0x3c60cc01,
  0x98038000,0x60c000a0,0x548400be,0x98038000,0x3406fff4,0x90838000,
  0x4180001c,0x2c000004,0x40800014,0x806d0000,0x5400103a,0x7c630214,
  0x90830088,0x38000008,0x3c60cc01,0x98038000,0x60c000b0,0x54a4063e,
  0x98038000,0x3406fff4,0x90838000,0x4d800020,0x2c000004,0x4c800020,
  0x806d0000,0x5400103a,0x7c630214,0x90830098,0x4e800020};
inline constexpr std::array<std::uint32_t,17> kGxSetArrayLeafBody{
  0x2c030019,0x40820008,0x3860000a,0x38e00008,0x3863fff7,0x3cc0cc01,
  0x98e68000,0x606000a0,0x548400be,0x98068000,0x606300b0,0x54a0063e,
  0x90868000,0x98e68000,0x98668000,0x90068000,0x4e800020};
inline bool ProveGxSetArray(const std::uint8_t* code,std::size_t size,bool* leaf=nullptr) noexcept {
  if(leaf)*leaf=false;
  if(!code || size<68u) return false;
  bool exact_leaf=true;
  for(unsigned i=0;i<17;++i) {
    const auto* b=code+4*i;
    const auto w=(std::uint32_t(b[0])<<24)|(std::uint32_t(b[1])<<16)|
                 (std::uint32_t(b[2])<<8)|b[3];
    if(w!=kGxSetArrayLeafBody[i]){exact_leaf=false;break;}
  }
  if(exact_leaf){if(leaf)*leaf=true;return true;}
  if(size<140u)return false;
  std::uint32_t relocated_load=0;
  for(unsigned i=0;i<35;++i) {
    const auto* b=code+4*i;
    const auto w=(std::uint32_t(b[0])<<24)|(std::uint32_t(b[1])<<16)|
                 (std::uint32_t(b[2])<<8)|b[3];
    if(i==15) {relocated_load=w;if((w&0xffff0000u)!=0x806d0000u && (w&0xffff0000u)!=0x80620000u)return false;}
    else if(i==30) {if(w!=relocated_load)return false;}
    else if(w!=kGxSetArrayBody[i]) return false;
  }
  return true;
}
// Vertex arrays perform two ordered CP bindings without SDA access. The
// 140-byte variant also mutates SDK shadow state for matrix/light IDs (21..24),
// which retain their original AOT path. The 68-byte leaf admits those IDs.
// NBT aliases the normal array exactly in both variants.
inline bool GxVertexArrayPacket(std::uint32_t attribute,std::uint32_t base,
                                std::uint32_t stride,std::array<std::uint8_t,12>& packet,
                                bool leaf=false) noexcept {
  if(attribute==25u) attribute=10u;
  if(attribute<9u || attribute>(leaf?24u:20u)) return false;
  const auto index=attribute-9u;
  const std::uint32_t address=base&0x3fffffffu;
  packet={8u,static_cast<std::uint8_t>(index|0xa0u),
          std::uint8_t(address>>24),std::uint8_t(address>>16),std::uint8_t(address>>8),std::uint8_t(address),
          8u,static_cast<std::uint8_t>(index|0xb0u),0,0,0,std::uint8_t(stride)};
  return true;
}
} // namespace GekkoAOTSdk
