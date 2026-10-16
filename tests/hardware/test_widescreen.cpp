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
  // v343: letterbox bars/fades reach both clip edges; HUD panels and
  // partial-width geometry stay centred at the original aspect.
  using Span = std::array<float,2>;
  const std::array<Span,8> bars{{{-1.f,1.f},{1.f,1.f},{1.f,.7f},{-1.f,.7f},
                                 {-1.f,-.7f},{1.f,-.7f},{1.f,-1.f},{-1.f,-1.f}}};
  CHECK(IsEdgeAnchoredSpan(bars));
  const std::array<Span,3> beyond{{{-1.f,-1.133f},{1.f,-1.133f},{1.f,-.829f}}};
  CHECK(IsEdgeAnchoredSpan(beyond));
  const std::array<Span,4> panel{{{-.6f,.2f},{.6f,.2f},{.6f,-.2f},{-.6f,-.2f}}};
  CHECK(!IsEdgeAnchoredSpan(panel));
  const std::array<Span,4> left_only{{{-1.f,1.f},{-1.f,.5f},{-1.f,-.5f},{-1.f,-1.f}}};
  CHECK(!IsEdgeAnchoredSpan(left_only));
  const std::array<Span,3> off_screen{{{-1.f,1.1f},{1.f,1.1f},{1.f,1.4f}}};
  CHECK(!IsEdgeAnchoredSpan(off_screen));
  // v353: whole-screen composite (Shadow's bloom) vs a letterbox bar.
  const std::array<Span,4> whole{{{-.998f,.998f},{1.002f,.998f},{-.998f,-1.002f},{1.002f,-1.002f}}};
  CHECK(IsFullScreenSpan(whole));
  CHECK(!IsFullScreenSpan(beyond));
  CHECK(!IsFullScreenSpan(panel));
  // Only single-colour textures may be stretched with such a span.
  const std::array<std::uint8_t,8> black16{{0,0,0,0,0,0,0,0}};
  const std::array<std::uint8_t,8> pattern16{{0x80,0x00,0x80,0x00,0x80,0x00,0x80,0x00}};
  const std::array<std::uint8_t,8> art16{{0x80,0x00,0x81,0x00,0x80,0x00,0x80,0x00}};
  CHECK(IsUniformTexture(black16.data(), black16.size(), 5u));
  CHECK(IsUniformTexture(pattern16.data(), pattern16.size(), 5u));
  CHECK(!IsUniformTexture(art16.data(), art16.size(), 5u));
  CHECK(!IsUniformTexture(pattern16.data(), pattern16.size(), 1u));  // I8 must repeat bytewise
  CHECK(!IsUniformTexture(nullptr, 64u, 1u));
  // v353: CMPR (format 14) compares decoded texels. Metroid Prime's letterbox
  // texture is white with equal endpoints and varying index bits.
  const std::array<std::uint8_t,16> cmpr_white{{0xff,0xff,0xff,0xff,0x00,0x55,0xaa,0xff,
                                                0xff,0xff,0xff,0xff,0x12,0x34,0x56,0x78}};
  CHECK(!IsUniformTexture(cmpr_white.data(), cmpr_white.size(), 14u));  // index 3 is transparent
  const std::array<std::uint8_t,16> cmpr_flat{{0xff,0xff,0xff,0xff,0x00,0x55,0xaa,0x2a,
                                               0xff,0xff,0xff,0xff,0x15,0x26,0x19,0x22}};
  CHECK(IsUniformTexture(cmpr_flat.data(), cmpr_flat.size(), 14u));    // indices 0..2 only
  const std::array<std::uint8_t,8> cmpr_ramp{{0xf8,0x00,0x00,0x1f,0x00,0x55,0xaa,0xff}};
  CHECK(!IsUniformTexture(cmpr_ramp.data(), cmpr_ramp.size(), 14u));
  CHECK(IsOpaqueTexture(cmpr_ramp.data(), cmpr_ramp.size(), 14u));      // c0 > c1: 4-colour mode
  CHECK(!IsOpaqueTexture(cmpr_white.data(), cmpr_white.size(), 14u));   // index 3 transparent
  CHECK(IsOpaqueTexture(cmpr_flat.data(), cmpr_flat.size(), 14u));
  CHECK(IsOpaqueTexture(nullptr, 0u, 1u));                              // I8 has no alpha
  CHECK(!IsOpaqueTexture(pattern16.data(), pattern16.size(), 6u));      // RGBA8 not proven
  // v348: Luigi's Mansion draws its live 3D portrait into 477,370 110x84
  // inside the Game Boy Horror frame; split-screen halves touch an edge.
  CHECK(IsInsetSpan(477.f, 110.f, 640.f));
  CHECK(!IsInsetSpan(0.f, 148.f, 640.f));     // texture producer at the EFB origin
  CHECK(!IsInsetSpan(320.f, 320.f, 640.f));   // right split-screen half
  CHECK(!IsInsetSpan(40.f, 560.f, 640.f));    // narrowed main view
  CHECK(!IsInsetSpan(477.f, 110.f, 0.f));
  // v354: Lemony's pyramid: full copy -> 320x240 pass -> copy -> ... stays
  // screen-derived; a 3D render copied from 0,0 148x116 does not.
  CopyProvenance provenance;
  int screen = 0, half = 0, portrait = 0;
  CHECK(provenance.NoteCopy(&screen, 0.f, 0.f, 640.f, 480.f, 640.f, 480.f));
  provenance.NoteScreenDraw(0.f, 0.f, 320.f, 240.f);
  CHECK(provenance.NoteCopy(&half, 0.f, 0.f, 320.f, 240.f, 640.f, 480.f));
  CHECK(!provenance.NoteCopy(&portrait, 0.f, 0.f, 148.f, 480.f, 640.f, 480.f)); // mostly outside
  CHECK(provenance.IsScreenDerived(&screen) && provenance.IsScreenDerived(&half));
  CHECK(!provenance.IsScreenDerived(&portrait));
  provenance.BeginFrame();
  CHECK(!provenance.NoteCopy(&half, 0.f, 0.f, 148.f, 116.f, 640.f, 480.f));   // new frame, no pass
  CHECK(!provenance.IsScreenDerived(&half));
  DisplayInsets insets;
  insets.NoteDraw(477.f, 110.f, 7u);
  CHECK(!insets.IsInset(477.f, 110.f, 7u));   // unconfirmed during its first frame
  insets.NoteCopy(0.f, 640.f, 7u);            // a full-screen grab is not a producer copy
  insets.EndFrame(7u);
  CHECK(insets.IsInset(477.f, 110.f, 8u));
  CHECK(insets.IsInset(477.f, 110.f, 10u));
  CHECK(!insets.IsInset(477.f, 110.f, 11u));  // expires when no longer drawn
  CHECK(!insets.IsInset(478.f, 110.f, 8u));
  insets.NoteDraw(477.f, 110.f, 11u);
  insets.NoteCopy(479.f, 106.f, 11u);         // copied out: a render-to-texture pass
  insets.EndFrame(11u);
  CHECK(!insets.IsInset(477.f, 110.f, 12u));
  insets.NoteDraw(200.f, 128.f, 20u);
  insets.EndFrame(21u);                       // drawn in another frame: not confirmed
  CHECK(!insets.IsInset(200.f, 128.f, 22u));
  // v362: perspective cards of a 2D screen keep the UI scissor only after a
  // run of frames without depth-writing 3D geometry.
  FlatFrameHistory flat;
  for (unsigned i=0;i<FlatFrameHistory::Frames-1u;++i) { flat.EndFrame(0u); CHECK(!flat.Flat()); }
  flat.EndFrame(0u);
  CHECK(flat.Flat());
  flat.EndFrame(250u);                         // a 3D frame ends the 2D screen at once
  CHECK(!flat.Flat());
  for (unsigned i=0;i<8u;++i) { flat.EndFrame(i%2u ? 0u : 40u); CHECK(!flat.Flat()); } // 3D/2D alternation
  std::cout << "Widescreen projection and copy provenance passed\n";
}
