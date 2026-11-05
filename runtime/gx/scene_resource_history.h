// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>

namespace GekkoAOT::GX {
struct SceneTextureUse {
  std::uint64_t resource=0,producer=0;
  std::uint32_t revision=0,draws=0,maps=0;
};
// Resource IDs identify lifetimes, not GPU addresses. A revision must match
// exactly; unknown/evicted producers never inherit another texture's camera.
// This graph records color provenance. It does not assert that sampling a
// color copy reconstructs its depth, normals, camera or world transform.
class SceneResourceHistory {
public:
  static constexpr unsigned Capacity=256;
  std::uint64_t NextPass() noexcept {return ++sequence_;}
  void Write(std::uint64_t resource,std::uint32_t revision,std::uint64_t producer) noexcept {
    if(!resource || !producer)return;
    for(auto& e:entries_)if(e.resource==resource){e={resource,producer,++clock_,revision};return;}
    auto* oldest=&entries_[0];
    for(auto& e:entries_)if(e.age<oldest->age)oldest=&e;
    *oldest={resource,producer,++clock_,revision};
  }
  std::uint64_t Read(std::uint64_t resource,std::uint32_t revision) noexcept {
    for(auto& e:entries_)if(e.resource==resource){
      e.age=++clock_;return e.revision==revision?e.producer:0;
    }
    return 0;
  }
  void Reset() noexcept {*this={};}
private:
  struct Entry {std::uint64_t resource=0,producer=0,age=0;std::uint32_t revision=0;};
  std::array<Entry,Capacity> entries_{};
  std::uint64_t sequence_=0,clock_=0;
};
} // namespace GekkoAOT::GX
