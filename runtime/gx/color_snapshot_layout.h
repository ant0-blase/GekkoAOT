// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <optional>

namespace GekkoAOT::GX {
struct ColorSnapshotLayout {
  std::uint32_t row_bytes, row_pitch;
  std::uint64_t byte_size;
};
// WebGPU texture readback rows have 256-byte alignment. Reject overflow before
// allocation, including the padding of the last row used by asynchronous maps.
inline std::optional<ColorSnapshotLayout> ColorSnapshotExtent(std::uint32_t width,
                                                             std::uint32_t height) noexcept {
  const auto bytes = std::uint64_t(width) * 4u;
  const auto pitch = (bytes + 255u) & ~std::uint64_t{255u};
  if (!width || !height || pitch > UINT32_MAX || pitch * height > UINT32_MAX) return {};
  return ColorSnapshotLayout{static_cast<std::uint32_t>(bytes), static_cast<std::uint32_t>(pitch), pitch * height};
}
} // namespace GekkoAOT::GX
