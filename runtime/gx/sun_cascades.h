// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef GEKKOAOT_SUN_CASCADES_H
#define GEKKOAOT_SUN_CASCADES_H
// Cascaded sun shadow maps for replayed GX geometry. Every matrix maps the
// VIEW space of the scene camera (GX model-view output) to the light clip
// space of one cascade, in the replay convention: row-major, clip = M * p,
// reversed depth (1 = nearest to the sun).
//
// The light basis is built from the world-stable sun and world vertical, so
// it is fixed in the world while the camera turns. Each cascade bounds its
// frustum slice with a sphere (constant size) and snaps the window to whole
// shadow-map texels of a grid fixed to a world point (`anchor`, view space):
// neither camera rotation nor camera movement makes shadow edges crawl.
#if __has_include("gekkoaot_environment_lighting.h")
#include "gekkoaot_environment_lighting.h"
#include "gekkoaot_scene_capture.h"
#else
#include "environment_lighting.h"
#include "scene_capture.h"
#endif

namespace GekkoAOT::GX {
inline constexpr unsigned MaxSunCascades = 2;

struct SunCascades {
  unsigned count = 0, resolution = 0;
  std::array<std::array<float, 16>, MaxSunCascades> clip{};
  // View units per texel, depth bias, far split distance, blend start distance.
  std::array<std::array<float, 4>, MaxSunCascades> params{};
};

// View-space distances of the camera's near and far planes.
inline bool CameraDepthRange(const SceneCapture& camera, float& near_distance, float& far_distance) noexcept {
  if (camera.validity != SceneValidity::Valid) return false;
  float d[2];
  for (unsigned i = 0; i < 2; ++i) {
    float h[4]{};
    for (unsigned r = 0; r < 4; ++r) h[r] = camera.inverse[4 * r + 2] * float(i) + camera.inverse[4 * r + 3];
    if (!(std::abs(h[3]) > 1e-20f)) return false;
    d[i] = -h[2] / h[3];
  }
  near_distance = std::min(d[0], d[1]); far_distance = std::max(d[0], d[1]);
  return near_distance > 0 && std::isfinite(far_distance) && far_distance > near_distance;
}

// Practical split (log/uniform blend) over the shadowed range, which stops at
// `range_ratio` times the near distance: GX far planes often lie far beyond
// the visible scene (SMB: 0.1..20000).
inline bool FitSunCascades(const SceneCapture& camera, const EnvVec3& sun, const EnvVec3& up, unsigned count,
                           unsigned resolution, SunCascades& out, float range_ratio = 3000.f,
                           const EnvVec3* anchor = nullptr, const EnvVec3* horizontal = nullptr) noexcept {
  out = {};
  float n, f;
  if (count == 0 || count > MaxSunCascades || resolution < 16 || !CameraDepthRange(camera, n, f)) return false;
  const EnvVec3 z = EnvNormalize(sun);
  if (EnvDot(z, z) < .5f) return false;
  // At the zenith, world up is parallel to the light. Use a tracked WORLD
  // horizontal rather than view X, otherwise turning the camera rotates the
  // shadow grid even though the sun and geometry have not moved.
  EnvVec3 hint = EnvNormalize(up);
  if (EnvDot(hint, hint) < .5f || std::abs(EnvDot(hint, z)) > .99f){
    hint=horizontal?EnvNormalize(*horizontal):EnvVec3{1,0,0};
    if(EnvDot(hint,hint)<.5f || std::abs(EnvDot(hint,z))>.99f)
      hint=std::abs(z[0])<.9f?EnvVec3{1,0,0}:EnvVec3{0,0,1};
  }
  const EnvVec3 x = EnvNormalize(EnvCross(hint, z)), y = EnvCross(z, x);
  const float ax = anchor ? EnvDot(*anchor, x) : 0.f, ay = anchor ? EnvDot(*anchor, y) : 0.f;
  // Corner rays of the camera frustum (view space, on the near plane).
  std::array<EnvVec3, 4> rays{};
  for (unsigned i = 0; i < 4; ++i) {
    const float ndc[4]{i & 1 ? 1.f : -1.f, i & 2 ? 1.f : -1.f, 0, 1};
    float h[4]{};
    for (unsigned r = 0; r < 4; ++r)
      for (unsigned c = 0; c < 4; ++c) h[r] += camera.inverse[4 * r + c] * ndc[c];
    if (!(std::abs(h[3]) > 1e-20f)) return false;
    const EnvVec3 p{h[0] / h[3], h[1] / h[3], h[2] / h[3]};
    if (!(p[2] < 0)) return false;
    rays[i] = {p[0] / -p[2], p[1] / -p[2], -1.f}; // point at view distance 1
  }
  const float far_limit = std::min(f, n * range_ratio);
  float split_near = n;
  for (unsigned c = 0; c < count; ++c) {
    const float t = float(c + 1) / float(count);
    const float split_far = .75f * n * std::pow(far_limit / n, t) + .25f * (n + (far_limit - n) * t);
    EnvVec3 center{};
    std::array<EnvVec3, 8> corners{};
    for (unsigned i = 0; i < 8; ++i) {
      // The next cascade also covers the preceding transition band.
      const float d = i < 4 ? (c ? split_near * .9f : split_near) : split_far;
      corners[i] = {rays[i & 3][0] * d, rays[i & 3][1] * d, rays[i & 3][2] * d};
      for (unsigned k = 0; k < 3; ++k) center[k] += corners[i][k] / 8.f;
    }
    float radius = 0;
    for (const auto& p : corners) {
      const EnvVec3 v{p[0] - center[0], p[1] - center[1], p[2] - center[2]};
      radius = std::max(radius, std::sqrt(EnvDot(v, v)));
    }
    if (!(radius > 0) || !std::isfinite(radius)) return false;
    const float texel = 2.f * radius / float(resolution);
    const float cx = ax + std::floor((EnvDot(center, x) - ax) / texel) * texel;
    const float cy = ay + std::floor((EnvDot(center, y) - ay) / texel) * texel;
    // Casters up to four radii towards the sun still shade the slice.
    const float cz = EnvDot(center, z), depth_near = cz - radius, depth_far = cz + 5.f * radius;
    const float depth_scale = 1.f / (depth_far - depth_near);
    auto& m = out.clip[c];
    m = {x[0] / radius, x[1] / radius, x[2] / radius, -cx / radius,
         y[0] / radius, y[1] / radius, y[2] / radius, -cy / radius,
         z[0] * depth_scale, z[1] * depth_scale, z[2] * depth_scale, -depth_near * depth_scale,
         0, 0, 0, 1};
    out.params[c] = {texel, .5f * texel * depth_scale, split_far, split_far * .9f};
    split_near = split_far;
  }
  out.count = count; out.resolution = resolution;
  return true;
}

inline std::array<float, 4> ApplyRowMajor(const std::array<float, 16>& m, const EnvVec3& p) noexcept {
  std::array<float, 4> r{};
  for (unsigned i = 0; i < 4; ++i) r[i] = m[4 * i] * p[0] + m[4 * i + 1] * p[1] + m[4 * i + 2] * p[2] + m[4 * i + 3];
  return r;
}
} // namespace GekkoAOT::GX
#endif
