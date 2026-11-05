// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../gx/pipeline.hpp"
#include "../webgpu/gpu.hpp"
namespace aurora::gfx::detail { struct RenderPass; struct DrawCommand; }
namespace aurora::gfx::shadow {
// Explicit view-to-light clip mapping. No camera/world transform is inferred.
// This validation API previews a real depth replay, not lit guest output.
bool validation_view(const float* viewToLightClip, unsigned resolution);
bool recording_boundary();
// Sun cascades. Armed with the per-cascade resolution (0: off), every eligible
// draw retains a depth-pipeline plan and the original uniform. The sealed pass
// supplies one shared projection per cascade and selects draws projected
// by the views in `views` (bit id - 1), without copying their uniforms.
void arm_cascades(unsigned resolution) noexcept;
wgpu::BindGroupLayout projection_layout();
bool cascades_armed() noexcept;
// Caster draws of the pass per scene view (1-based id).
std::array<unsigned,16> caster_counts(const detail::RenderPass& pass) noexcept;
unsigned finalize_cascades(detail::RenderPass& pass, const std::array<float,16>* clip, unsigned count, uint32_t views, bool loadPrevious = false);
// Depth atlas (cascades side by side) of the pass being encoded, else a 1x1 clear map.
wgpu::TextureView cascade_view(const detail::RenderPass& pass);
bool copy_uniform(Range range, std::vector<uint8_t>& out);
void record_draw(gx::DrawData& draw, const gx::PipelineConfig& config);
void prepare(gx::DrawData& draw, const gx::PipelineConfig& config, detail::RenderPass& pass);
const gx::DrawData* gx_payload(const detail::DrawCommand& command);
void encode(const wgpu::CommandEncoder& encoder, detail::RenderPass& pass);
// Frame-unlock replays re-encode a frame whose caster uniforms are not
// replayed: the cascade atlas the real frame rendered is still exact.
void set_frame_replay(bool replaying) noexcept;
bool frame_replay() noexcept;
const webgpu::TextureWithSampler& preview(const webgpu::TextureWithSampler& original);
void reset();
}
