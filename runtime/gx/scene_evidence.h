// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>

namespace GekkoAOT::GX {
// Evidence only: neither perspective nor an orthographic projection proves a
// camera/world transform, HUD membership, or static geometry. These labels may
// be used for diagnostics, never to remove/reorder original draws.
enum class SceneClass : std::uint8_t {
  Unknown, Opaque3DCandidate, TransparentCandidate, InterfaceCandidate, Intermediate, Count
};
struct DrawEvidence {
  bool perspective = false, depth_test = false, depth_write = false;
  bool blended = false, offscreen = false, screen_fill = false, perspective_ui = false;
};
constexpr SceneClass ClassifyDraw(DrawEvidence draw) noexcept {
  if (draw.offscreen) return SceneClass::Intermediate;
  if (draw.screen_fill || draw.perspective_ui) return SceneClass::InterfaceCandidate;
  if (draw.blended) return SceneClass::TransparentCandidate;
  if (draw.perspective && draw.depth_test && draw.depth_write) return SceneClass::Opaque3DCandidate;
  return SceneClass::Unknown;
}
struct SceneEvidence {
  std::array<std::uint64_t, static_cast<unsigned>(SceneClass::Count)> draws{};
  void Observe(DrawEvidence draw) noexcept { ++draws[static_cast<unsigned>(ClassifyDraw(draw))]; }
  void Reset() noexcept { draws.fill(0); }
};
} // namespace GekkoAOT::GX
