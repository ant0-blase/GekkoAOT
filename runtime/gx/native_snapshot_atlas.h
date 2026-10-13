// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <optional>
#include <vector>

namespace GekkoAOT::GX {
// Recording-thread cache of immutable native storage snapshots. Never trusts
// guest write notifications: equality is proven against the uploaded bytes.
// Comparisons use ordinary cacheable CPU memory. Mapped GPU staging can be
// write-combined; reading it on every draw is catastrophically slow. Capture
// once, upload those exact bytes, then retain offsets, never staging pointers.
class NativeSnapshotAtlas {
  struct Entry {
    const std::uint8_t* source = nullptr;
    std::uint32_t length = 0, offset = 0, shadow = 0, capacity = 0;
  };
  static constexpr std::size_t kSets = 256, kWays = 4;
  static constexpr std::size_t kMaxLength = 64 * 1024;
  static constexpr std::size_t kShadowBudget = 4 * 1024 * 1024;
  std::array<std::array<Entry, kWays>, kSets> entries_{};
  std::array<std::uint8_t, kSets> next_{};
  std::uint64_t frame_ = 0;
  std::uint32_t segment_ = 0;
  bool scoped_ = false;
  std::vector<std::uint8_t> shadow_;
  std::size_t budget_ = kShadowBudget;
  bool recycle_ = false;
  std::uint64_t recycled_ = 0, budget_declines_ = 0;

  static bool RecyclingEnabled() noexcept {
    static const bool enabled = [] {
      const char* flag = std::getenv("GEKKOAOT_NATIVE_GX_SNAPSHOT_RECYCLING");
      return flag && flag[0] == '1';
    }();
    return enabled;
  }

  static std::size_t Set(const std::uint8_t* source, std::size_t length) noexcept {
    const auto key = reinterpret_cast<std::uintptr_t>(source);
    return ((key >> 3) ^ (key >> 12) ^ (key >> 24) ^ (length * 0x9e3779b1u)) & (kSets - 1);
  }
public:
  explicit NativeSnapshotAtlas(std::size_t budget = kShadowBudget,
                               bool recycle = RecyclingEnabled()) noexcept
      : budget_(budget < kShadowBudget ? budget : kShadowBudget), recycle_(recycle) {}
  struct Snapshot {
    const std::uint8_t* bytes = nullptr;
    std::uint32_t offset = 0;
    std::uint32_t capacity = 0;
    explicit operator bool() const noexcept { return bytes != nullptr; }
  };
  std::size_t ShadowBytes() const noexcept { return shadow_.size(); }
  std::uint64_t RecycledSnapshots() const noexcept { return recycled_; }
  std::uint64_t BudgetDeclines() const noexcept { return budget_declines_; }
  void Scope(std::uint64_t frame, std::uint32_t segment) noexcept {
    if (scoped_ && frame_ == frame && segment_ == segment) return;
    entries_ = {};
    next_ = {};
    shadow_.clear();
    frame_ = frame;
    segment_ = segment;
    scoped_ = true;
  }
  std::optional<std::uint32_t> Find(const std::uint8_t* source, std::size_t length,
                                  std::size_t size) const noexcept {
    if (!scoped_ || !source || !length || length > kMaxLength) return {};
    for (const auto& entry : entries_[Set(source, length)]) {
      if (entry.source == source && entry.length == length && entry.offset <= size &&
          length <= size - entry.offset &&
          std::memcmp(source, shadow_.data() + entry.shadow, length) == 0)
        return entry.offset;
    }
    return {};
  }
  Snapshot Capture(const std::uint8_t* source, std::size_t length) {
    if (!scoped_ || !source || !length || length > kMaxLength)
      return {};
    // Only the CPU comparison shadow is recycled. GPU ranges already contain
    // independent immutable copies and are never overwritten here. Capture
    // and Remember are paired on the serialized recording thread. An entry
    // with a pending, unremembered capture is never eligible for reclamation.
    if (recycle_) {
      const auto set = Set(source, length);
      auto& victim = entries_[set][next_[set]];
      if (victim.source && victim.capacity >= length) {
        const auto offset = victim.shadow, capacity = victim.capacity;
        victim = {}; // Invalidate before bytes belonging to the old key change.
        std::memcpy(shadow_.data() + offset, source, length);
        ++recycled_;
        return {shadow_.data() + offset, offset, capacity};
      }
    }
    if (length > budget_ - shadow_.size()) {
      ++budget_declines_;
      return {};
    }
    const auto offset = static_cast<std::uint32_t>(shadow_.size());
    const auto required = shadow_.size() + length;
    if (required > shadow_.capacity()) {
      const auto grown = shadow_.capacity() * 2u;
      const auto wanted = grown > required ? grown : required;
      shadow_.reserve(wanted < budget_ ? wanted : budget_);
    }
    shadow_.insert(shadow_.end(), source, source + length);
    return {shadow_.data() + offset, offset, static_cast<std::uint32_t>(length)};
  }
  void Remember(const std::uint8_t* source, std::size_t length, std::uint32_t offset,
                Snapshot snapshot) noexcept {
    if (!scoped_ || !source || !length || length > kMaxLength || !snapshot ||
        snapshot.offset > shadow_.size() || length > shadow_.size() - snapshot.offset ||
        snapshot.capacity < length || snapshot.capacity > shadow_.size() - snapshot.offset) return;
    const auto set = Set(source, length);
    auto& way = next_[set];
    entries_[set][way] = {source, static_cast<std::uint32_t>(length), offset, snapshot.offset, snapshot.capacity};
    way = (way + 1u) % kWays;
  }
};
} // namespace GekkoAOT::GX
