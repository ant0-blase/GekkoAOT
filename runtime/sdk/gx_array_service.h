// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "word_proof.h"
namespace GekkoAOTSdk {
// Full retail SDK leaf. Only the two identical SDA relocations may vary,
// using either SDK small-data base r2 or r13. Neither is read by admitted
// vertex-array calls; matrix/light shadow updates remain in the AOT fallback.
// Debug assertions, changed control flow and different side effects decline.
// Fingerprints of the two retail SDK variants (see word_proof.h). In the
// 35-instruction variant, words 15 and 30 are the same SDA-relative load,
// checked below instead of being part of the fingerprint.
inline constexpr WordProof kGxSetArrayLeafProof{"FFFFFFFFFFFFFFFFF", 0x7ab7ef3899194766ull};
inline constexpr WordProof kGxSetArrayProof{"FFFFFFFFFFFFFFF-FFFFFFFFFFFFFF-FFFF", 0xb397f5a192755498ull};
inline bool ProveGxSetArray(const std::uint8_t* code,std::size_t size,bool* leaf=nullptr) noexcept {
  if(leaf)*leaf=false;
  if(!code || size<68u) return false;
  if(MatchesWordProof(code,size,kGxSetArrayLeafProof)){if(leaf)*leaf=true;return true;}
  if(size<140u)return false;
  const auto relocated_load=WordProofRead(code+4*15);
  if((relocated_load&0xffff0000u)!=0x806d0000u && (relocated_load&0xffff0000u)!=0x80620000u)return false;
  if(WordProofRead(code+4*30)!=relocated_load)return false;
  return MatchesWordProof(code,size,kGxSetArrayProof);
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
