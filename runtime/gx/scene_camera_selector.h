// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace GekkoAOT::GX {
// Select camera evidence over a VI frame, including all intermediate EFB
// copies. A final viewmodel draw must not replace the scene's lens identity.
class SceneCameraSelector {
  struct Candidate {
    std::array<float,10> key{};
    float area=0,depth_span=0;
    std::uint64_t vertices=0;
  };
  std::array<Candidate,32> candidates_{};
  unsigned count_=0,best_=0;
  std::uint64_t epoch_=~std::uint64_t{0};
public:
  void Reset() noexcept {*this=SceneCameraSelector{};}
  bool Observe(std::uint64_t epoch,const std::array<float,10>& key,
               float depth_span,unsigned vertices) noexcept {
    const float area=key[8]*key[9];
    if(!std::isfinite(area)||area<=0||!std::isfinite(depth_span)||depth_span<=0)return false;
    if(epoch_!=epoch){epoch_=epoch;count_=0;best_=0;}
    unsigned index=0;
    while(index<count_ && candidates_[index].key!=key)++index;
    if(index==count_){
      if(count_==candidates_.size())return false;
      candidates_[count_++]={key,area,depth_span,0};
    }
    auto& current=candidates_[index];current.vertices+=vertices;
    const auto score=[](const auto& c){
      const double span=std::min(c.depth_span,1.f);
      return double(c.area)*double(c.vertices)*span*span*span*span;
    };
    if(score(current)>score(candidates_[best_]))best_=index;
    return best_==index;
  }
};
}
