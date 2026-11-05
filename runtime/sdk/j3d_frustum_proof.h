// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "structural_signature.h"
#include "../gx/frustum_assist.h"

namespace GekkoAOTSdk {
inline bool J3dBodyProof(const std::uint8_t* code,unsigned size,std::uint64_t h1,std::uint64_t h2) {
  if(!code)return false;
  std::uint64_t a=0xcbf29ce484222325ull,b=0x9e3779b97f4a7c15ull;
  for(unsigned i=0;i<size;i+=4){
    const auto word=StructuralReadBE32(code+i);
    const auto op=word>>26,ra=(word>>16)&31u;
    const auto w=(op==48u || op==50u) && ra==2u ? word&0xffff0000u : StructuralInstructionSkeleton(word);
    a=(a^w)*0x100000001b3ull;
    b^=w+0x9e3779b9ull+(b<<6)+(b>>2);b*=0xbf58476d1ce4e5b9ull;
  }
  return a==h1 && b==h2;
}
// Full builder and all nine mathematical call sites, including complete
// cross-product/normalization/tangent bodies. Relocation alone is ignored;
// names, game IDs, hard-coded guest addresses and short prefixes admit nothing.
template<class Read> inline bool ProveJ3dFrustum(std::uint32_t pc,Read read) {
  const auto* code=read(pc,360);
  if(!J3dBodyProof(code,360,6428409756406521404ull,10145740655173921519ull))return false;
  std::uint32_t tangent=0,cross=0,normalize=0,target=0;
  if(!StructuralDirectCall(pc+48,StructuralReadBE32(code+48),&tangent) ||
     !StructuralDirectCall(pc+240,StructuralReadBE32(code+240),&cross) ||
     !StructuralDirectCall(pc+300,StructuralReadBE32(code+300),&normalize))return false;
  for(unsigned off:{256u,272u,288u})
    if(!StructuralDirectCall(pc+off,StructuralReadBE32(code+off),&target)||target!=cross)return false;
  for(unsigned off:{312u,324u,336u})
    if(!StructuralDirectCall(pc+off,StructuralReadBE32(code+off),&target)||target!=normalize)return false;
  return J3dBodyProof(read(cross,60),60,12371546636096855545ull,8407306849173366387ull) &&
    J3dBodyProof(read(normalize,68),68,3340355295555724257ull,5673665332761199244ull) &&
    J3dBodyProof(read(tangent,116),116,16243164244075453754ull,11231514819565222553ull);
}
template<class Read> inline bool ProveJ3dConstants(std::uint32_t pc,std::uint32_t sda,Read read) {
  const auto* code=read(pc,360);
  if(!code)return false;
  const auto constant=[&](unsigned offset,float expected){
    const auto* data=read(sda+std::int16_t(StructuralReadBE32(code+offset)),4);
    return data && GekkoAOT::GX::ReadBigEndianFloat(data)==expected;
  };
  return constant(20,.5f) && constant(36,.01745329238474369f);
}
}
