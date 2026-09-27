// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace GekkoAOT::GX {
// Raw BP addresses are 512-byte offsets in TMEM, not
// GX SDK palette names. Uploads copy 32-byte lines from main memory.
class TlutMemory {
public:
  static constexpr std::uint32_t Size = 1024 * 1024;
  // GameCube's TLUT DMA ignores the upper address bits. Retail libraries
  // sometimes leave these set; they must not become host/guest pointer bits.
  static constexpr std::uint32_t SourceAddress(std::uint32_t word) { return (word << 5u) & 0x01ffffffu; }
  static constexpr std::uint32_t Offset(std::uint32_t word) { return (word & 1023u) * 512u; }
  static constexpr std::uint32_t UploadBytes(std::uint32_t word) { return ((word >> 10) & 2047u) * 32u; }
  bool Load(std::uint32_t trigger, std::span<const std::uint8_t> source) {
    const auto offset = Offset(trigger), size = UploadBytes(trigger);
    if (source.size() != size || size > Size - offset) return false;
    if (size) {
      std::memcpy(bytes_.data() + offset, source.data(), size);
      ++revision_;
    }
    return true;
  }
  std::span<const std::uint8_t> Palette(std::uint32_t binding, std::uint32_t entries) const {
    const auto offset = Offset(binding);
    if (entries > (Size - offset) / 2) return {};
    return {bytes_.data() + offset, entries * 2u};
  }
  std::uint64_t Revision() const { return revision_; }
private:
  std::array<std::uint8_t, Size> bytes_{};
  std::uint64_t revision_ = 0;
};
}
