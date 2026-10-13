// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <limits>

namespace GekkoAOT::GX {
struct CompactArrayWindow {
  std::uint32_t prefix = 0;
  std::uint32_t bytes = 0;
  explicit operator bool() const noexcept { return bytes != 0; }
};

// Raw guest indices stay unchanged. Upload the used suffix, then bias the u32
// shader address by -prefix. Align the source prefix to a word: Aurora's packed
// byte/halfword readers fetch a u32 before selecting the requested component.
inline CompactArrayWindow MakeCompactArrayWindow(
    std::uint32_t minimum, std::uint32_t maximum, std::uint32_t stride,
    std::uint32_t element, std::uint32_t available,
    std::uint32_t visible, bool shortPrefix = false) noexcept {
  if (minimum > maximum || maximum > 65535u || !element) return {};
  const std::uint64_t begin = (std::uint64_t(minimum) * stride) & ~std::uint64_t{3};
  const std::uint64_t end = std::uint64_t(maximum) * stride + element;
  if ((!shortPrefix && begin < 4096u) || end > available || end > visible || end <= begin ||
      end > std::numeric_limits<std::uint32_t>::max()) return {};
  const auto bytes = end - begin;
  // Do not replace an already cheap upload with another staging operation.
  if (bytes > std::uint64_t(visible) / 2u) return {};
  return {static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(bytes)};
}

inline std::uint32_t CompactArrayShaderBase(std::uint32_t staged,
                                          std::uint32_t prefix) noexcept {
  return staged - prefix;
}

// Stable native tiles let nearby indexed draws share the same immutable GPU
// array and therefore the same shader base. Expansion stays inside the already
// resolver-approved RAM span. The visible extent is a draw high-water, not
// an allocation bound; unused captured bytes are never addressed by the draw.
inline CompactArrayWindow TileCompactArrayWindow(CompactArrayWindow window,
                                                std::uint32_t available,
                                                std::uint32_t visible) noexcept {
  if (!window || window.bytes > 8192u) return window;
  constexpr std::uint64_t tile = 256u;
  const auto begin = std::uint64_t(window.prefix) & ~(tile - 1u);
  const auto end = std::uint64_t(window.prefix) + window.bytes;
  if (end > available || end > visible) return window;
  const auto limit = available;
  const auto rounded = (end + tile - 1u) & ~(tile - 1u);
  const auto bounded = rounded < limit ? rounded : limit;
  if (bounded <= begin || bounded - begin > 16384u) return window;
  return {static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(bounded - begin)};
}
} // namespace GekkoAOT::GX
