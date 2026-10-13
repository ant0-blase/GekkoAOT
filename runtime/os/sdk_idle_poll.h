// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>

namespace GekkoAOT::NativeOS {
// The poll must follow OSEnableInterrupts and read the r13 run-queue bitmask.
// Recognition uses instructions, never a title or address database.
template<class ReadWord>
std::optional<std::int16_t> SdkIdlePollOffset(std::uint32_t pc, ReadWord read) {
  std::uint32_t load, compare, branch, call;
  if (pc < 4u || !read(pc, &load) || !read(pc + 4u, &compare) ||
      !read(pc + 8u, &branch) || !read(pc - 4u, &call)) return {};
  if ((load & 0xffff0000u) != 0x800d0000u || compare != 0x28000000u ||
      branch != 0x4182fff8u || (call & 0xfc000003u) != 0x48000001u) return {};
  const auto displacement = static_cast<std::int32_t>(call << 6u) >> 6u;
  const auto enable = pc - 4u + static_cast<std::uint32_t>(displacement & ~3);
  constexpr std::uint32_t signature[] = {
      0x7c6000a6u, 0x60648000u, 0x7c800124u, 0x54638ffeu, 0x4e800020u};
  for (unsigned i = 0; i < 5; ++i) {
    std::uint32_t word;
    if (!read(enable + 4u * i, &word) || word != signature[i]) return {};
  }
  return static_cast<std::int16_t>(load & 0xffffu);
}
} // namespace GekkoAOT::NativeOS
