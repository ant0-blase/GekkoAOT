// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace GekkoAOTSdk {
struct DvdFastOpenProof { std::int16_t count_offset, fst_offset; };
// A complete leaf proof, independent of SDK addresses and SDA relocation.
// Longer/debug variants with cancellation/assertion work are left to AOT.
inline std::optional<DvdFastOpenProof> ProveDvdFastOpen(const std::uint8_t* code,
                                                     std::size_t size) noexcept {
  if (!code || size < 116) return {};
  constexpr std::array<std::uint32_t, 29> body{
    0x2c030000,0x41800038,0x800d0000,0x7c030040,0x4080002c,
    0x1cc3000c,0x806d0000,0x7c03302e,0x5400000f,0x4082000c,
    0x38000000,0x48000008,0x38000001,0x2c000000,0x4182000c,
    0x38600000,0x4e800020,0x7c633214,0x80a30004,0x38000000,
    0x38600001,0x90a40030,0x80ad0000,0x7ca53214,0x80a50008,
    0x90a40034,0x90040038,0x9004000c,0x4e800020};
  std::array<std::uint32_t,29> words{};
  for (std::size_t i=0;i<body.size();++i) {
    const auto* p=code+i*4;
    words[i]=(std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|
             (std::uint32_t(p[2])<<8)|p[3];
    const auto mask=(i==2 || i==6 || i==22)?0xffff0000u:0xffffffffu;
    if ((words[i]&mask)!=body[i]) return {};
  }
  if ((words[6]&0xffff)!=(words[22]&0xffff)) return {};
  return DvdFastOpenProof{static_cast<std::int16_t>(words[2]),
                          static_cast<std::int16_t>(words[6])};
}
} // namespace GekkoAOTSdk
