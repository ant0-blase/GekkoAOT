// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/widescreen_policy.h"
#include "../../runtime/gx/frustum_assist.h"
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
int main() {
  using namespace GekkoAOT::GX;
  const auto target = 16.f / 9.f;
  const auto original = ProjectionAspect(1.f, 4.f/3.f, 640.f, 480.f);
  // After the final scanout widens, equal original pixel dimensions still
  // have equal physical dimensions; an existing 16:9 camera stays unchanged.
  CHECK(std::abs(original / target * target - 4.f/3.f) < 1e-6f);
  CHECK(std::abs(ProjectionAspect(1.f, target, 640.f, 480.f) / target - 1.f) < 1e-6f);
  CHECK(std::abs(ProjectionAspect(1.f, 16.f/10.f, 640.f, 480.f) - 16.f/10.f) < 1e-6f);
  CHECK(std::abs(ProjectionAspect(1.f, 21.f/9.f, 640.f, 480.f) - 21.f/9.f) < 1e-6f);
  CHECK(std::abs(PerspectiveXScale(1.f, 4.f/3.f, 640.f, 480.f, original) - 1.f) < 1e-6f);
  CHECK(std::abs(PerspectiveXScale(1.f, 4.f/3.f, 640.f, 480.f, target) - .75f) < 1e-6f);
  CHECK(std::abs(PerspectiveXScale(1.f, target, 640.f, 480.f, target) - 1.f) < 1e-6f);
  CHECK(std::abs(PerspectiveXScale(1.f, 4.f/3.f, 640.f, 480.f, 16.f/10.f) - 5.f/6.f) < 1e-6f);
  CHECK(std::abs(PerspectiveXScale(1.f, 4.f/3.f, 640.f, 480.f, 21.f/9.f) - 4.f/7.f) < 1e-6f);
  CHECK(std::abs(UiXScale(target) - .75f) < 1e-6f);
  CHECK(PerspectiveXScale(1.f, target, 640.f, 480.f, 0.f) == 1.f);
  CHECK(UiXScale(std::numeric_limits<float>::quiet_NaN()) == 1.f);
  // Luigi's Game Boy screen clip must stay registered with its orthographic
  // frame. The retail scissor 477..587 becomes 437.75..520.25 at 16:9.
  CHECK(std::abs(UiClipX(477.f, 0.f, 640.f, .75f) - 437.75f) < 1e-6f);
  CHECK(std::abs(UiClipX(587.f, 0.f, 640.f, .75f) - 520.25f) < 1e-6f);
  CHECK(UiClipX(477.f, 0.f, 640.f, 1.f) == 477.f);
  CHECK(UiClipX(40.f, 20.f, 80.f, .75f) == 45.f);
  CHECK(UiClipX(40.f, 20.f, 0.f, .75f) == 40.f);
  CHECK(std::abs(ProjectionAspect(1.f, 8.f/3.f, 640.f, 240.f) - original) < 1e-6f);
  CHECK(ProjectionAspect(0.f, 1.f, 640.f, 480.f) == original);
  CHECK(ProjectionAspect(std::numeric_limits<float>::quiet_NaN(), 1.f, 640.f, 480.f) == original);
  // The source rectangle, not the downsampled texture size, identifies a
  // screen copy. Small glyph/shadow copies must retain UI aspect correction.
  CHECK(IsScreenCopy(640, 480, 640.f, 480.f));
  CHECK(!IsScreenCopy(128, 128, 640.f, 480.f));
  CHECK(!IsScreenCopy(640, 32, 640.f, 480.f));
  CHECK(!IsScreenCopy(640, 480, 0.f, 480.f));

  TextureViewportRoles roles;
  CHECK(!roles.IsProducer(0,0,512,512,0));
  roles.Observe(0,0,512,512,2,2,508,508,640,480,1);
  CHECK(roles.IsProducer(0,0,512,512,1));
  CHECK(roles.IsProducer(0,0,512,512,3));
  CHECK(!roles.IsProducer(0,0,512,512,4));
  CHECK(!roles.IsProducer(0,0,640,480,2));
  roles.Observe(0,0,640,480,0,0,32,32,640,480,2);
  CHECK(!roles.IsProducer(0,0,640,480,2));
  roles.Observe(0,0,32,32,0,0,32,32,640,480,2);
  CHECK(roles.IsProducer(0,0,32,32,2));
  roles.Observe(0,0,480,480,0,0,480,480,480,480,2);
  CHECK(!roles.IsProducer(0,0,480,480,2));
  roles.Reset();
  CHECK(!roles.IsProducer(0,0,32,32,2));

  const std::array<std::array<float,2>,4> canvas{{{-1,-1},{1,-1},{1,1},{-1,1}}};
  CHECK(IsScreenFillQuad(canvas,false));
  auto strip=canvas;
  std::swap(strip[2],strip[3]);
  CHECK(IsScreenFillQuad(strip,true));
  CHECK(!IsScreenFillQuad(strip,false)); // Bow-tie quads do not cover the screen.
  auto hud=canvas;
  for (auto& corner:hud) for (auto& axis:corner) axis*=.5f;
  CHECK(!IsScreenFillQuad(hud,false)); // Preserve normal HUD proportions.
  auto diamond=canvas;
  diamond={{{0,-1},{1,0},{0,1},{-1,0}}};
  CHECK(!IsScreenFillQuad(diamond,false)); // Full bounds do not prove full coverage.
  auto duplicate=canvas;
  duplicate[3]=duplicate[2];
  CHECK(!IsScreenFillQuad(duplicate,false));
  const std::array<std::array<float,2>,6> triangles{{{-1,-1},{1,-1},{1,1},
                                                   {-1,-1},{1,1},{-1,1}}};
  CHECK(IsScreenFillTriangles(triangles));
  auto overlap=triangles;
  overlap[4]=overlap[1];
  CHECK(!IsScreenFillTriangles(overlap));

  // CPU-frustum assist discovery is deliberately conservative: only one
  // adjacent camera-like FOV/4:3 pair is accepted by the runtime.
  std::array<std::uint8_t, 64> guest{};
  WriteBigEndianFloat(guest.data() + 16, 55.0f);
  WriteBigEndianFloat(guest.data() + 20, 4.0f / 3.0f);
  auto candidates = FindProjectionCorrelatedCandidates(guest, 0x80000000u, 0u, guest.size(), 55.0f, 4.0f / 3.0f);
  CHECK(candidates.size() == 1u);
  CHECK(candidates[0].fov_address == 0x80000010u);
  CHECK(candidates[0].aspect_address == 0x80000014u);
  WriteBigEndianFloat(guest.data() + 20, 16.0f / 9.0f);
  candidates = FindProjectionCorrelatedCandidates(guest, 0x80000000u, 0u, guest.size(), 55.0f, 4.0f / 3.0f);
  CHECK(candidates.empty());
  CHECK(LooksLikeNativeAspect(4.0f / 3.0f));
  CHECK(!LooksLikeNativeAspect(16.0f / 9.0f));
  CHECK(LooksLikeCameraFov(90.0f));
  CHECK(!LooksLikeCameraFov(180.0f));
  CHECK(MatchesObservedFov(55.0f, 55.0f));
  CHECK(!MatchesObservedFov(65.0f, 55.0f));
  std::cout << "Widescreen projection and copy provenance passed\n";
}
