// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace GekkoAOTSdk {
template<std::size_t N>
inline bool ExactWords(const std::uint8_t* code,std::size_t size,
                       const std::array<std::uint32_t,N>& words) noexcept {
  if (!code || size<N*4u) return false;
  for (std::size_t i=0;i<N;++i) {
    const auto* p=code+4*i;
    const auto word=(std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|
                    (std::uint32_t(p[2])<<8)|p[3];
    if (word!=words[i]) return false;
  }
  return true;
}
// Whole SDK leaves, including the helper reached by older wrappers. Debug
// variants with assertions or additional GX state changes remain guest AOT.
template<class Resolve>
inline bool ProveGxNormalMatrix(const std::uint8_t* code,std::size_t size,
                                std::uint32_t pc,bool packed,Resolve resolve) {
  constexpr std::array<std::uint32_t,20> normal{
    0x1ca40003,0x3c80cc01,0x38000010,0x38a50400,0x98048000,0x64a00008,
    0x94048000,0xe0a30000,0xc0830008,0xe0630010,0xc0430018,0xe0230020,
    0xc0030028,0xf0a40000,0xd0840000,0xf0640000,0xd0440000,0xf0240000,
    0xd0040000,0x4e800020};
  constexpr std::array<std::uint32_t,18> contiguous{
    0x1ca40003,0x3c80cc01,0x38000010,0x38a50400,0x98048000,0x64a00008,
    0x94048000,0xe0830000,0xe0630008,0xe0430010,0xe0230018,0xc0030020,
    0xf0840000,0xf0640000,0xf0440000,0xf0240000,0xd0040000,0x4e800020};
  if (packed) return ExactWords(code,size,contiguous);
  if (ExactWords(code,size,normal)) return true;
  constexpr std::array<std::uint32_t,16> wrapper{
    0x7c0802a6,0x90010004,0x9421fff8,0x1c840003,0x38040400,0x3ca0cc01,
    0x38c00010,0x98c58000,0x64000008,0x38858000,0x90058000,0,
    0x8001000c,0x38210008,0x7c0803a6,0x4e800020};
  if (!code || size<64u) return false;
  for (unsigned i=0;i<16;++i) {
    if (i==11) continue;
    if (!ExactWords(code+4*i,4u,std::array<std::uint32_t,1>{wrapper[i]})) return false;
  }
  const auto* b=code+44;
  const auto branch=(std::uint32_t(b[0])<<24)|(std::uint32_t(b[1])<<16)|
                    (std::uint32_t(b[2])<<8)|b[3];
  if ((branch&0xfc000003u)!=0x48000001u) return false;
  const auto displacement=static_cast<std::int32_t>((branch&0x03fffffcu)<<6)>>6;
  constexpr std::array<std::uint32_t,13> helper{
    0xe0030000,0xc0230008,0xe0430010,0xc0630018,0xe0830020,0xc0a30028,
    0xf0040000,0xd0240000,0xf0440000,0xd0640000,0xf0840000,0xd0a40000,
    0x4e800020};
  return ExactWords(resolve(pc+44u+static_cast<std::uint32_t>(displacement),52u),52u,helper);
}
} // namespace GekkoAOTSdk
