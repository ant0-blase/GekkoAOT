// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/render_graph.h"
#include "../../runtime/gx/scene_capture.h"
#include "../../runtime/gx/background_geometry.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      std::fprintf(stderr, "line %d: %s\n", __LINE__, #x);                     \
      std::exit(1);                                                            \
    }                                                                          \
  } while (false)
using namespace GekkoAOT::GX;
int main() {
  std::array<float, 16> p{1.2f, 0, .1f,  0,     0, 1.6f, -.05f, 0,
                          0,    0, .01f, 1.01f, 0, 0,    -1,    0};
  SceneCapture scene;
  scene.Observe(p, {20, 10, 640, 480}, {.2f, .8f});
  CHECK(scene.validity == SceneValidity::Valid);
  for (float z : {-1.f, -5.f, -10.f, -100.f})
    for (float x : {-1.f, 0.f, 1.f})
      for (float y : {-.5f, 0.f, .5f}) {
        const float point[]{x, y, z, 1};
        float clip[4]{};
        for (unsigned r = 0; r < 4; ++r)
          for (unsigned c = 0; c < 4; ++c)
            clip[r] += p[r * 4 + c] * point[c];
        const float sx = 20 + (clip[0] / clip[3] + 1) * 320,
                    sy = 10 + (1 - clip[1] / clip[3]) * 240;
        const float depth = .2f + clip[2] / clip[3] * .6f;
        std::array<float, 3> result{};
        CHECK(ReconstructView(scene, sx, sy, depth, result));
        CHECK(std::abs(result[0] - x) < .001 &&
              std::abs(result[1] - y) < .001 && std::abs(result[2] - z) < .01);
      }
  scene.Observe(p, {20, 10, 640, 480}, {.2f, .8f});
  CHECK(scene.validity == SceneValidity::Valid);
  auto other = p;
  other[0] *= 1.1f;
  scene.Observe(other, {20, 10, 640, 480}, {.2f, .8f});
  CHECK(scene.validity == SceneValidity::MixedProjection);
  scene.Observe(p, {20, 10, 640, 480}, {.2f, .8f});
  CHECK(scene.validity == SceneValidity::MixedProjection);
  SceneCapture invalid;
  invalid.Observe({}, {0, 0, 640, 480}, {0, 1});
  CHECK(invalid.validity == SceneValidity::InvalidProjection);
  // Retail XF scale/offset decoding may round the reversed full range one
  // ULP below zero. Preserve that effective range, including camera reuse.
  const float ulp = std::numeric_limits<float>::epsilon();
  SceneCapture rounded;
  rounded.Observe(p, {0, 0, 640, 480}, {-ulp, 1});
  CHECK(rounded.validity == SceneValidity::Valid && rounded.depth_range[0] == -ulp);
  rounded.Observe(p, {0, 0, 640, 480}, {-ulp, 1});
  CHECK(rounded.validity == SceneValidity::Valid);
  for (const auto range : {std::array<float,2>{0, 1 + ulp}, {-ulp, 1 + ulp}}) {
    SceneCapture accepted;
    accepted.Observe(p, {0, 0, 640, 480}, range);
    CHECK(accepted.validity == SceneValidity::Valid);
  }
  for (const auto range : {std::array<float,2>{-8 * ulp, 1}, {0, 1 + 8 * ulp},
                          {.8f, .2f}, {.2f, .2f},
                          {0, std::numeric_limits<float>::infinity()}}) {
    SceneCapture rejected;
    rejected.Observe(p, {0, 0, 640, 480}, range);
    CHECK(rejected.validity == SceneValidity::InvalidProjection);
  }
  invalid = {};
  other = p;
  other[3] = std::numeric_limits<float>::quiet_NaN();
  invalid.Observe(other, {0, 0, 640, 480}, {0, 1});
  CHECK(invalid.validity == SceneValidity::InvalidProjection);
  invalid = {};
  invalid.Observe(p, {0, 0, 0, 480}, {0, 1});
  CHECK(invalid.validity == SceneValidity::InvalidProjection);
  SceneCaptureLayers layers;
  SceneCapture world, weapon;
  world.Observe(p, {20, 10, 640, 480}, {0, 1});
  other = p;
  other[0] *= 1.5f;
  weapon.Observe(other, {20, 10, 640, 480}, {0, .1f});
  CHECK(layers.Observe(world) == 1);
  CHECK(layers.Observe(weapon) == 2);
  CHECK(layers.Observe(world) == 1 && layers.count == 2);
  CHECK(layers.Observe(invalid) == 0 && layers.count == 2);
  for (unsigned id = 1; id <= MaxSceneViews; ++id)
    for (float z : {.05f, 1.f, 4.f, 1234.567f, 100000.f}) {
      const float packed = PackSceneDepth(z, id);
      std::uint32_t bits;
      std::memcpy(&bits, &packed, 4);
      CHECK((bits & 15u) == id);
      bits &= ~15u;
      float unpacked;
      std::memcpy(&unpacked, &bits, 4);
      CHECK(std::abs(unpacked - z) <= z * 2e-6f);
    }
  CHECK(PackSceneDepth(1, 0) == 0 && PackSceneDepth(1, 16) == 0);
  CHECK(PackSceneDepth(-1, 1) == 0);
  CHECK(PackSceneDepth(std::numeric_limits<float>::infinity(), 1) == 0);
  for (unsigned id = 3; id <= MaxSceneViews; ++id) {
    other = p;
    other[0] *= float(id);
    SceneCapture next;
    next.Observe(other, {20, 10, 640, 480}, {0, 1});
    CHECK(layers.Observe(next) == id);
  }
  CHECK(layers.count == MaxSceneViews);
  other[0] *= 2;
  SceneCapture excess;
  excess.Observe(other, {20, 10, 640, 480}, {0, 1});
  CHECK(layers.Observe(excess) == 0 && layers.Observe(world) == 1);
  // A minimap/picture-in-picture viewport nested in a retained main camera is
  // not a receiver; side-by-side split-screen viewports are not nested.
  SceneCaptureLayers split;
  SceneCapture left, right;
  left.Observe(p, {0, 0, 320, 480}, {0, 1});
  right.Observe(other, {320, 0, 320, 480}, {0, 1});
  CHECK(!IsInsetViewport(split, {480, 330, 140, 130}));
  CHECK(split.Observe(left) == 1 && !IsInsetViewport(split, {320, 0, 320, 480}));
  CHECK(split.Observe(right) == 2 && IsInsetViewport(split, {480, 330, 140, 130}));
  CHECK(!IsInsetViewport(split, {200, 330, 240, 130}));        // spans both halves
  CHECK(!IsInsetViewport(split, {320, 0, 200, 400}));          // over a quarter of its camera
  CHECK(IsInsetViewport(layers, {460, 330, 160, 120}) && !IsInsetViewport(layers, {20, 10, 640, 480}));
  CHECK(!IsInsetViewport(layers, {460, 330, 0, 120}));
  OrderedRenderGraph graph(Resource(RenderResource::EfbColor) |
                           Resource(RenderResource::EfbDepth) |
                           Resource(RenderResource::SceneNormals));
  CHECK(graph.Add({RenderPass::PrepareDepth, Resource(RenderResource::EfbDepth),
                   Resource(RenderResource::ViewDepth)}));
  CHECK(graph.Add({RenderPass::ScreenOcclusion,
                   Resource(RenderResource::ViewDepth) |
                       Resource(RenderResource::SceneNormals),
                   Resource(RenderResource::Occlusion)}));
  CHECK(graph.Add({RenderPass::BilateralX, Resource(RenderResource::Occlusion),
                   Resource(RenderResource::FilteredOcclusion)}));
  CHECK(graph.Add({RenderPass::BilateralY,
                   Resource(RenderResource::FilteredOcclusion),
                   Resource(RenderResource::Occlusion)}));
  CHECK(graph.Add(
      {RenderPass::ComposeEnhanced,
       Resource(RenderResource::EfbColor) | Resource(RenderResource::Occlusion),
       Resource(RenderResource::EnhancedColor)}));
  unsigned count = 0;
  CHECK(graph.Execute([&](auto) {
    ++count;
    return true;
  }) && count == 5);
  {
    // Depth slices of one camera (Medal of Honor: near 0.014 and 0.28, same
    // field of view) share one ID; the world slice (farther near) is kept.
    const auto slice=[](float n,float f){
      SceneCapture c;const float a=f-n;
      c.Observe({1.2f,0,0,0, 0,1.6f,0,0, 0,0,n/a,n*f/a, 0,0,-1,0},{0,0,1280,720},{0,1});return c;};
    SceneCaptureLayers layers;
    CHECK(layers.Observe(slice(.014f,1000))==1 && layers.Observe(slice(.28f,1000))==1 && layers.count==1);
    CHECK(std::abs(NearDistance(layers.views[0])-.28f)<1e-3f);
    // A background depth slice has a farther near plane but a shorter
    // range. It must not redefine the world's length unit in either order.
    SceneCaptureLayers background;
    CHECK(background.Observe(slice(1,1000000))==1);
    CHECK(background.Observe(slice(100,1000))==1);
    CHECK(std::abs(NearDistance(background.views[0])-1)<1e-3f);
    background={};background.Observe(slice(100,1000));background.Observe(slice(1,1000000));
    CHECK(std::abs(NearDistance(background.views[0])-1)<1e-3f);
    SceneCapture zoomed;zoomed.Observe({2.4f,0,0,0, 0,3.2f,0,0, 0,0,.28f/999.72f,280/999.72f, 0,0,-1,0},{0,0,1280,720},{0,1});
    CHECK(layers.Observe(zoomed)==2 && CameraMask(layers,1)==1u);
  }
  {
    const std::array<std::array<float,3>,4> shell{{{-70,-70,-70},{-70,70,-70},{70,70,-70},{70,-70,-70}}};
    CHECK(CameraCentredShell(shell,{0,0,0},1,1));
    EyeCentredPrelude prelude;
    const std::array<std::array<float,3>,4> smallDome{{{-4,4,2},{4,4,2},{4,4,-6},{-4,4,-6}}};
    prelude.Begin(1);CHECK(prelude.Observe(smallDome,{0,0,0},1,.28f));
    CHECK(!prelude.Observe(shell,{2,3,-10},1,.28f));
    CHECK(!prelude.Observe(smallDome,{0,0,0},1,.28f)); // world phase already began
    prelude.Begin(2);CHECK(prelude.Observe(smallDome,{0,0,0},1,.28f));
    EyeCentredPrelude weapon;
    const std::array<std::array<float,3>,4> front{{{-1,-1,-4},{1,-1,-4},{1,1,-4},{-1,1,-4}}};
    weapon.Begin(1);CHECK(!weapon.Observe(front,{0,0,0},1,.28f));

    CHECK(!CameraCentredShell(shell,{0,2,-10},1,1)); // a translated real object
    CHECK(!CameraCentredShell(shell,{0,0,0},1,10)); // a nearby object
    auto irregular=shell;irregular[0][0]=0;
    CHECK(!CameraCentredShell(irregular,{0,0,0},1,1));
    const std::array<std::array<float,3>,4> distantFace{{{-150000,-100000,-150000},
      {150000,-100000,-150000},{150000,100000,-150000},{-150000,100000,-150000}}};
    CHECK(DistantBackgroundFace(distantFace,{1641,-102,-9006},5));
    auto distantBuilding=distantFace;
    for(auto& p:distantBuilding){p[0]*=.001f;p[1]*=.001f;}
    CHECK(!DistantBackgroundFace(distantBuilding,{1641,-102,-9006},5));
    CHECK(!DistantBackgroundFace(distantFace,{100000,0,0},5));
    CHECK(!DistantBackgroundFace(front,{0,0,0},.28f));
  }
  std::puts("Exact reversed-Z/off-axis view reconstruction, viewport range, "
            "ambiguous/singular capture rejection, inset viewports and "
            "five-pass enhancement dependencies passed");
}
