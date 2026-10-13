// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>

namespace GekkoAOT::GX {
// Persistence pays when a snapshot survives several frames. Repeatedly
// invalidated arrays use the existing mapped frame staging instead, avoiding
// an allocation, zero fill and second CPU copy inside Queue::WriteBuffer.
// This only selects storage; publication and invalidation remain authoritative.
class ArrayUploadPolicy {
 public:
  bool UsePersistent(std::uint32_t frame) noexcept {
    if (cooling_) {
      if (frame - cooldown_start_ < RetryFrames) return false;
      cooling_ = false;
    }
    if (uploaded_ && frame - last_upload_ <= RecentFrames) {
      cooling_ = true;
      cooldown_start_ = frame;
      return false;
    }
    return true;
  }
  void Uploaded(std::uint32_t frame) noexcept {
    uploaded_ = true;
    last_upload_ = frame;
  }
 private:
  static constexpr std::uint32_t RecentFrames = 4;
  static constexpr std::uint32_t RetryFrames = 64;
  std::uint32_t last_upload_ = 0;
  std::uint32_t cooldown_start_ = 0;
  bool uploaded_ = false;
  bool cooling_ = false;
};
} // namespace GekkoAOT::GX
