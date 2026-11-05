// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "frustum_assist.h"
#include <array>
namespace GekkoAOT::GX {
// Repair only the four proven camera-space side planes and their live portal
// bounds. Near/far, camera data, traversal and all culling tests remain guest
// owned. Called after the game rebuilds its camera and before object traversal.
inline bool AdjustReciprocalFrustum(float sourceX,float sourceY,float targetX,float targetY,
                                    std::uint8_t* planes,std::uint8_t* bounds) noexcept {
  for(float value:{sourceX,sourceY,targetX,targetY})
    if(!std::isfinite(value)||value<.01f||value>1000.f)return false;
  if(!planes||!bounds || (sourceX==targetX&&sourceY==targetY))return false;
  const auto side=[](float x,float y){
    const float a=1/std::sqrt(1+x*x),b=1/std::sqrt(1+y*y);
    return std::array<float,16>{-x*a,0,-a,0,x*a,0,-a,0,0,-y*b,-b,0,0,y*b,-b,0};
  };
  const auto original=side(sourceX,sourceY),replacement=side(targetX,targetY);
  const std::array<float,4> sourceBounds{-1/sourceX,-1/sourceY,1/sourceX,1/sourceY};
  const std::array<float,4> targetBounds{-1/targetX,-1/targetY,1/targetX,1/targetY};
  const auto close=[](float a,float b){return std::isfinite(a)&&std::abs(a-b)<=std::max(.00001f,std::abs(b)*.002f);};
  for(unsigned i=0;i<16;++i)if(!close(ReadBigEndianFloat(planes+4*i),original[i]))return false;
  for(unsigned i=0;i<4;++i)if(!close(ReadBigEndianFloat(bounds+4*i),sourceBounds[i]))return false;
  for(unsigned i=0;i<16;++i)WriteBigEndianFloat(planes+4*i,replacement[i]);
  for(unsigned i=0;i<4;++i)WriteBigEndianFloat(bounds+4*i,targetBounds[i]);
  return true;
}
}
