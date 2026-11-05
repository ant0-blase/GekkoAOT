#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace GekkoAOT::GX {
// v358 FPS unlock: a GX uniform block of the last guest frame, moved along
// the motion of the same draw's block one frame earlier.
enum class ReplayMotion : std::uint8_t { Unchanged, Moved, Cut };

// GX position/normal matrix memory: rows 0-29 hold the ten position (and
// normal) matrices; rows 30-59 of the position memory are texture matrices.
inline constexpr std::uint32_t ReplayMatrixRows = 30;

// Rigid blend of one 3x4 (position) or 3x3 (normal) matrix stored as three
// 16-byte rows: A = R S (polar decomposition), R interpolated by quaternion
// slerp, the stretch S and the translation linearly, at t from `previous`
// (t = 0) to `current` (t = 1); t > 1 extrapolates. Entrywise blending
// shrinks and shears a turning object (a 90 degree turn passes through a
// matrix of scale 0.71). Returns false (nothing written) for matrices that
// are not a proper rotation times a stretch in both frames (singular,
// mirrored in one frame only, non-finite) or that turn by more than 150
// degrees: the caller blends those entrywise.
// The frame-pair part of a rigid blend (polar decompositions, quaternions,
// stretches): computed once per pair of guest frames, applied per replay.
struct RigidPair {
  bool ok = false, translation = false;
  double sign = 1, angle = 0;
  double qa[4]{}, qb[4]{}, sa[3][3]{}, sb[3][3]{}, ta[3]{}, tb[3]{};
};
inline RigidPair PrepareRigidPair(const std::uint8_t* current, const std::uint8_t* previous, bool translation) noexcept {
  RigidPair pair;
  pair.translation = translation;
  using M3 = double[3][3];
  double ca[3][4], pa[3][4];
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c) {
      float a, b;
      std::memcpy(&a, current + r * 16 + c * 4, 4);
      std::memcpy(&b, previous + r * 16 + c * 4, 4);
      if (!std::isfinite(a) || !std::isfinite(b)) return pair;
      ca[r][c] = a; pa[r][c] = b;
    }
  const auto det = [](const M3& m) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  };
  // Polar rotation by Newton iteration Q <- (Q + Q^-T) / 2 (quadratic).
  const auto polar = [&](const double (&a)[3][4], M3& q, M3& stretch, double& sign) {
    M3 m;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) m[r][c] = a[r][c];
    const double d = det(m);
    double scale = 0;
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) scale = std::max(scale, std::abs(m[r][c]));
    if (!(scale > 0) || !(std::abs(d) > 1e-9 * scale * scale * scale)) return false;
    sign = d < 0 ? -1.0 : 1.0; // a mirrored matrix: interpolate -A's rotation
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) q[r][c] = sign * m[r][c];
    for (int it = 0; it < 12; ++it) {
      const double dq = det(q);
      if (!(std::abs(dq) > 1e-30)) return false;
      M3 inv_t; // (Q^-1)^T = cofactor(Q) / det(Q)
      inv_t[0][0] = (q[1][1] * q[2][2] - q[1][2] * q[2][1]) / dq;
      inv_t[0][1] = -(q[1][0] * q[2][2] - q[1][2] * q[2][0]) / dq;
      inv_t[0][2] = (q[1][0] * q[2][1] - q[1][1] * q[2][0]) / dq;
      inv_t[1][0] = -(q[0][1] * q[2][2] - q[0][2] * q[2][1]) / dq;
      inv_t[1][1] = (q[0][0] * q[2][2] - q[0][2] * q[2][0]) / dq;
      inv_t[1][2] = -(q[0][0] * q[2][1] - q[0][1] * q[2][0]) / dq;
      inv_t[2][0] = (q[0][1] * q[1][2] - q[0][2] * q[1][1]) / dq;
      inv_t[2][1] = -(q[0][0] * q[1][2] - q[0][2] * q[1][0]) / dq;
      inv_t[2][2] = (q[0][0] * q[1][1] - q[0][1] * q[1][0]) / dq;
      double change = 0;
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
          const double v = .5 * (q[r][c] + inv_t[r][c]);
          change = std::max(change, std::abs(v - q[r][c]));
          q[r][c] = v;
        }
      if (change < 1e-12) break;
    }
    // S = Q^T (sign A): symmetric stretch in the object's own frame.
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) {
        double v = 0;
        for (int k = 0; k < 3; ++k) v += q[k][r] * sign * m[k][c];
        stretch[r][c] = v;
      }
    return true;
  };
  const auto quat = [](const M3& m, double (&q)[4]) { // w, x, y, z (Shepperd)
    const double tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0) {
      const double s = std::sqrt(tr + 1.0) * 2;
      q[0] = .25 * s; q[1] = (m[2][1] - m[1][2]) / s; q[2] = (m[0][2] - m[2][0]) / s; q[3] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
      const double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2;
      q[0] = (m[2][1] - m[1][2]) / s; q[1] = .25 * s; q[2] = (m[0][1] + m[1][0]) / s; q[3] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
      const double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2;
      q[0] = (m[0][2] - m[2][0]) / s; q[1] = (m[0][1] + m[1][0]) / s; q[2] = .25 * s; q[3] = (m[1][2] + m[2][1]) / s;
    } else {
      const double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2;
      q[0] = (m[1][0] - m[0][1]) / s; q[1] = (m[0][2] + m[2][0]) / s; q[2] = (m[1][2] + m[2][1]) / s; q[3] = .25 * s;
    }
  };
  M3 qc, qp;
  double signc = 1, signp = 1;
  if (!polar(ca, qc, pair.sb, signc) || !polar(pa, qp, pair.sa, signp) || signc != signp) return pair;
  quat(qp, pair.qa); quat(qc, pair.qb);
  double dot = 0;
  for (int i = 0; i < 4; ++i) dot += pair.qa[i] * pair.qb[i];
  if (dot < 0) { for (auto& v : pair.qb) v = -v; dot = -dot; }
  pair.angle = std::acos(std::min(dot, 1.0)); // half the rotation angle
  if (pair.angle > 75.0 * 3.14159265358979 / 180.0) return pair;
  for (int r = 0; r < 3; ++r) { pair.ta[r] = pa[r][3]; pair.tb[r] = ca[r][3]; }
  pair.sign = signc;
  pair.ok = true;
  return pair;
}
// Writes the blended matrix at t (0 = previous, 1 = current) into `out`.
inline void ApplyRigidPair(const RigidPair& p, double t, std::uint8_t* out) noexcept {
  double q[4];
  if (p.angle < 1e-6) {
    for (int i = 0; i < 4; ++i) q[i] = p.qa[i] + t * (p.qb[i] - p.qa[i]);
  } else {
    const double wa = std::sin((1 - t) * p.angle) / std::sin(p.angle), wb = std::sin(t * p.angle) / std::sin(p.angle);
    for (int i = 0; i < 4; ++i) q[i] = wa * p.qa[i] + wb * p.qb[i];
  }
  const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  for (auto& v : q) v /= n;
  const double w = q[0], x = q[1], y = q[2], z = q[3];
  const double rot[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                            {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                            {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      double v = 0;
      for (int k = 0; k < 3; ++k) v += rot[r][k] * (p.sa[k][c] + t * (p.sb[k][c] - p.sa[k][c]));
      const float f = static_cast<float>(p.sign * v);
      std::memcpy(out + r * 16 + c * 4, &f, 4);
    }
    if (p.translation) {
      const float f = static_cast<float>(p.ta[r] + t * (p.tb[r] - p.ta[r]));
      std::memcpy(out + r * 16 + 12, &f, 4);
    }
  }
}
inline bool BlendRigidMatrix(const std::uint8_t* current, const std::uint8_t* previous, std::uint8_t* out,
                             bool translation, double t) noexcept {
  const RigidPair pair = PrepareRigidPair(current, previous, translation);
  if (!pair.ok) return false;
  ApplyRigidPair(pair, t, out);
  return true;
}

// CPU-animated vertex arrays (skinned or deformed by the game every frame):
// the array of the newest frame blended toward the same array (same guest
// address, size, stride and format) of the previous frame, component by
// component in the array's own format (GX_U8, GX_S8, GX_U16, GX_S16, GX_F32;
// big- or little-endian), at t from previous (0) to current (1). Bytes of
// the stride after the `count` components (other interleaved data) stay
// current. Returns false and writes nothing for an array whose motion is out
// of proportion to its extent (a buffer reused for another object): the
// caller keeps the newest frame's vertices.
inline bool BlendVertexArray(const std::uint8_t* current, const std::uint8_t* previous, std::uint32_t size,
                             std::uint32_t stride, std::uint8_t comp, std::uint8_t count, bool little_endian,
                             double t, std::uint8_t* out) noexcept {
  const std::uint32_t width = comp == 0 || comp == 1 ? 1u : comp == 2 || comp == 3 ? 2u : comp == 4 ? 4u : 0u;
  if (width == 0 || count == 0 || count > 3 || stride < width * count || size < stride) return false;
  const auto load = [&](const std::uint8_t* p) -> double {
    std::uint32_t raw = 0;
    for (std::uint32_t i = 0; i < width; ++i) raw |= std::uint32_t(p[little_endian ? i : width - 1 - i]) << (8 * i);
    switch (comp) {
      case 0: return double(std::uint8_t(raw));
      case 1: return double(std::int8_t(raw));
      case 2: return double(std::uint16_t(raw));
      case 3: return double(std::int16_t(raw));
      default: { float f; std::memcpy(&f, &raw, 4); return double(f); }
    }
  };
  const auto store = [&](std::uint8_t* p, double v) {
    std::uint32_t raw;
    switch (comp) {
      case 0: raw = std::uint8_t(std::clamp(std::lround(v), 0l, 255l)); break;
      case 1: raw = std::uint8_t(std::int8_t(std::clamp(std::lround(v), -128l, 127l))); break;
      case 2: raw = std::uint16_t(std::clamp(std::lround(v), 0l, 65535l)); break;
      case 3: raw = std::uint16_t(std::int16_t(std::clamp(std::lround(v), -32768l, 32767l))); break;
      default: { const float f = float(v); std::memcpy(&raw, &f, 4); }
    }
    for (std::uint32_t i = 0; i < width; ++i) p[little_endian ? i : width - 1 - i] = std::uint8_t(raw >> (8 * i));
  };
  // Proportion test: the mean change against the mean magnitude. Spike test:
  // no vertex moving both four times farther than the median vertex and
  // more than half the mesh's extent (a vertex that now stands for another
  // point of the mesh; a mesh moving as a whole passes).
  double delta = 0, magnitude = 0;
  double lo[3] = {INFINITY, INFINITY, INFINITY}, hi[3] = {-INFINITY, -INFINITY, -INFINITY};
  thread_local std::vector<double> moved;
  moved.clear();
  for (std::uint32_t at = 0; at + width * count <= size; at += stride) {
    double d = 0;
    for (std::uint32_t c = 0; c < count; ++c) {
      const double a = load(current + at + c * width), b = load(previous + at + c * width);
      if (!std::isfinite(a) || !std::isfinite(b)) return false;
      delta += (a - b) * (a - b);
      magnitude += a * a + b * b;
      d += (a - b) * (a - b);
      lo[c] = std::min(lo[c], a);
      hi[c] = std::max(hi[c], a);
    }
    moved.push_back(d);
  }
  if (!(delta <= 0.25 * magnitude)) return false;
  if (moved.size() >= 4) {
    double extent = 0;
    for (std::uint32_t c = 0; c < count; ++c) extent += (hi[c] - lo[c]) * (hi[c] - lo[c]);
    const double worst = *std::max_element(moved.begin(), moved.end());
    std::nth_element(moved.begin(), moved.begin() + moved.size() / 2, moved.end());
    const double median = moved[moved.size() / 2];
    if (worst > 16.0 * median && worst > 0.25 * extent) return false;
  }
  for (std::uint32_t at = 0; at + width * count <= size; at += stride)
    for (std::uint32_t c = 0; c < count; ++c) {
      const double a = load(current + at + c * width), b = load(previous + at + c * width);
      if (a != b) store(out + at + c * width, b + t * (a - b));
    }
  return true;
}

// Block layout from `proj` on: the projection (4 rows of 4 floats), the
// `pos_rows` position rows, then the normal rows. Only the projection and
// the position/normal matrices are blended: texture matrices, colours and
// lights keep the real frame's values (a scrolling or randomised texture
// offset would otherwise fly across the surface). `pnmtx` is the draw's first
// position row. `out` holds a copy of `current` and receives
// current + alpha * (current - previous) on the blended rows.
// `rigid(current_rows, previous_rows, translation)` returns the frame-pair
// part of a matrix's rigid blend (callers cache it across replays).
template <class Rigid>
inline ReplayMotion ExtrapolateUniformBlock(const std::uint8_t* current, const std::uint8_t* previous,
                                            std::uint32_t size, std::uint32_t proj, std::uint32_t pnmtx,
                                            std::uint32_t pos_rows, float alpha, std::uint8_t* out,
                                            Rigid&& rigid) noexcept {
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
  // Position and normal matrices: rigidly, matrix by matrix (entrywise only
  // where that is impossible).
  const auto blend_matrices = [&](std::uint32_t base, bool translation) {
    for (std::uint32_t m = 0; m + 3u <= rows; m += 3u) {
      const std::uint32_t at = base + m * 16u;
      if (!differs(at, 48u)) continue;
      const RigidPair& pair = rigid(current + at, previous + at, translation);
      if (pair.ok) ApplyRigidPair(pair, 1.0 + double(alpha), out + at);
      else blend(at, 48u);
    }
  };
  blend_matrices(pos, true);
  blend_matrices(nrm, false);
  return ReplayMotion::Moved;
}
inline ReplayMotion ExtrapolateUniformBlock(const std::uint8_t* current, const std::uint8_t* previous,
                                            std::uint32_t size, std::uint32_t proj, std::uint32_t pnmtx,
                                            std::uint32_t pos_rows, float alpha, std::uint8_t* out) noexcept {
  RigidPair scratch;
  return ExtrapolateUniformBlock(current, previous, size, proj, pnmtx, pos_rows, alpha, out,
                                 [&](const std::uint8_t* c, const std::uint8_t* p, bool t) -> const RigidPair& {
                                   scratch = PrepareRigidPair(c, p, t);
                                   return scratch;
                                 });
}
// A 3x4 position matrix (GX matrix memory rows: three rows of four floats,
// 16 bytes apart), in double precision.
using ReplayAffine = std::array<std::array<double, 4>, 3>;

inline ReplayAffine LoadReplayAffine(const std::uint8_t* rows) noexcept {
  ReplayAffine a{};
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c) { float v; std::memcpy(&v, rows + r * 16 + c * 4, 4); a[r][c] = v; }
  return a;
}
// Writes the 3x3 part only when `translation` is false (normal matrices).
inline void StoreReplayAffine(const ReplayAffine& a, std::uint8_t* rows, bool translation = true) noexcept {
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < (translation ? 4 : 3); ++c) { const float v = float(a[r][c]); std::memcpy(rows + r * 16 + c * 4, &v, 4); }
}
inline ReplayAffine MulReplayAffine(const ReplayAffine& a, const ReplayAffine& b) noexcept {
  ReplayAffine o{};
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c)
      o[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + (c == 3 ? a[r][3] : 0.0);
  return o;
}
inline bool InvertReplayAffine(const ReplayAffine& a, ReplayAffine& o) noexcept {
  const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
                     a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                     a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
  if (!std::isfinite(det) || std::abs(det) < 1e-12) return false;
  const double k = 1.0 / det;
  o[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) * k;
  o[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * k;
  o[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * k;
  o[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) * k;
  o[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * k;
  o[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * k;
  o[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) * k;
  o[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * k;
  o[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * k;
  for (int r = 0; r < 3; ++r) o[r][3] = -(o[r][0] * a[0][3] + o[r][1] * a[1][3] + o[r][2] * a[2][3]);
  return true;
}
inline std::array<double, 3> ApplyReplayAffine(const ReplayAffine& a, const std::array<double, 3>& p) noexcept {
  return {a[0][0] * p[0] + a[0][1] * p[1] + a[0][2] * p[2] + a[0][3],
          a[1][0] * p[0] + a[1][1] * p[1] + a[1][2] * p[2] + a[1][3],
          a[2][0] * p[0] + a[2][1] * p[1] + a[2][2] * p[2] + a[2][3]};
}

// Camera motion between two guest frames: for a draw that is still in the
// world, current = C * previous with the same C (the view change) whatever
// its model matrix. `deltas` holds current * inverse(previous) of candidate
// draw pairs; C is the value most of them agree on (at least 3 and a fifth of
// them), or false (no estimate: a cut, or a scene with nothing static).
inline bool EstimateReplayCamera(const std::vector<ReplayAffine>& deltas, ReplayAffine& camera,
                                 std::size_t* support = nullptr) noexcept {
  const auto agree = [](const ReplayAffine& a, const ReplayAffine& b) {
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c)
        if (std::abs(a[r][c] - b[r][c]) > 2e-3 * (1.0 + std::abs(a[r][c]))) return false;
      if (std::abs(a[r][3] - b[r][3]) > 2e-3 * (1.0 + std::abs(a[r][3]))) return false;
    }
    return true;
  };
  const std::size_t n = std::min<std::size_t>(deltas.size(), 256);
  std::size_t best = 0, votes = 0;
  for (std::size_t i = 0; i < n; ++i) {
    std::size_t count = 0;
    for (std::size_t j = 0; j < n; ++j) count += agree(deltas[i], deltas[j]);
    if (count > votes) { votes = count; best = i; }
  }
  if (support) *support = votes;
  if (votes < 3 || votes * 5 < n) return false;
  camera = deltas[best];
  return true;
}

// Pairs the blocks one draw signature builds in two frames. Games draw their
// scene in a stable order, so blocks align in order (the k-th hose segment
// with the k-th), with gaps for draws culled or added (an alignment, not
// nearest neighbours: neighbours of a moving chain swap partners). `cost(i,
// j)` is how far block i of the current frame is from where block j of the
// previous frame would be (infinite: impossible); a gap costs `gap`. With a
// gap above any total of costs, drawing order decides and positions only
// veto pairs and place the unavoidable gaps. partner[i] = j or -1.
template <class Cost>
inline void AlignReplayBlocks(std::size_t n, std::size_t m, Cost&& cost, double gap, std::vector<std::int32_t>& partner) {
  partner.assign(n, -1);
  if (n == 0 || m == 0) return;
  constexpr double inf = std::numeric_limits<double>::infinity();
  std::vector<double> d((n + 1) * (m + 1), inf);
  std::vector<std::uint8_t> step((n + 1) * (m + 1), 0); // 1 match, 2 skip current, 3 skip previous
  const auto at = [m](std::size_t i, std::size_t j) { return i * (m + 1) + j; };
  d[at(0, 0)] = 0;
  for (std::size_t i = 0; i <= n; ++i)
    for (std::size_t j = 0; j <= m; ++j) {
      const double here = d[at(i, j)];
      if (here == inf) continue;
      if (i < n && j < m) {
        const double c = cost(i, j);
        if (c != inf && here + c < d[at(i + 1, j + 1)]) { d[at(i + 1, j + 1)] = here + c; step[at(i + 1, j + 1)] = 1; }
      }
      if (i < n && here + gap < d[at(i + 1, j)]) { d[at(i + 1, j)] = here + gap; step[at(i + 1, j)] = 2; }
      if (j < m && here + gap < d[at(i, j + 1)]) { d[at(i, j + 1)] = here + gap; step[at(i, j + 1)] = 3; }
    }
  for (std::size_t i = n, j = m; i > 0 || j > 0;) {
    const auto s = step[at(i, j)];
    if (s == 1) { partner[--i] = std::int32_t(--j); }
    else if (s == 2) --i;
    else --j;
  }
}
// Pairs one draw signature's blocks of two frames. `distance(i, j)`: how far
// current block i is from where previous block j would be (infinite:
// impossible). First, draws that stand (nearly) where a look-alike stood --
// mutual nearest within `lock` -- pair whatever their drawing order (coins or
// pickups the game re-sorts); the rest align in drawing order (moving chains,
// whose neighbours swap nearest), each pair within `gate`. `gap`: the cost of
// leaving a block unpaired (huge: drawing order decides; about the gate, with
// distances measured from predicted positions: a slot reused by a new object
// becomes two gaps instead of a long pair).
template <class Distance>
inline void PairReplayBlocks(std::size_t n, std::size_t m, Distance&& distance, double lock, double gate,
                             std::vector<std::int32_t>& partner, double gap = 1e6) {
  constexpr double inf = std::numeric_limits<double>::infinity();
  partner.assign(n, -1);
  if (n == 0 || m == 0) return;
  std::vector<double> d(n * m);
  std::vector<std::size_t> nearest_previous(n, m), nearest_current(m, n);
  for (std::size_t i = 0; i < n; ++i)
    for (std::size_t j = 0; j < m; ++j) {
      const double v = d[i * m + j] = distance(i, j);
      if (v == inf) continue;
      if (nearest_previous[i] == m || v < d[i * m + nearest_previous[i]]) nearest_previous[i] = j;
      if (nearest_current[j] == n || v < d[nearest_current[j] * m + j]) nearest_current[j] = i;
    }
  std::vector<bool> taken(m, false);
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t j = nearest_previous[i];
    if (j < m && nearest_current[j] == i && d[i * m + j] <= lock) { partner[i] = std::int32_t(j); taken[j] = true; }
  }
  std::vector<std::size_t> rest_current, rest_previous;
  for (std::size_t i = 0; i < n; ++i) if (partner[i] < 0) rest_current.push_back(i);
  for (std::size_t j = 0; j < m; ++j) if (!taken[j]) rest_previous.push_back(j);
  std::vector<std::int32_t> aligned;
  AlignReplayBlocks(rest_current.size(), rest_previous.size(), [&](std::size_t a, std::size_t b) {
    const double v = d[rest_current[a] * m + rest_previous[b]];
    return v <= gate ? v : inf;
  }, gap, aligned);
  for (std::size_t a = 0; a < rest_current.size(); ++a)
    if (aligned[a] >= 0) partner[rest_current[a]] = std::int32_t(rest_previous[std::size_t(aligned[a])]);
}
} // namespace GekkoAOT::GX
