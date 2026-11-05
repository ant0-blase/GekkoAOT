// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/render_graph.h"
#include "../../runtime/gx/scene_evidence.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
using namespace GekkoAOT::GX;
int main() {
  const auto efb = Resource(RenderResource::EfbColor) | Resource(RenderResource::EfbDepth);
  const auto xfb = Resource(RenderResource::DisplayCopy), cache = Resource(RenderResource::GuestArrayCache);
  OrderedRenderGraph graph(efb);
  CHECK(graph.Add({RenderPass::DisplayCopy,efb,xfb}));
  CHECK(graph.Add({RenderPass::PublishArrayCache,xfb,cache}));
  std::vector<RenderPass> order;
  CHECK(graph.Execute([&](auto pass) { order.push_back(pass); return true; }));
  CHECK(order == std::vector<RenderPass>({RenderPass::DisplayCopy,RenderPass::PublishArrayCache}));
  CHECK(graph.RecordedPasses() == 2);
  CHECK(!graph.Execute([&](auto) { std::abort(); return true; }));
  CHECK(!graph.Add({RenderPass::DisplayCopy,efb,xfb}));

  // Even a valid prefix cannot execute if a later node lacks its input. This
  // prevents partially applied plans after failed capability/resource checks.
  OrderedRenderGraph invalid;
  CHECK(invalid.Add({RenderPass::DisplayCopy,0,xfb}));
  CHECK(invalid.Add({RenderPass::PublishArrayCache,efb,cache}));
  CHECK(!invalid.Valid());
  CHECK(!invalid.Execute([](auto) { std::abort(); return true; }));
  OrderedRenderGraph failed(efb);
  CHECK(failed.Add({RenderPass::DisplayCopy,efb,xfb}));
  CHECK(failed.Add({RenderPass::PublishArrayCache,xfb,cache}));
  unsigned calls = 0;
  CHECK(!failed.Execute([&](auto) { ++calls; return false; }));
  CHECK(calls == 1 && failed.RecordedPasses() == 0);
  CHECK(!failed.Execute([](auto) { std::abort(); return true; }));
  OrderedRenderGraph full;
  for (unsigned i = 0; i < OrderedRenderGraph::Capacity; ++i)
    CHECK(full.Add({RenderPass::DisplayCopy,0,xfb}));
  CHECK(!full.Add({RenderPass::DisplayCopy,0,xfb}));
  CHECK(!full.Execute([](auto) { std::abort(); return true; }));

  // Perspective alone never admits a camera or a shadow pass. Existing
  // widescreen UI/fill evidence and offscreen boundaries take precedence.
  CHECK(ClassifyDraw({true}) == SceneClass::Unknown);
  CHECK(ClassifyDraw({false,true,true}) == SceneClass::Unknown);
  CHECK(ClassifyDraw({true,true,true}) == SceneClass::Opaque3DCandidate);
  CHECK(ClassifyDraw({true,true,true,true}) == SceneClass::TransparentCandidate);
  CHECK(ClassifyDraw({true,true,true,false,false,false,true}) == SceneClass::InterfaceCandidate);
  CHECK(ClassifyDraw({true,true,true,false,false,true}) == SceneClass::InterfaceCandidate);
  CHECK(ClassifyDraw({true,true,true,false,true,true,true}) == SceneClass::Intermediate);
  SceneEvidence scene;
  scene.Observe({true});scene.Observe({true,true,true});
  CHECK(scene.draws[0] == 1 && scene.draws[1] == 1);
  scene.Reset();for (auto count : scene.draws) CHECK(count == 0);
  std::puts("Render graph: dependency preflight, EFB/cache order, failure, capacity, single execution and conservative scene evidence passed");
}
