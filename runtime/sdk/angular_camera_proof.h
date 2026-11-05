// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "structural_signature.h"
#include "../gx/frustum_assist.h"
#include <span>
namespace GekkoAOTSdk {
inline bool AngularBodyHash(const std::uint8_t* code,unsigned size,
                           std::uint64_t h1,std::uint64_t h2) {
  if(!code)return false;
  std::uint64_t a=0xcbf29ce484222325ull,b=0x9e3779b97f4a7c15ull;
  for(unsigned i=0;i<size;i+=4){
    const auto w=StructuralInstructionSkeleton(StructuralReadBE32(code+i));
    a=(a^w)*0x100000001b3ull;
    b^=w+0x9e3779b9ull+(b<<6)+(b>>2);b*=0xbf58476d1ce4e5b9ull;
  }
  return a==h1 && b==h2;
}
// First proven engine provider: a pure setter of four side-plane angles and a
// cached pixel focal length. Match all register/data flow, mathematical callees
// and relocated constants. Neither title IDs nor guest addresses are rules.
template<class Read>
inline bool ProveAngularCameraSetter(std::uint32_t pc,Read read,float* focal=nullptr) {
  const auto* code=read(pc,160);
  if(!AngularBodyHash(code,160,5773351521268936550ull,14470121306214665709ull))return false;
  const unsigned loads[]{24,32,40,80,128},lis[]{16,20,28,72,124};
  const float expected[]{.5f,3.1415927410125732f,180.f,1.f,240.f};
  for(unsigned i=0;i<5;++i){
    const auto upper=StructuralReadBE32(code+lis[i]);
    const auto lower=StructuralReadBE32(code+loads[i]);
    const auto address=(upper<<16)+std::int16_t(lower&0xffff);
    const auto* data=read(address,4);
    if(!data || GekkoAOT::GX::ReadBigEndianFloat(data)!=expected[i])return false;
  }
  std::uint32_t tangent=0,angle=0,again=0;
  if(!StructuralDirectCall(pc+68,StructuralReadBE32(code+68),&tangent) ||
     !StructuralDirectCall(pc+96,StructuralReadBE32(code+96),&angle) ||
     !StructuralDirectCall(pc+120,StructuralReadBE32(code+120),&again) || tangent!=again || tangent==angle)return false;
  if(!AngularBodyHash(read(tangent,128),128,6032160163685330249ull,10940234931918130976ull) ||
     !AngularBodyHash(read(angle,68),68,9561163349507237278ull,16218446512488965885ull))return false;
  if(focal)*focal=expected[4];
  return true;
}
}
