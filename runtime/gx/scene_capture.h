// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef GEKKOAOT_SCENE_CAPTURE_H
#define GEKKOAOT_SCENE_CAPTURE_H
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <limits>

namespace GekkoAOT::GX {
enum class SceneValidity : unsigned {
  Missing,
  Valid,
  InvalidProjection,
  MixedProjection
};
inline bool InvertProjection(const std::array<float, 16> &input,
                             std::array<float, 16> &output) noexcept {
  double a[4][8]{};
  for (unsigned r = 0; r < 4; ++r)
    for (unsigned c = 0; c < 4; ++c) {
      if (!std::isfinite(input[4 * r + c]))
        return false;
      a[r][c] = input[4 * r + c];
      a[r][c + 4] = r == c;
    }
  for (unsigned c = 0; c < 4; ++c) {
    unsigned pivot = c;
    for (unsigned r = c + 1; r < 4; ++r)
      if (std::abs(a[r][c]) > std::abs(a[pivot][c]))
        pivot = r;
    if (std::abs(a[pivot][c]) < 1e-12)
      return false;
    if (pivot != c)
      for (unsigned j = 0; j < 8; ++j)
        std::swap(a[c][j], a[pivot][j]);
    const double scale = a[c][c];
    for (auto &v : a[c])
      v /= scale;
    for (unsigned r = 0; r < 4; ++r)
      if (r != c) {
        const auto v = a[r][c];
        for (unsigned j = 0; j < 8; ++j)
          a[r][j] -= v * a[c][j];
      }
  }
  std::array<float, 16> inverse{};
  for (unsigned r = 0; r < 4; ++r)
    for (unsigned c = 0; c < 4; ++c) {
      inverse[r * 4 + c] = float(a[r][c + 4]);
      if (!std::isfinite(inverse[r * 4 + c]))
        return false;
    }
  output = inverse;
  return true;
}
// Exact effective projection from the ORIGINAL GX shader uniforms. This is a
// view-space contract: projection does not prove a world camera or static mesh.
struct SceneCapture {
  std::array<float, 16> projection{}, inverse{};
  std::array<float, 4> viewport{};
  std::array<float, 2> depth_range{};
  SceneValidity validity = SceneValidity::Missing;
  void Observe(const std::array<float, 16> &p, const std::array<float, 4> &v,
               std::array<float, 2> d) noexcept {
    if (validity == SceneValidity::InvalidProjection ||
        validity == SceneValidity::MixedProjection)
      return;
    if (validity == SceneValidity::Valid) {
      if (projection != p || viewport != v || depth_range != d)
        validity = SceneValidity::MixedProjection;
      return;
    }
    for (float f : v)
      if (!std::isfinite(f)) {
        validity = SceneValidity::InvalidProjection;
        return;
      }
    // XF viewport scale/offset arithmetic can put a full GX depth range one
    // float ULP outside [0,1]. Keep the actual rasterizer range for inversion;
    // rejecting that rounding error excludes otherwise valid whole scenes.
    constexpr float rangeSlop = 4 * std::numeric_limits<float>::epsilon();
    if (v[2] <= 0 || v[3] <= 0 || !std::isfinite(d[0]) ||
        !std::isfinite(d[1]) || d[0] < -rangeSlop || d[1] > 1 + rangeSlop || d[1] - d[0] < 1e-7f ||
        !InvertProjection(p, inverse)) {
      validity = SceneValidity::InvalidProjection;
      return;
    }
    projection = p;
    viewport = v;
    depth_range = d;
    validity = SceneValidity::Valid;
  }
};
inline constexpr unsigned MaxSceneViews=15;
// Two captured views are one camera when only their depth mapping differs:
// games draw their world in depth slices (Medal of Honor: up to 15 near/far
// pairs per frame) with the same view space, field of view and viewport.
inline bool SameCamera(const SceneCapture& a,const SceneCapture& b) noexcept {
  if(a.viewport!=b.viewport)return false;
  // Games recompute the matrix per slice and widescreen correction can scale
  // X per draw (Medal of Honor: 0.6% apart): allow 2%, far below any real
  // change of field of view (an aim zoom is 30% or more). Rays then differ by
  // at most 2% at the screen edge, negligible for occlusion and shadows.
  for(unsigned i=0;i<8;++i)
    if(std::abs(a.projection[i]-b.projection[i])>.02f*std::max(1.f,std::abs(a.projection[i])))return false;
  return true;
}
// View-space distance of a valid capture's near plane.
inline float NearDistance(const SceneCapture& c) noexcept {
  float d[2];
  for(unsigned i=0;i<2;++i){
    float h[4]{};
    for(unsigned r=0;r<4;++r)h[r]=c.inverse[4*r+2]*float(i)+c.inverse[4*r+3];
    d[i]=std::abs(h[3])>1e-20f?-h[2]/h[3]:0.f;
  }
  return std::min(d[0],d[1]);
}
inline float FarDistance(const SceneCapture& c) noexcept {
  float far=0;for(unsigned i=0;i<2;++i){float h[4]{};
    for(unsigned r=0;r<4;++r)h[r]=c.inverse[4*r+2]*float(i)+c.inverse[4*r+3];
    if(std::abs(h[3])>1e-20f)far=std::max(far,-h[2]/h[3]);
  }return far;
}
inline bool PreferWorldSlice(const SceneCapture& candidate,const SceneCapture& retained) noexcept {
  const float nextFar=FarDistance(candidate),oldFar=FarDistance(retained);
  if(nextFar<oldFar*.5f)return false;
  if(nextFar>oldFar*2.f)return true;
  return NearDistance(candidate)>NearDistance(retained);
}
struct SceneCaptureLayers {
  std::array<SceneCapture,MaxSceneViews> views{};
  unsigned count=0, rejected=0;
  // Depth slices of one camera share one ID: the capture keeps linear view
  // depth and pixel rays depend only on the X/Y projection, so slices never
  // exhaust the 15 IDs. The slice reaching the world is retained ahead of shorter background
  // slices; comparable ranges prefer the farther near plane.
  unsigned Observe(const SceneCapture& capture) noexcept {
    if(capture.validity!=SceneValidity::Valid){++rejected;return 0;}
    for(unsigned i=0;i<count;++i)
      if(SameCamera(views[i],capture)){
        if(PreferWorldSlice(capture,views[i]))views[i]=capture;
        return i+1;
      }
    if(count==MaxSceneViews){++rejected;return 0;}
    views[count]=capture;return ++count;
  }
};
// Bit (id - 1) for every retained view of the same camera as view `id`.
inline std::uint32_t CameraMask(const SceneCaptureLayers& scene,unsigned id) noexcept {
  if(id==0||id>scene.count)return 0;
  std::uint32_t mask=0;
  for(unsigned i=0;i<scene.count;++i)if(SameCamera(scene.views[i],scene.views[id-1]))mask|=1u<<i;
  return mask;
}

// A perspective viewport inside a larger camera already retained in this pass,
// covering at most a quarter of it, is an inset (minimap, picture-in-picture)
// rather than a main scene. Split-screen viewports tile the target; they never
// nest, so each keeps its own receivers.
inline bool IsInsetViewport(const SceneCaptureLayers& scene,
                            const std::array<float,4>& v) noexcept {
  if(!(v[2]>0 && v[3]>0))return false;
  constexpr float slack=.5f;
  for(unsigned i=0;i<scene.count;++i){
    const auto& o=scene.views[i].viewport;
    if(v[0]>=o[0]-slack && v[1]>=o[1]-slack && v[0]+v[2]<=o[0]+o[2]+slack &&
       v[1]+v[3]<=o[1]+o[3]+slack && v[2]*v[3]*4.f<=o[2]*o[3])return true;
  }
  return false;
}
// Keep a camera ID in the four low mantissa bits. R32Float stores these bits
// exactly; depth retains 19 fractional bits and never uses a resolved MSAA ID.
inline float PackSceneDepth(float depth,unsigned view) noexcept {
  if(!std::isfinite(depth)||depth<=0||view==0||view>MaxSceneViews)return 0;
  std::uint32_t bits;std::memcpy(&bits,&depth,4);bits=(bits&~15u)|view;
  std::memcpy(&depth,&bits,4);return depth;
}
inline bool ReconstructView(const SceneCapture &scene, float x, float y,
                            float depth,
                            std::array<float, 3> &output) noexcept {
  if (scene.validity != SceneValidity::Valid || !std::isfinite(depth))
    return false;
  const double clip[]{2. * (x - scene.viewport[0]) / scene.viewport[2] - 1.,
                      1. - 2. * (y - scene.viewport[1]) / scene.viewport[3],
                      (depth - scene.depth_range[0]) /
                          (scene.depth_range[1] - scene.depth_range[0]),
                      1.};
  double p[4]{};
  for (unsigned r = 0; r < 4; ++r)
    for (unsigned c = 0; c < 4; ++c)
      p[r] += scene.inverse[4 * r + c] * clip[c];
  if (std::abs(p[3]) < 1e-12)
    return false;
  std::array<float, 3> result{};
  for (unsigned i = 0; i < 3; ++i) {
    result[i] = float(p[i] / p[3]);
    if (!std::isfinite(result[i]))
      return false;
  }
  output = result;
  return true;
}
} // namespace GekkoAOT::GX

#endif // GEKKOAOT_SCENE_CAPTURE_H
