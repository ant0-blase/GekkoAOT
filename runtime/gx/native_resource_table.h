// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace GekkoAOT::GX {
// Native immutable upload resources. The upload arena itself owns the only
// cacheable copy; this table owns offsets, never pointers into reallocatable
// storage or a second copy of its bytes. Each frame/storage segment is a scope.
// Guest write notifications and content hashes are not correctness premises.
class NativeResourceTable {
  struct Entry {
    const std::uint8_t* source = nullptr;
    std::uint32_t length = 0, offset = 0;
  };
  static constexpr std::size_t Sets = 256, Ways = 4, MaxLength = 64 * 1024;
  std::array<std::array<Entry, Ways>, Sets> entries_{};
  std::array<std::uint8_t, Sets> next_{};
  std::uint64_t frame_ = 0;
  std::uint32_t segment_ = 0;
  bool scoped_ = false;
  static std::size_t Set(const std::uint8_t* source, std::size_t length) noexcept {
    const auto key = reinterpret_cast<std::uintptr_t>(source);
    return ((key >> 3) ^ (key >> 12) ^ (key >> 24) ^ (length * 0x9e3779b1u)) & (Sets - 1);
  }
public:
  void Scope(std::uint64_t frame, std::uint32_t segment) noexcept {
    if (scoped_ && frame_ == frame && segment_ == segment) return;
    entries_ = {};
    next_ = {};
    frame_ = frame; segment_ = segment; scoped_ = true;
  }
  std::optional<std::uint32_t> Find(const std::uint8_t* source, std::size_t length,
                                  std::span<const std::uint8_t> arena) const noexcept {
    if (!scoped_ || !source || !length || length > MaxLength) return {};
    for (const auto& entry : entries_[Set(source, length)]) {
      if (entry.source == source && entry.length == length &&
          entry.offset <= arena.size() && length <= arena.size() - entry.offset &&
          std::memcmp(source, arena.data() + entry.offset, length) == 0)
        return entry.offset;
    }
    return {};
  }
  // The caller has copied the source into this immutable arena range. Earlier
  // ranges survive eviction and subsequent guest writes, until scope rotation.
  bool Remember(const std::uint8_t* source, std::size_t length, std::uint32_t offset,
                std::span<const std::uint8_t> arena) noexcept {
    if (!scoped_ || !source || !length || length > MaxLength ||
        offset > arena.size() || length > arena.size() - offset) return false;
    const auto set = Set(source, length);
    auto& way = next_[set];
    entries_[set][way] = {source, static_cast<std::uint32_t>(length), offset};
    way = (way + 1u) % Ways;
    return true;
  }
};
} // namespace GekkoAOT::GX
