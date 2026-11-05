// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "angular_camera.h"
#include "projection_fov.h"

namespace GekkoAOT::GX {
// J3DUClipper stores four normalized side planes and retains its native lens.
// Adjust the planes, not the shared FOV/aspect fields: those can also feed GX
// projections, and changing them would apply the renderer's hor+ twice.
inline bool AdjustJ3dFrustum(std::uint8_t* camera,
                            const std::array<float,6>& observed,
                            float guest_aspect,float target_aspect,float target_fov) noexcept {
  if(!camera || !std::isfinite(guest_aspect) || guest_aspect<=0 ||
     !std::isfinite(target_aspect) || target_aspect<.5f || target_aspect>5.f ||
     (target_fov!=0 && !LooksLikeCameraFov(target_fov)))return false;
  const float fov=ReadBigEndianFloat(camera+0x4c),aspect=ReadBigEndianFloat(camera+0x50);
  const float near=ReadBigEndianFloat(camera+0x54),far=ReadBigEndianFloat(camera+0x58);
  if(!LooksLikeCameraFov(fov)||!std::isfinite(aspect)||aspect<=0 ||
     !std::isfinite(near)||!std::isfinite(far)||near<=0||far<=near)return false;
  const float vertical=1.f/std::tan(fov*.008726646259971648f);
  const std::array<float,6> lens{vertical/aspect,0,vertical,0,-near/(far-near),-far*near/(far-near)};
  // J3D may cull with a more conservative near plane than the render camera
  // (100 versus 10 in MKDD). Side-plane correlation uses the angular lens
  // and far clip, while the game's own near/far tests remain untouched.
  auto angular=lens;angular[4]=observed[4];angular[5]=observed[5];
  const float render_far=observed[4]!=0.f ? observed[5]/observed[4] : 0.f;
  if(!SameCameraProjection(angular,observed) || !std::isfinite(render_far) ||
     std::abs(far-render_far)>std::max(1e-3f,far*.005f))return false;
  const float scale=guest_aspect/target_aspect;
  const float focal=PerspectiveFovScale(vertical,target_fov);
  const float horizontal=1.f/(lens[0]*scale*focal),height=1.f/(vertical*focal);
  if(!std::isfinite(horizontal)||!std::isfinite(height)||horizontal<=0||height<=0)return false;
  const float nx=1.f/std::hypot(1.f,horizontal),ny=1.f/std::hypot(1.f,height);
  // Cross(topLeft,bottomLeft), Cross(topRight,topLeft), and their opposites.
  const std::array<float,12> planes{-nx,0,horizontal*nx, 0,ny,height*ny,
                                  nx,0,horizontal*nx, 0,-ny,height*ny};
  for(unsigned i=0;i<planes.size();++i)WriteBigEndianFloat(camera+4+4*i,planes[i]);
  return true;
}
}
