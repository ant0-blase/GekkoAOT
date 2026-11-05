// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "device_proofs.h"
#include "word_proof.h"
#include <bit>
namespace GekkoAOTSdk {
enum class GxPixelFamily : unsigned { AlphaCompare,ZMode,ColorUpdate,AlphaUpdate };
// Fingerprints of the four call-free SDK leaves (see word_proof.h). 'H' marks
// the loads of the relocated SDA cell: only their displacement may differ.
inline const WordProof& GxPixelProof(GxPixelFamily family) {
  static constexpr WordProof proofs[]{
    {"FHFFFFFFFFFFFFFFFFFF", 0x2aa8ca025f6fa4daull},
    {"HFFFFFFFFFHFFFFFFFHFFFFFFHFFFF", 0x04e6d7a9e8a80166ull},
    {"HFFFFFFFFFFHFFFF", 0x66face7ed551d6a8ull},
    {"HFFFFFFFFFFHFFFF", 0x285b2e2aa0425f32ull},
  };
  return proofs[static_cast<unsigned>(family)];
}
// Entire call-free SDK leaf, including FIFO stores and GXData bookkeeping.
// Only the relocated SDA cell displacement may differ; every reference must
// use the same cell. No runtime title/address rule or approximate signature.
inline std::optional<std::int16_t> ProveGxPixelLeaf(std::span<const std::uint8_t> code,GxPixelFamily family) {
  const auto& proof=GxPixelProof(family);if(code.size()!=proof.masks.size()*4)return std::nullopt;
  std::optional<std::int16_t> sda;
  for(unsigned i=0;i<proof.masks.size();++i){
    if(proof.masks[i]!='H')continue;
    const auto actual=DeviceWord(code.data()+4*i);
    if(sda && *sda!=std::int16_t(actual))return std::nullopt;
    sda=std::int16_t(actual);
  }
  if(!MatchesWordProof(code.data(),code.size(),proof))return std::nullopt;
  return sda;
}
// The semantic result fits in two scalar host registers. Keep the arithmetic
// separate from commit so IRQ/transport failure cannot partially publish GPRs.
struct GxPixelResult {std::uint32_t word,scratch;};
inline GxPixelResult ComputeGxPixelLeaf(GxPixelFamily family,const std::uint32_t* gpr,
                                      std::uint32_t cached) {
  if(family==GxPixelFamily::AlphaCompare){
    const auto blend=((gpr[7]&255)<<8)|(gpr[4]&255);
    auto value=(gpr[3]<<16)|blend;
    value=(value&0xffc7ffffu)|(gpr[6]<<19);
    value=(value&0xff3fffffu)|(gpr[5]<<22);
    return {0xf3000000u|(value&0xffffffu),blend};
  }
  if(family==GxPixelFamily::ZMode){
    const auto first=(cached&~1u)|(gpr[3]&255u);
    const auto second=(first&~14u)|(gpr[4]<<1);
    const auto scratch=second&~16u;
    return {scratch|((gpr[5]<<4)&0xff0u),scratch};
  }
  const bool alpha=family==GxPixelFamily::AlphaUpdate;
  const auto field=std::rotl(gpr[3],alpha?4:3)&(alpha?0xff0u:0x7f8u);
  return {(cached&~(alpha?16u:8u))|field,0};
}
inline void CommitGxPixelRegisters(GxPixelFamily family,std::uint32_t* gpr,
                                   std::uint32_t context,GxPixelResult result) {
  gpr[0]=0;
  if(family==GxPixelFamily::AlphaCompare){
    gpr[3]=0xcc010000u;gpr[4]=result.word;gpr[7]=result.scratch;gpr[8]=context;
  }else if(family==GxPixelFamily::ZMode){
    gpr[3]=result.word;gpr[4]=context;gpr[5]=0xcc010000u;
    gpr[6]=result.scratch;gpr[7]=context+0x1d8;gpr[8]=gpr[7];
  }else{
    gpr[3]=result.word;gpr[4]=context;gpr[5]=0xcc010000u;gpr[6]=context+0x1d0;
  }
}
// Full-register adapter used by the independent PPC differential oracle.
inline std::uint32_t ExecuteGxPixelLeaf(GxPixelFamily family,std::uint32_t* gpr,
                                      std::uint32_t context,std::uint32_t cached) {
  const auto result=ComputeGxPixelLeaf(family,gpr,cached);
  CommitGxPixelRegisters(family,gpr,context,result);return result.word;
}
} // namespace GekkoAOTSdk
