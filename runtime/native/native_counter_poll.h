// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>
namespace GekkoAOT::Native {
// Read-only SDK counter leaf followed by a signed elapsed-time wait. Every
// instruction and the relative call/loop target is proved, without title IDs.
struct NativeCounterPoll {
  std::uint16_t spr=0;
  std::uint8_t baseline=0;
  std::int16_t offset=0;
  template<class Read>
  static std::optional<NativeCounterPoll> Decode(std::uint32_t leaf,std::uint32_t lr,Read read) {
    std::uint32_t first,ret,call,load,subtract,compare,branch;
    if(lr<4u || !read(leaf,&first) || !read(leaf+4u,&ret) || ret!=0x4e800020u ||
       !read(lr-4u,&call) || !read(lr,&load) || !read(lr+4u,&subtract) ||
       !read(lr+8u,&compare) || !read(lr+12u,&branch)) return {};
    if((first&0xfc0007ffu)!=0x7c0002a6u || ((first>>21u)&31u)!=3u ||
       (call&0xfc000003u)!=0x48000001u || (load&0xffff0000u)!=0x800d0000u ||
       (subtract&0xffe0ffffu)!=0x7c601850u || compare!=0x7c030000u || branch!=0x4081fff0u) return {};
    // subf r3,baseline,r3: only the RA operand is variable.
    if(((subtract>>16u)&31u)==3u) return {};
    const auto displacement=static_cast<std::int32_t>((call&0x03fffffcu)<<6u)>>6u;
    if(lr-4u+static_cast<std::uint32_t>(displacement)!=leaf) return {};
    const auto spr=std::uint16_t(((first>>16u)&31u)|((first>>6u)&0x3e0u));
    if(spr!=953u && spr!=954u && spr!=957u && spr!=958u) return {};
    return NativeCounterPoll{spr,static_cast<std::uint8_t>((subtract>>16u)&31u),static_cast<std::int16_t>(load)};
  }
  template<class Read>
  static std::optional<NativeCounterPoll> DecodeBoundary(std::uint32_t pc,std::uint32_t lr,Read read) {
    if(auto leaf=Decode(pc,lr,read)) return leaf;
    std::uint32_t call;
    if(!read(pc,&call) || (call&0xfc000003u)!=0x48000001u) return {};
    const auto displacement=static_cast<std::int32_t>((call&0x03fffffcu)<<6u)>>6u;
    return Decode(pc+static_cast<std::uint32_t>(displacement),pc+4u,read);
  }
  // A core-cycle counter gives an exact scheduling horizon. Other PMC event
  // selectors, frozen counters and negative/ambiguous wraps preserve AOT.
  static std::optional<std::uint64_t> Cycles(std::uint32_t current,std::uint32_t baseline,
      std::uint32_t limit,unsigned selector,bool frozen) {
    const auto elapsed=current-baseline;
    if(frozen || selector!=1u || limit>0x7fffffffu || elapsed>0x7fffffffu || elapsed>limit) return {};
    return std::uint64_t(limit-elapsed)+1u;
  }
};
} // namespace GekkoAOT::Native
