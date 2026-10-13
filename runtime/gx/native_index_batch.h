// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace GekkoAOT::GX {
// Rebase only the NEW immutable draw's native index span. Reject malformed or
// overflowing spans before changing any byte; previously recorded draws survive.
inline bool RebaseNativeIndexSpan(std::uint8_t* bytes, std::size_t size,
                                  std::uint32_t base, std::uint32_t vertices) noexcept {
  if (!bytes || !size || (size & 1u) || !vertices || vertices > 65535u ||
      base > 65535u - vertices) return false;
  for (std::size_t at = 0; at < size; at += 2) {
    std::uint16_t index;
    std::memcpy(&index, bytes + at, 2);
    if (index >= vertices) return false;
  }
  for (std::size_t at = 0; at < size; at += 2) {
    std::uint16_t index;
    std::memcpy(&index, bytes + at, 2);
    index = static_cast<std::uint16_t>(std::uint32_t(index) + base);
    std::memcpy(bytes + at, &index, 2);
  }
  return true;
}
} // namespace GekkoAOT::GX
