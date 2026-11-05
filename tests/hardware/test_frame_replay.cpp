// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/frame_replay_policy.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {
int failures = 0;

void Expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

// prefix (16 bytes) + projection (64) + 64 position rows + 32 normal rows +
// light mask (16), like Aurora's GX uniform block.
constexpr std::uint32_t kProj = 16, kPosRows = 64, kRows = kProj + 64, kNrm = kRows + kPosRows * 16,
                        kMask = kNrm + 32 * 16, kSize = kMask + 16;

struct Block {
  std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(kSize, 0);
  void Set(std::uint32_t at, float v) { std::memcpy(bytes.data() + at, &v, 4); }
  float Get(std::uint32_t at) const { float v; std::memcpy(&v, bytes.data() + at, 4); return v; }
  void SetWord(std::uint32_t at, std::uint32_t v) { std::memcpy(bytes.data() + at, &v, 4); }
  std::uint32_t Word(std::uint32_t at) const { std::uint32_t v; std::memcpy(&v, bytes.data() + at, 4); return v; }
};

Block Scene(float x, std::uint32_t row = 3) {
  Block b;
  b.Set(0, 640.f);
  for (std::uint32_t i = 0; i < 4; ++i) b.Set(kProj + i * 20u, 1.f);       // identity projection
  for (std::uint32_t r = 0; r < 3; ++r) b.Set(kRows + (row + r) * 16u + r * 4u, 1.f);
  b.Set(kRows + row * 16u + 12u, x);                                     // translation x
  b.Set(kRows + (row + 2u) * 16u + 12u, -10.f);                          // translation z
  return b;
}

using GekkoAOT::GX::ExtrapolateUniformBlock;
using GekkoAOT::GX::ReplayMotion;

ReplayMotion Run(const Block& current, const Block& previous, float alpha, Block& out) {
  return ExtrapolateUniformBlock(current.bytes.data(), previous.bytes.data(), kSize, kProj, 3, kPosRows, alpha,
                                 out.bytes.data());
}

void TestUnchanged() {
  const auto a = Scene(1.f), b = Scene(1.f);
  auto out = a;
  Expect(Run(a, b, .5f, out) == ReplayMotion::Unchanged, "identical blocks are unchanged");
  Expect(out.bytes == a.bytes, "unchanged block keeps the real frame");
}

void TestMotion() {
  const auto current = Scene(1.5f), previous = Scene(1.f);
  auto out = current;
  out.Set(0, 0.f);
  Expect(Run(current, previous, .5f, out) == ReplayMotion::Moved, "translation is motion");
  Expect(out.Get(kRows + 3u * 16u + 12u) == 1.75f, "translation extrapolated by alpha");
  Expect(out.Get(kRows + 5u * 16u + 12u) == -10.f, "static component untouched");
  Expect(out.Get(0) == 0.f, "prefix before the projection is never written");
  auto back = current;
  Run(current, previous, -.5f, back);
  Expect(back.Get(kRows + 3u * 16u + 12u) == 1.25f, "negative alpha interpolates toward the previous frame");
  auto zero = current;
  Run(current, previous, 0.f, zero);
  Expect(zero.bytes == current.bytes, "alpha 0 is the real frame");
}

void TestCut() {
  auto current = Scene(1.f), previous = Scene(1.f);
  // The draw's matrix flips to an unrelated pose: a camera cut.
  current.Set(kRows + 3u * 16u, -1.f);
  current.Set(kRows + 3u * 16u + 12u, 40.f);
  previous.Set(kRows + 5u * 16u + 12u, 25.f);
  auto out = current;
  Expect(Run(current, previous, .5f, out) == ReplayMotion::Cut, "matrix jump is a cut");
  Expect(out.bytes == current.bytes, "cut keeps the real frame");
}

void TestUnrelatedRows() {
  // Another matrix slot reused by a different object jumps; the draw's own
  // rows move smoothly, so the block is still motion.
  auto current = Scene(1.5f), previous = Scene(1.f);
  current.Set(kRows + 0u * 16u + 12u, 300.f);
  previous.Set(kRows + 0u * 16u + 12u, -300.f);
  auto out = current;
  Expect(Run(current, previous, .5f, out) == ReplayMotion::Moved, "unused row jump is not a cut");
}

void TestTextureMatrix() {
  // A scrolling texture matrix (GX rows 30+) wraps from 0.98 to 0.02: it is
  // never extrapolated, even while the object moves.
  auto current = Scene(1.5f), previous = Scene(1.f);
  current.Set(kRows + 30u * 16u + 12u, .02f);
  previous.Set(kRows + 30u * 16u + 12u, .98f);
  auto out = current;
  Run(current, previous, .5f, out);
  Expect(out.Get(kRows + 30u * 16u + 12u) == .02f, "texture matrix keeps the real frame");
  // A texture matrix alone changing is not motion.
  auto still = Scene(1.f), before = Scene(1.f);
  still.Set(kRows + 30u * 16u + 12u, .5f);
  auto unchanged = still;
  Expect(Run(still, before, .5f, unchanged) == ReplayMotion::Unchanged, "texture scroll alone is unchanged");
}

void TestNormalRows() {
  auto current = Scene(1.5f), previous = Scene(1.f);
  current.Set(kNrm + 3u * 16u, .5f);
  previous.Set(kNrm + 3u * 16u, .25f);
  auto out = current;
  Run(current, previous, .5f, out);
  Expect(out.Get(kNrm + 3u * 16u) == .625f, "normal matrix rows follow the motion");
}

void TestIntegerMask() {
  auto current = Scene(1.5f), previous = Scene(1.f);
  current.SetWord(kMask, 3u);
  previous.SetWord(kMask, 1u);
  auto out = current;
  Run(current, previous, .5f, out);
  Expect(out.Word(kMask) == 3u, "integer light mask keeps the real frame");
}
} // namespace

// A matrix (rows of 4 floats) turned by `deg` about Y, scaled, translated.
std::vector<std::uint8_t> Turned(double deg, double scale, double tx) {
  std::vector<std::uint8_t> m(48, 0);
  const double r = deg * 3.14159265358979 / 180.0;
  const double v[3][4] = {{std::cos(r) * scale, 0, std::sin(r) * scale, tx},
                          {0, scale, 0, 2},
                          {-std::sin(r) * scale, 0, std::cos(r) * scale, -10}};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 4; ++j) { const float f = float(v[i][j]); std::memcpy(m.data() + i * 16 + j * 4, &f, 4); }
  return m;
}
float At(const std::vector<std::uint8_t>& m, int r, int c) { float f; std::memcpy(&f, m.data() + r * 16 + c * 4, 4); return f; }

void TestRigidBlend() {
  using GekkoAOT::GX::BlendRigidMatrix;
  // A 90 degree turn: the midpoint is a 45 degree rotation of unit scale, not
  // the 0.71 scale entrywise blending gives.
  const auto prev = Turned(0, 1, 0), cur = Turned(90, 1, 4);
  auto out = cur;
  Expect(BlendRigidMatrix(cur.data(), prev.data(), out.data(), true, .5), "rigid blend accepts a turn");
  const float c = At(out, 0, 0), s = At(out, 0, 2);
  Expect(std::abs(c - .70710678f) < 1e-4f && std::abs(s - .70710678f) < 1e-4f, "midpoint of a 90 degree turn is 45 degrees");
  Expect(std::abs(c * c + s * s - 1.f) < 1e-4f, "rigid midpoint keeps unit scale");
  Expect(std::abs(At(out, 0, 3) - 2.f) < 1e-5f && At(out, 2, 3) == -10.f, "translation is linear");
  // Scale and extrapolation: t = 1.5 continues the turn and the growth.
  const auto grow = Turned(30, 2, 0), base = Turned(0, 1, 0);
  auto ext = grow;
  Expect(BlendRigidMatrix(grow.data(), base.data(), ext.data(), true, 1.5), "rigid extrapolation");
  const float ec = At(ext, 0, 0), es = At(ext, 0, 2), scale = std::sqrt(ec * ec + es * es);
  Expect(std::abs(scale - 2.5f) < 1e-3f && std::abs(std::atan2(es, ec) - 45.f * 3.14159265f / 180.f) < 1e-3f,
         "extrapolation continues rotation and scale");
  // Mirrored in one frame only: refused (the caller blends entrywise).
  auto mirrored = Turned(10, 1, 0);
  for (int r = 0; r < 3; ++r) { const float f = -At(mirrored, r, 0); std::memcpy(mirrored.data() + r * 16, &f, 4); }
  auto refused = mirrored;
  Expect(!BlendRigidMatrix(mirrored.data(), base.data(), refused.data(), true, .5), "mirror flip is not rigid");
  // A 170 degree turn in one frame is refused too.
  const auto flip = Turned(170, 1, 0);
  auto keep = flip;
  Expect(!BlendRigidMatrix(flip.data(), base.data(), keep.data(), true, .5), "turn over 150 degrees is not blended rigidly");
}

void TestVertexArray() {
  using GekkoAOT::GX::BlendVertexArray;
  // Big-endian s16 XYZ positions, stride 8 (2 bytes of other data each).
  const auto put16 = [](std::vector<std::uint8_t>& v, std::uint32_t at, int x) {
    v[at] = std::uint8_t((x >> 8) & 0xff); v[at + 1] = std::uint8_t(x & 0xff);
  };
  const auto get16 = [](const std::vector<std::uint8_t>& v, std::uint32_t at) {
    return int(std::int16_t(std::uint16_t(v[at] << 8 | v[at + 1])));
  };
  std::vector<std::uint8_t> prev(16, 0), cur(16, 0);
  put16(prev, 0, 100); put16(prev, 2, -200); put16(prev, 4, 300); prev[6] = 7; prev[7] = 8;
  put16(prev, 8, 1000); put16(prev, 10, 0); put16(prev, 12, -50); prev[14] = 9; prev[15] = 9;
  cur = prev;
  put16(cur, 0, 120); put16(cur, 4, 280); put16(cur, 8, 1040); cur[6] = 1; // interleaved byte changes too
  auto out = cur;
  Expect(BlendVertexArray(cur.data(), prev.data(), 16, 8, 3, 3, false, .5, out.data()), "s16 BE array blends");
  Expect(get16(out, 0) == 110 && get16(out, 4) == 290 && get16(out, 8) == 1020 && get16(out, 2) == -200,
         "positions halfway, unchanged components kept");
  Expect(out[6] == 1, "interleaved data after the components stays current");
  // Little-endian floats, extrapolated.
  std::vector<std::uint8_t> fp(12), fc(12);
  const float a[3] = {1.f, 2.f, 3.f}, b[3] = {2.f, 2.f, 3.f};
  std::memcpy(fp.data(), a, 12); std::memcpy(fc.data(), b, 12);
  auto fo = fc;
  Expect(BlendVertexArray(fc.data(), fp.data(), 12, 12, 4, 3, true, 1.5, fo.data()), "f32 LE array blends");
  float r[3]; std::memcpy(r, fo.data(), 12);
  Expect(r[0] == 2.5f && r[1] == 2.f && r[2] == 3.f, "f32 extrapolation");
  // A buffer reused for unrelated data is not blended.
  std::vector<std::uint8_t> other(12);
  const float z[3] = {-500.f, 900.f, 40.f};
  std::memcpy(other.data(), z, 12);
  auto keep = other;
  Expect(!BlendVertexArray(other.data(), fp.data(), 12, 12, 4, 3, true, .5, keep.data()) && keep == other,
         "unrelated content is not blended");
}

void TestVertexSpike() {
  using GekkoAOT::GX::BlendVertexArray;
  // A 10-vertex f32 chain along x moving +0.1 in y; one vertex of the
  // previous frame stood for another point of the mesh (x 9 -> 0).
  std::vector<float> prev(30), cur(30);
  for (int i = 0; i < 10; ++i) { prev[i * 3] = float(i); cur[i * 3] = float(i); cur[i * 3 + 1] = .1f; }
  std::vector<std::uint8_t> p(120), c(120), o(120);
  std::memcpy(p.data(), prev.data(), 120); std::memcpy(c.data(), cur.data(), 120);
  o = c;
  Expect(BlendVertexArray(c.data(), p.data(), 120, 12, 4, 3, true, .5, o.data()), "chain moving as a whole blends");
  prev[27] = 0.f;
  std::memcpy(p.data(), prev.data(), 120);
  o = c;
  Expect(!BlendVertexArray(c.data(), p.data(), 120, 12, 4, 3, true, .5, o.data()), "a vertex jumping across the mesh is refused");
  // The whole chain translated by 2 (each vertex past the next one) still
  // blends: every vertex moves alike, nothing is a spike.
  for (int i = 0; i < 10; ++i) { prev[i * 3] = float(i) - 2.f; prev[i * 3 + 1] = .1f; }
  std::memcpy(p.data(), prev.data(), 120);
  o = c;
  Expect(BlendVertexArray(c.data(), p.data(), 120, 12, 4, 3, true, .5, o.data()), "uniform translation blends");
}

void TestAlignment() {
  using GekkoAOT::GX::AlignReplayBlocks;
  constexpr double inf = std::numeric_limits<double>::infinity();
  // A chain of 6 segments 1 apart moving +0.8: nearest-first would pair
  // segment k with k+1 of the previous frame; alignment keeps k with k.
  std::vector<double> cur, prev;
  for (int k = 0; k < 6; ++k) { prev.push_back(k); cur.push_back(k + .8); }
  const auto chain = [&](std::size_t i, std::size_t j) {
    const double d = std::abs(cur[i] - prev[j]);
    return d <= 1.0 ? d : inf;
  };
  std::vector<std::int32_t> partner;
  AlignReplayBlocks(cur.size(), prev.size(), chain, 1e6, partner);
  bool order = true;
  for (std::size_t k = 0; k < partner.size(); ++k) order = order && partner[k] == std::int32_t(k);
  Expect(order, "a moving chain keeps its drawing-order partners");
  // One of five look-alikes culled in the current frame: the others keep
  // their own partners (no shift), the missing one leaves a gap.
  std::vector<double> objects{0, 10, 20, 30, 40}, visible{0, 10, 30, 40};
  const auto look = [&](std::size_t i, std::size_t j) {
    const double d = std::abs(visible[i] - objects[j]);
    return d <= 2.0 ? d : inf;
  };
  AlignReplayBlocks(visible.size(), objects.size(), look, 1e6, partner);
  Expect(partner == std::vector<std::int32_t>{0, 1, 3, 4}, "a culled draw leaves a gap, not a shift");
  // A pair too far apart is never made.
  const auto far = [](std::size_t, std::size_t) { return inf; };
  AlignReplayBlocks(2, 2, far, 1e6, partner);
  Expect(partner == std::vector<std::int32_t>{-1, -1}, "gated pairs stay unpaired");
}

void TestCameraEstimate() {
  using namespace GekkoAOT::GX;
  // Camera turning 5 degrees about y and moving; three static draws with
  // different model matrices, one moving draw.
  const double a = 5.0 * 3.14159265358979 / 180.0;
  ReplayAffine view0{{{1, 0, 0, 0}, {0, 1, 0, -2}, {0, 0, 1, -10}}};
  ReplayAffine turn{{{std::cos(a), 0, std::sin(a), 0.3}, {0, 1, 0, 0}, {-std::sin(a), 0, std::cos(a), 0.1}}};
  ReplayAffine view1 = MulReplayAffine(turn, view0);
  std::vector<ReplayAffine> deltas;
  for (int i = 0; i < 3; ++i) {
    ReplayAffine model{{{1, 0, 0, double(i * 4)}, {0, 1, 0, double(i)}, {0, 0, 1, double(-i * 3)}}};
    ReplayAffine m0 = MulReplayAffine(view0, model), m1 = MulReplayAffine(view1, model), inverse;
    Expect(InvertReplayAffine(m0, inverse), "affine inverts");
    deltas.push_back(MulReplayAffine(m1, inverse));
  }
  ReplayAffine moving{{{1, 0, 0, 3}, {0, 1, 0, 0}, {0, 0, 1, 0}}}, moved{{{1, 0, 0, 3.5}, {0, 1, 0, 0}, {0, 0, 1, 0}}};
  ReplayAffine inverse;
  InvertReplayAffine(MulReplayAffine(view0, moving), inverse);
  deltas.push_back(MulReplayAffine(MulReplayAffine(view1, moved), inverse));
  ReplayAffine camera;
  Expect(EstimateReplayCamera(deltas, camera), "camera found");
  double error = 0;
  for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) error = std::max(error, std::abs(camera[r][c] - turn[r][c]));
  Expect(error < 1e-9, "camera motion is the view change");
  deltas.resize(2);
  Expect(!EstimateReplayCamera(deltas, camera), "two draws are not enough evidence");
}

void TestLookAlikes() {
  using GekkoAOT::GX::PairReplayBlocks;
  // Three still coins the game draws in another order this frame, plus a
  // moving chain: coins keep their own partners, the chain its order.
  // The chain moves 0.95 of its spacing: each segment is 0.05 from its
  // neighbour's previous place (0.05%), still no lock.
  std::vector<double> prev{0, 10, 20, 100, 101, 102, 103}, cur{20, 0, 10.00001, 100.95, 101.95, 102.95, 103.95};
  const auto d = [&](std::size_t i, std::size_t j) { return std::abs(cur[i] - prev[j]) / 100.0; };
  std::vector<std::int32_t> partner;
  PairReplayBlocks(cur.size(), prev.size(), d, 1e-4, 0.25, partner);
  Expect(partner == std::vector<std::int32_t>{2, 0, 1, 3, 4, 5, 6}, "still look-alikes lock, a moving chain keeps its order");
}

void TestParticleTurnover() {
  using GekkoAOT::GX::PairReplayBlocks;
  // Five tracked particles falling slightly off their predictions; the first
  // died and a new one was born at the end of the pool order.
  std::vector<double> predicted{0, 1, 2, 3, 4}, cur{1.2, 2.2, 3.2, 4.2, 5.3};
  const auto d = [&](std::size_t i, std::size_t j) { return std::abs(cur[i] - predicted[j]) / 100.0; };
  std::vector<std::int32_t> partner;
  PairReplayBlocks(cur.size(), predicted.size(), d, 1e-4, 0.05, partner, 0.25 * 0.05);
  Expect(partner == std::vector<std::int32_t>{1, 2, 3, 4, -1}, "survivors keep their partners, the newborn is unpaired");
}

int main() {
  TestParticleTurnover();
  TestLookAlikes();
  TestVertexSpike();
  TestAlignment();
  TestCameraEstimate();
  TestVertexArray();
  TestRigidBlend();
  TestUnchanged();
  TestMotion();
  TestCut();
  TestUnrelatedRows();
  TestTextureMatrix();
  TestNormalRows();
  TestIntegerMask();
  if (failures == 0) std::puts("frame replay policy: ok");
  return failures == 0 ? 0 : 1;
}
