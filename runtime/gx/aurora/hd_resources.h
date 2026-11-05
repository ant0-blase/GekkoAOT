// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gekkoaot_graphics_settings.h"
#include "texture_replacement.hpp"
#include <string_view>
#include <array>
#include "mesh_displacement.h"
namespace aurora::gfx::hd {
// Streaming-worker scheduling hint; never called from the draw thread.
bool generated_needs_compute(void*) noexcept;
void configure(const GekkoAOT::GX::GraphicsSettings&);
bool enabled() noexcept;
std::uint64_t pending_bytes() noexcept;
unsigned reconstruction_scale() noexcept;
bool needs_observation(const GXTexObj_&) noexcept;
bool material_enabled() noexcept;
struct MaterialBinding {TextureHandle texture;std::array<float,4> parameters{},detail{};};
MaterialBinding request_material(const GXTexObj_&,const texture::TextureSourceKey&,const GXTlutObj_* = nullptr);
void bind_material(MaterialBinding);
const MaterialBinding& bound_material() noexcept;
std::uint64_t replacement_budget() noexcept;
std::optional<texture_replacement::ReplacementResult> request(
    const GXTexObj_&,const texture::TextureSourceKey&,const GXTlutObj_* = nullptr);
std::shared_ptr<const GekkoAOT::GX::Mesh::HeightField> request_height(const GXTexObj_&,const texture::TextureSourceKey&,const GXTlutObj_* = nullptr);
void reset() noexcept;
void report() noexcept;
}
namespace aurora::texture {
// Internal registration uses the existing streaming queue; publish invalidates
// only dependent texture objects instead of flushing all texture caches.
ReplacementRegistration register_generated_replacement(std::string_view,VirtualFileSource,ReplacementOptions);
}

namespace aurora::gx {std::shared_ptr<const GekkoAOT::GX::Mesh::HeightField> displacement_height_for_draw();}
