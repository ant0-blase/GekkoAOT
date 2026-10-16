// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/frame_replay_policy.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
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

int main() {
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
