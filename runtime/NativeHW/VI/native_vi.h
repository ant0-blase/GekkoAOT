#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>

namespace GekkoAOT::NativeHW::VI {

// A GX production counter alone cannot prove an unchanged VI image: CPU-owned
// RAM XFBs and interlaced field selection can change without GXCopyDisp.
// Only a renderer-confirmed unchanged cached image may suppress a VI request.
constexpr bool ShouldPresent(bool native_vi, std::uint64_t produced_frames,
                             std::uint64_t last_presented_frames,
                             bool unchanged_cached_image = false) noexcept {
  return !native_vi || !unchanged_cached_image || produced_frames != last_presented_frames;
}

} // namespace GekkoAOT::NativeHW::VI
