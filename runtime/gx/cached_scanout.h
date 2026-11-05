// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace GekkoAOT::GX {
// Only immutable GX copy snapshots have a generation suitable for deduping.
// RAM framebuffers must still be observed on VI fields, even without GX draws.
struct CachedScanoutKey {
  std::uintptr_t texture = 0;
  std::uint64_t generation = 0;
  std::uint32_t top = 0, bottom = 0;
  std::uint32_t width = 0, height = 0, aspect = 0;
  bool operator==(const CachedScanoutKey&) const = default;
};
class CachedScanout {
 public:
  bool Duplicate(const CachedScanoutKey& key) const noexcept {
    return valid_ && key_ == key;
  }
  void Queued(const CachedScanoutKey& key) noexcept { key_ = key; valid_ = true; }
  void Reset() noexcept { valid_ = false; }
 private:
  CachedScanoutKey key_{};
  bool valid_ = false;
};
} // namespace GekkoAOT::GX
