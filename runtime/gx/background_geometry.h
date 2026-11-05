// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <span>
namespace GekkoAOT::GX {
// Large background faces can be anchored in level space rather than exactly
// at the eye. Use this only for an opaque, depth-ALWAYS background pass.
// Angular extent excludes ordinary distant buildings and individual objects.
inline bool DistantBackgroundFace(std::span<const std::array<float,3>> vertices,
                                  const std::array<float,3>& translation,float nearPlane) noexcept {
  if(vertices.size()<3 || !(nearPlane>0) || !std::isfinite(nearPlane))return false;
  float low=INFINITY,high=0,translationLength=0;
  for(float t:translation){if(!std::isfinite(t))return false;translationLength+=t*t;}
  float minimumCosine=1;
  for(unsigned i=0;i<vertices.size();++i){
    const auto& p=vertices[i];float squared=0;for(float v:p)squared+=v*v;
    const float radius=std::sqrt(squared);if(!std::isfinite(radius)||radius<=0)return false;
    low=std::min(low,radius);high=std::max(high,radius);
    for(unsigned j=0;j<i;++j){const auto& q=vertices[j];
      const float qr=std::sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]);
      minimumCosine=std::min(minimumCosine,(p[0]*q[0]+p[1]*q[1]+p[2]*q[2])/(radius*qr));
    }
  }
  return low>nearPlane*1024.f && high<low*1.6f &&
         translationLength<low*low*.0625f && minimumCosine<.75f;
}
// A distant shell centred on the eye is background, not world geometry.
// Its translation-free transform and common radial distance distinguish it
// from a building, soldier or vehicle. No texture colours or GameID involved.
inline bool CameraCentredShell(std::span<const std::array<float,3>> vertices,
                               const std::array<float,3>& translation,float matrixScale,float nearPlane) noexcept {
  if(vertices.size()<3 || !(matrixScale>0) || !(nearPlane>0))return false;
  for(float t:translation)if(!std::isfinite(t)||std::abs(t)>matrixScale*1e-4f)return false;
  float low=INFINITY,high=0;
  for(const auto& p:vertices){const float radius=std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
    if(!std::isfinite(radius))return false;low=std::min(low,radius);high=std::max(high,radius);}
  return low>100.f*nearPlane && high<low*1.025f;
}
}

namespace GekkoAOT::GX {
// Some engines draw a small dome around the eye before translated scenery.
// Distance alone cannot distinguish that dome: it can be closer than a car.
// Keep the enclosing prelude's role across camera turns, and end it as soon
// as translated scene geometry begins. A front-facing weapon alone does not
// establish enclosing-background evidence.
class EyeCentredPrelude {
  std::uint64_t frame_=~std::uint64_t(0);
  bool open_=true,seenEnclosing_=false;
public:
  void Begin(std::uint64_t frame){if(frame!=frame_){frame_=frame;open_=true;}}
  bool Open() const{return open_;}
  void Scene(){open_=false;}
  bool Observe(std::span<const std::array<float,3>> points,const std::array<float,3>& translation,float scale,float near){
    if(!open_ || points.size()<3 || !(scale>0) || !(near>0))return false;
    for(float t:translation)if(!std::isfinite(t)||std::abs(t)>scale*1e-4f){open_=false;return false;}
    float radius=0;bool behind=false;
    for(const auto& p:points){const float r=std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);if(!std::isfinite(r)){open_=false;return false;}radius=std::max(radius,r);behind|=p[2]>near;}
    if(radius>near*512){open_=false;return false;}
    if(radius<near*8)return false;
    if(behind)seenEnclosing_=true;
    return seenEnclosing_;
  }
};
}
