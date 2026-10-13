// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>

namespace GekkoAOT::GX {
// Encoder-local state only. A new render pass or a custom encoder invalidates
// every slot. Resource identities are native handles; offsets are part of the
// uniform binding identity. No guest state or resource lifetime is cached.
class NativeBindGroupCache {
  struct Binding {
    std::uintptr_t handle = 0;
    std::uint32_t offset = 0;
  };
  std::array<Binding, 3> bindings_{};
  std::uintptr_t index_buffer_ = 0;
  unsigned valid_ = 0;
public:
  void Reset() noexcept { valid_ = 0; }
  // v300: GX draws use one whole-buffer index binding per encoder and select
  // their range with firstIndex; same invalidation as the bind groups.
  bool NeedsIndexBuffer(std::uintptr_t handle) noexcept {
    if (!handle) return true;
    constexpr unsigned bit = 1u << 3;
    const bool changed = !(valid_ & bit) || index_buffer_ != handle;
    index_buffer_ = handle;
    valid_ |= bit;
    return changed;
  }
  bool NeedsBind(unsigned group, std::uintptr_t handle,
                 std::uint32_t offset = 0) noexcept {
    // Invalid/null bindings retain the original API call and its validation.
    if (group < 1 || group > 3 || !handle) return true;
    const unsigned slot = group - 1;
    const unsigned bit = 1u << slot;
    const bool changed = !(valid_ & bit) || bindings_[slot].handle != handle ||
                         bindings_[slot].offset != offset;
    bindings_[slot] = {handle, offset};
    valid_ |= bit;
    return changed;
  }
};
} // namespace GekkoAOT::GX
