// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef GEKKOAOT_RENDER_GRAPH_H
#define GEKKOAOT_RENDER_GRAPH_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace GekkoAOT::GX {
// A bounded, ordered recording graph. Resources are logical versions owned by
// Aurora, not duplicate textures. Execution records commands; it does not imply
// GPU retirement. Never reorder across EFB copies, guest publication or PE.
enum class RenderResource : std::uint32_t {
  EfbColor = 1u << 0, EfbDepth = 1u << 1, DisplayCopy = 1u << 2,
  GuestArrayCache = 1u << 3, SceneNormals = 1u << 4, ViewDepth = 1u << 5,
  Occlusion = 1u << 6, FilteredOcclusion = 1u << 7, EnhancedColor = 1u << 8,
  SceneCapture = 1u << 9, OpaqueContribution = 1u << 10,
  Indirect = 1u << 11, FilteredIndirect = 1u << 12, Reflections = 1u << 13
};
constexpr std::uint32_t Resource(RenderResource resource) noexcept {
  return static_cast<std::uint32_t>(resource);
}
enum class RenderPass : std::uint8_t { DisplayCopy, PublishArrayCache, PrepareDepth,
  ScreenOcclusion, BilateralX, BilateralY, ComposeEnhanced,
  ScreenIndirect, IndirectBlurX, IndirectBlurY, ScreenReflections };
class OrderedRenderGraph {
public:
  struct Node { RenderPass pass; std::uint32_t reads, writes; };
  static constexpr std::size_t Capacity = 16;
  explicit OrderedRenderGraph(std::uint32_t imports = 0) : imports_(imports) {}
  bool Add(Node node) noexcept {
    if (sealed_ || failed_ || count_ == Capacity) { failed_ = true; return false; }
    nodes_[count_++] = node;
    return true;
  }
  bool Valid() const noexcept {
    if (failed_) return false;
    auto available = imports_;
    for (std::size_t i = 0; i < count_; ++i) {
      if (nodes_[i].reads & ~available) return false;
      available |= nodes_[i].writes;
    }
    return true;
  }
  template<class Record> bool Execute(Record&& record) {
    // Validate the entire plan before the first side effect. A failed or
    // partially recorded graph must never replay an EFB copy on retry.
    if (sealed_) return false;
    sealed_ = true;
    if (!Valid()) return false;
    for (std::size_t i = 0; i < count_; ++i) {
      if (!record(nodes_[i].pass)) { failed_ = true; return false; }
      ++recorded_;
    }
    return true;
  }
  std::size_t RecordedPasses() const noexcept { return recorded_; }
private:
  std::array<Node, Capacity> nodes_{};
  std::size_t count_ = 0, recorded_ = 0;
  std::uint32_t imports_ = 0;
  bool sealed_ = false, failed_ = false;
};
} // namespace GekkoAOT::GX

#endif // GEKKOAOT_RENDER_GRAPH_H
