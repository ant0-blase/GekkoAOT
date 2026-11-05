// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "angular_camera.h"

namespace GekkoAOT::GX {
// Camera inputs shared by the proven guest frustum builder and its projection
// builder. Preserve native lenses so live FOV/aspect changes can be restored.
class TangentCameraAdjuster {
  struct Entry {
    std::uint32_t address=0;
    std::array<float,2> source{},applied{};
    float source_aspect=0;
    std::uint64_t age=0;
  };
  std::array<Entry,32> entries_{};
  std::uint64_t age_=0;
public:
  void Reset() noexcept {entries_={};age_=0;}
  bool Apply(std::uint32_t address,std::uint8_t* camera,
             const std::array<float,6>& observed,float observed_aspect,
             float target_fov,float target_aspect) noexcept {
    if(!camera || !address || !std::isfinite(observed_aspect) || observed_aspect<=0 ||
       !std::isfinite(target_aspect) || (target_aspect!=0 && (target_aspect<.5f || target_aspect>5)) ||
       (target_fov!=0 && !LooksLikeCameraFov(target_fov)))return false;
    const std::array<float,2> current{ReadBigEndianFloat(camera+0x2c),ReadBigEndianFloat(camera+0x30)};
    const float near=ReadBigEndianFloat(camera+0x24);
    for(float t:current)if(!std::isfinite(t)||t<=.01f||t>1000)return false;
    auto found=std::find_if(entries_.begin(),entries_.end(),[&](const auto& e){return e.address==address;});
    const auto matches=[&](const auto& t){return SameCameraProjection({1/t[0],0,1/t[1],0,observed[4],observed[5]},observed);};
    const float observed_near=observed[5]/(observed[4]-1);
    const bool known=found!=entries_.end() &&
      ((current==found->source && matches(found->applied)) ||
       (current==found->applied && matches(found->source)));
    if((!matches(current) && !known) || !std::isfinite(near) || near<=0 ||
       !std::isfinite(observed_near) || std::abs(near-observed_near)>std::max(1e-5f,near*.005f))return false;
    if(found==entries_.end()){
      found=std::min_element(entries_.begin(),entries_.end(),[](const auto& a,const auto& b){return a.age<b.age;});
      *found={address,current,current,observed_aspect,0};
    }else if(current!=found->applied && current!=found->source){
      found->source=current;found->source_aspect=observed_aspect;
    }
    found->age=++age_;
    const float vertical=target_fov==0?found->source[1]:std::tan(target_fov*.008726646259971648f);
    const float aspect=target_aspect==0?found->source_aspect:target_aspect;
    const float horizontal=vertical*(found->source[0]/found->source[1])*aspect/found->source_aspect;
    if(!std::isfinite(horizontal)||horizontal<=.01f||horizontal>1000)return false;
    const std::array<float,2> adjusted{horizontal,vertical};
    if(adjusted!=current){
      WriteBigEndianFloat(camera+0x2c,horizontal);WriteBigEndianFloat(camera+0x30,vertical);
      // The native setter invalidates cached inverse/projection matrices.
      camera[0x140]=1;camera[0x142]=0;camera[0x143]=0;
    }
    found->applied=adjusted;
    return true;
  }
};
}
