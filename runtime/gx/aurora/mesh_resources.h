// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../gx/gx.hpp"
#include <memory>
#include <span>
#include "mesh_refinement.h"
namespace aurora::gfx::geometry {
struct Prepared {
  std::array<GXAttrType,gx::MaxVtxAttr> descriptors{};
  gx::VtxFmt format{};
  std::array<std::vector<std::uint8_t>,2> vertices;
  std::array<unsigned,2> counts{};
  std::array<float,3> center{};
  float radius=0;
  unsigned originalTriangles=0;
  std::array<std::uint8_t,GX_VA_POS> matrixIndices{};
  std::vector<GekkoAOT::GX::Mesh::Vertex> reference;
  std::array<std::vector<GekkoAOT::GX::Mesh::PatchLayout>,2> patches;
  std::array<std::vector<GekkoAOT::GX::Mesh::Sample>,2> samples;
  std::vector<std::pair<unsigned,unsigned>> welds;
  bool animated=false,displaced=false;
};
void configure(unsigned quality,bool compare=false,bool displace=false,float depth=.015f) noexcept;
bool comparison_enabled() noexcept;
bool comparison_original() noexcept;
ClipRect comparison_scissor(ClipRect) noexcept;
class ComparisonScope {
  unsigned half_;bool canonical_,original_;ClipRect clip_;
public:
  ComparisonScope(unsigned half,bool original);
  ~ComparisonScope();
};
bool enabled() noexcept;
bool canonical_active() noexcept;
std::shared_ptr<const Prepared> request(GXPrimitive,GXVtxFmt,unsigned,std::span<const std::uint8_t>);
unsigned select_level(const Prepared&) noexcept;
void shutdown() noexcept;
// A temporary host-side canonical draw layout. The guest GX descriptors and
// VAT are restored before returning to the command stream, including recursion.
class FormatScope {
  std::array<GXAttrType,gx::MaxVtxAttr> descriptors_;
  gx::VtxFmt format_;
public:
  explicit FormatScope(const Prepared&);
  ~FormatScope();
};
}
