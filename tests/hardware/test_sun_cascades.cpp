// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/sun_cascades.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"%d: %s\n",__LINE__,#x);std::exit(1);}}while(false)
using namespace GekkoAOT::GX;
namespace {
// Reversed-depth perspective projection (row-major, clip = M * p), as captured.
SceneCapture Camera(float near_distance, float far_distance, float aspect = 16.f / 9.f) {
  const float f = 1.f / std::tan(.5f), a = far_distance - near_distance;
  SceneCapture camera;
  camera.Observe({f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, near_distance / a, near_distance * far_distance / a, 0, 0, -1, 0},
                 {0, 0, 1280, 720}, {0, 1});
  return camera;
}
EnvVec3 Rotate(const EnvVec3& v, float yaw) {
  return {std::cos(yaw) * v[0] + std::sin(yaw) * v[2], v[1], -std::sin(yaw) * v[0] + std::cos(yaw) * v[2]};
}
}
int main() {
  const auto camera = Camera(1, 1000);
  float n, f;
  CHECK(CameraDepthRange(camera, n, f) && std::abs(n - 1) < 1e-3f && std::abs(f - 1000) < 1);
  const EnvVec3 sun = EnvNormalize({.3f, .8f, .2f}), up{0, 1, 0};
  SunCascades cascades;
  CHECK(FitSunCascades(camera, sun, up, 2, 2048, cascades, 300.f));
  CHECK(cascades.count == 2 && cascades.params[0][2] < cascades.params[1][2] && std::abs(cascades.params[1][2] - 300) < 1e-2f);
  // Every point of each frustum slice lies inside its cascade window and depth.
  float split_near = 1;
  for (unsigned c = 0; c < 2; ++c) {
    const float split_far = cascades.params[c][2];
    for (float d : {split_near, split_far})
      for (float sx : {-1.f, 1.f})
        for (float sy : {-1.f, 1.f}) {
          const float half_y = std::tan(.5f) * d, half_x = half_y * 16.f / 9.f;
          const auto clip = ApplyRowMajor(cascades.clip[c], {sx * half_x, sy * half_y, -d});
          CHECK(std::abs(clip[0]) <= 1.001f && std::abs(clip[1]) <= 1.001f && clip[2] > 0 && clip[2] < 1 && clip[3] == 1);
        }
    split_near = split_far;
  }
  // Reversed depth: moving towards the sun increases depth.
  const auto a = ApplyRowMajor(cascades.clip[0], {0, 0, -5}), b = ApplyRowMajor(cascades.clip[0], {sun[0], sun[1], sun[2] - 5});
  CHECK(b[2] > a[2]);
  // Turning the camera (sun and vertical turn in view space, the world point
  // at the camera stays) keeps that point on the same sub-texel position.
  const EnvVec3 world{.37f, -.21f, -3.3f};
  float fraction[2]{};
  for (unsigned turn = 0; turn < 2; ++turn) {
    const float yaw = turn ? .37f : 0.f;
    SunCascades turned;
    CHECK(FitSunCascades(camera, Rotate(sun, yaw), Rotate(up, yaw), 2, 2048, turned, 300.f));
    const auto clip = ApplyRowMajor(turned.clip[0], Rotate(world, yaw));
    const float texels = (clip[0] * .5f + .5f) * 2048.f;
    fraction[turn] = texels - std::floor(texels);
  }
  CHECK(std::abs(fraction[0] - fraction[1]) < 2e-2f);
  // Moving the camera (the world origin moves in view space) keeps a world
  // point on the same sub-texel position when the grid is anchored to the
  // origin; a camera-anchored grid swims.
  float moved[3]{}, swum[3]{};
  for (unsigned step = 0; step < 3; ++step) {
    const EnvVec3 origin{-7.3f - .113f * step, 1.9f, -40.f + .271f * step}, point{origin[0] + 3.1f, origin[1], origin[2] - 9.7f};
    SunCascades anchored, free;
    CHECK(FitSunCascades(camera, sun, up, 2, 2048, anchored, 300.f, &origin) && FitSunCascades(camera, sun, up, 2, 2048, free, 300.f));
    for (int pass = 0; pass < 2; ++pass) {
      const auto clip = ApplyRowMajor(pass ? free.clip[1] : anchored.clip[1], point);
      const float texels = (clip[0] * .5f + .5f) * 2048.f;
      (pass ? swum : moved)[step] = texels - std::floor(texels);
    }
  }
  CHECK(std::abs(moved[0] - moved[1]) < 2e-2f && std::abs(moved[0] - moved[2]) < 2e-2f);
  CHECK(std::abs(swum[0] - swum[1]) > 2e-2f || std::abs(swum[0] - swum[2]) > 2e-2f);
  // A zenith sun must keep the same world grid through arbitrary yaw. View
  // X as the fallback up axis used to rotate the atlas with the camera.
  float zenithFraction[2]{};
  for(unsigned i=0;i<2;++i){
    const float yaw=i?.57f:0;const auto horizontal=Rotate({1,0,0},yaw);
    SunCascades zenith;CHECK(FitSunCascades(camera,up,up,2,2048,zenith,300.f,nullptr,&horizontal));
    const auto clip=ApplyRowMajor(zenith.clip[0],Rotate(world,yaw));
    const float texels=(clip[0]*.5f+.5f)*2048;zenithFraction[i]=texels-std::floor(texels);
  }
  CHECK(std::abs(zenithFraction[0]-zenithFraction[1])<.02f);
  // A zenith sun still has a valid basis; invalid input never fits.
  CHECK(FitSunCascades(camera, up, up, 1, 1024, cascades));
  CHECK(!FitSunCascades(camera, {0, 0, 0}, up, 2, 2048, cascades));
  CHECK(!FitSunCascades(SceneCapture{}, sun, up, 2, 2048, cascades));
  std::puts("Sun cascades: depth range, practical split, slice containment, reversed depth, rotation- and motion-stable texel grid passed");
}
