// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "frustum_assist.h"
#include <array>

namespace GekkoAOT::GX {
// Perspective camera parameters consumed by both CPU visibility and GX. These
// are signed side-plane angles, followed by positive near/far distances.
inline std::array<float,6> AngularProjection(const std::uint8_t* record) noexcept {
  std::array<float,6> p{};
  if (!record) return p;
  const float l=std::tan(ReadBigEndianFloat(record)), r=std::tan(ReadBigEndianFloat(record+4));
  const float t=std::tan(ReadBigEndianFloat(record+8)), b=std::tan(ReadBigEndianFloat(record+12));
  const float n=ReadBigEndianFloat(record+16), f=ReadBigEndianFloat(record+20);
  if (!(l<r && b<t && n>0 && f>n)) return p;
  return {2/(r-l),(r+l)/(r-l),2/(t-b),(t+b)/(t-b),-n/(f-n),-f*n/(f-n)};
}
inline bool SameCameraProjection(const std::array<float,6>& a,
                                 const std::array<float,6>& b) noexcept {
  for (unsigned i=0;i<6;++i)
    if(!std::isfinite(a[i]) || !std::isfinite(b[i]) ||
       std::abs(a[i]-b[i])>std::max(1e-6f,std::abs(b[i])*0.002f)) return false;
  return a[0]>0 && a[2]>0;
}
// Update the actual camera setter's outputs, including its cached focal length.
// The record's physical viewport dimensions and all transforms stay intact.
inline bool SetAngularCamera(std::uint8_t* record, float degrees, float aspect,
                              float focal_pixels) noexcept {
  if(!record || !LooksLikeCameraFov(degrees) || !std::isfinite(aspect) ||
     aspect<.5f || aspect>5 || !std::isfinite(focal_pixels) || focal_pixels<=0) return false;
  const float half=degrees*0.008726646259971648f;
  const float tangent=std::tan(half), horizontal=std::atan(aspect*tangent);
  const float values[]{-horizontal,horizontal,half,-half};
  for(unsigned i=0;i<4;++i)WriteBigEndianFloat(record+4*i,values[i]);
  WriteBigEndianFloat(record+0xec,focal_pixels/tangent);
  return true;
}
}
