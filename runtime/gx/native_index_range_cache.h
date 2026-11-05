// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace GekkoAOT::GX {
// Cache only the decoded indices of a complete draw, never guest array bytes,
// resource pointers, GPU ranges, bounds checks or coherency decisions. Both
// the complete layout and complete payload must match the immutable snapshot.
class NativeIndexRangeCache {
public:
  static constexpr std::size_t Fields=12,PayloadBytes=512,LayoutBytes=128,Sets=64,Ways=4;
  struct Bounds {std::array<std::uint32_t,Fields> minimum{},maximum{};};
  struct Stats {std::uint64_t hits=0,misses=0,admitted=0,rejected=0;};
  const Bounds* Find(const void* layout,std::size_t layout_bytes,const std::uint8_t* data,std::size_t size) noexcept {
    if(!Eligible(layout,layout_bytes,data,size)){++stats_.rejected;return nullptr;}
    const auto tag=Tag(layout,layout_bytes,data,size);auto& set=entries_[tag&(Sets-1)];
    for(auto& entry:set) {
      if(entry.payload_size!=size||entry.layout_size!=layout_bytes||entry.tag!=tag)continue;
      if(std::memcmp(entry.layout.data(),layout,layout_bytes)||std::memcmp(entry.payload.data(),data,size))continue;
      entry.recent=++stamp_;++stats_.hits;return &entry.bounds;
    }
    ++stats_.misses;return nullptr;
  }
  void Insert(const void* layout,std::size_t layout_bytes,const std::uint8_t* data,std::size_t size,const Bounds& bounds) noexcept {
    if(!Eligible(layout,layout_bytes,data,size))return;
    const auto tag=Tag(layout,layout_bytes,data,size);auto& set=entries_[tag&(Sets-1)];auto* victim=&set[0];
    for(auto& entry:set)if(!entry.payload_size||entry.recent<victim->recent)victim=&entry;
    victim->tag=tag;victim->recent=++stamp_;victim->payload_size=size;victim->layout_size=layout_bytes;
    std::memcpy(victim->layout.data(),layout,layout_bytes);std::memcpy(victim->payload.data(),data,size);
    victim->bounds=bounds;++stats_.admitted;
  }
  void Clear() noexcept {for(auto& set:entries_)for(auto& e:set)e.payload_size=0;stamp_=0;stats_={};}
  const Stats& Statistics()const noexcept{return stats_;}
private:
  struct Entry {std::uint64_t tag=0,recent=0;std::uint16_t payload_size=0,layout_size=0;
    std::array<std::uint8_t,LayoutBytes> layout{};std::array<std::uint8_t,PayloadBytes> payload{};Bounds bounds;};
  static bool Eligible(const void* layout,std::size_t ls,const void* data,std::size_t size) noexcept {
    return layout&&ls&&ls<=LayoutBytes&&data&&size&&size<=PayloadBytes;
  }
  // Samples select a set only. Collisions are checked with exact bytes above.
  static std::uint64_t Tag(const void* layout,std::size_t ls,const std::uint8_t* data,std::size_t size) noexcept {
    std::uint64_t first=0,last=0,shape=0;const auto n=size<8?size:8;const auto ln=ls<8?ls:8;
    std::memcpy(&first,data,n);std::memcpy(&last,data+size-n,n);std::memcpy(&shape,layout,ln);
    auto hash=first^(last*0x9e3779b97f4a7c15ull)^shape^(size*0xbf58476d1ce4e5b9ull);
    hash^=hash>>30;hash*=0xbf58476d1ce4e5b9ull;hash^=hash>>27;return hash;
  }
  std::array<std::array<Entry,Ways>,Sets> entries_{};std::uint64_t stamp_=0;Stats stats_{};
};
} // namespace GekkoAOT::GX
