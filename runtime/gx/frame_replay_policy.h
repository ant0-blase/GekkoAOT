#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace GekkoAOT::GX {
// v358 FPS unlock: a GX uniform block of the last guest frame, moved along
// the motion of the same draw's block one frame earlier.
enum class ReplayMotion : std::uint8_t { Unchanged, Moved, Cut };

// GX position/normal matrix memory: rows 0-29 hold the ten position (and
// normal) matrices; rows 30-59 of the position memory are texture matrices.
inline constexpr std::uint32_t ReplayMatrixRows = 30;

// Block layout from `proj` on: the projection (4 rows of 4 floats), the
// `pos_rows` position rows, then the normal rows. Only the projection and
// the position/normal matrices are blended: texture matrices, colours and
// lights keep the real frame's values (a scrolling or randomised texture
// offset would otherwise fly across the surface). `pnmtx` is the draw's first
// position row. `out` holds a copy of `current` and receives
// current + alpha * (current - previous) on the blended rows.
inline ReplayMotion ExtrapolateUniformBlock(const std::uint8_t* current, const std::uint8_t* previous,
                                            std::uint32_t size, std::uint32_t proj, std::uint32_t pnmtx,
                                            std::uint32_t pos_rows, float alpha, std::uint8_t* out) noexcept {
  const std::uint32_t pos = proj + 64u, nrm = pos + pos_rows * 16u;
  const std::uint32_t rows = std::min(ReplayMatrixRows, pos_rows);
  const auto in_block = [&](std::uint32_t at, std::uint32_t bytes) { return at <= size && bytes <= size - at; };
  if (!in_block(proj, 64u)) return ReplayMotion::Unchanged;
  const auto differs = [&](std::uint32_t at, std::uint32_t bytes) {
    return in_block(at, bytes) && std::memcmp(current + at, previous + at, bytes) != 0;
  };
  if (!differs(proj, 64u) && !differs(pos, rows * 16u) && !differs(nrm, rows * 16u))
    return ReplayMotion::Unchanged;
  const auto load = [](const std::uint8_t* p) { float v; std::memcpy(&v, p, 4); return v; };
  // A jump of the projection or of the draw's own matrix is a cut or a
  // teleport, not motion: the real frame's values stay.
  double delta = 0.0, magnitude = 0.0;
  const auto row = [&](std::uint32_t at) {
    if (!in_block(at, 16u)) return;
    for (std::uint32_t k = 0; k < 16u; k += 4u) {
      const double a = load(current + at + k), b = load(previous + at + k);
      delta += (a - b) * (a - b);
      magnitude += a * a + b * b;
    }
  };
  for (std::uint32_t r = 0; r < 4u; ++r) row(proj + r * 16u);
  for (std::uint32_t r = 0; r < 3u; ++r) row(pos + (pnmtx + r) * 16u);
  if (!(delta <= 0.25 * magnitude)) return ReplayMotion::Cut;
  const auto blendable = [](float v) { return v == 0.f || std::isnormal(v); };
  const auto blend = [&](std::uint32_t at, std::uint32_t bytes) {
    if (!in_block(at, bytes)) return;
    for (std::uint32_t end = at + bytes; at < end; at += 4u) {
      const float a = load(current + at), b = load(previous + at);
      if (a == b || !blendable(a) || !blendable(b)) continue;
      const float v = a + alpha * (a - b);
      std::memcpy(out + at, &v, 4);
    }
  };
  blend(proj, 64u);
  blend(pos, rows * 16u);
  blend(nrm, rows * 16u);
  return ReplayMotion::Moved;
}
} // namespace GekkoAOT::GX
