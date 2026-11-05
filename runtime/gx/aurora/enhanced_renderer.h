// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../webgpu/gpu.hpp"
#include "gekkoaot_graphics_settings.h"
#include "gekkoaot_scene_capture.h"
#include "gekkoaot_scene_resource_history.h"
#include "types.hpp"
#include <span>
namespace aurora::gfx::detail {
struct RenderPass;
}
namespace aurora::gfx::enhanced {
struct alignas(16) ViewParams {
  std::array<float,16> projection{},inverse{};
  std::array<float,4> viewport{};
};
struct alignas(16) Params {
  std::array<ViewParams,GekkoAOT::GX::MaxSceneViews> views{};
  std::array<float,4> depth{},effects{},light{},quality{},work{},copy_rect{};
  // GekkoAOT::GX::EnvironmentUniforms: sun, sun color, sky, ground, up.
  std::array<float,4> env_sun{},env_sun_color{},env_sky{},env_ground{},env_up{};
  // GekkoAOT::GX::SunCascades: view -> light clip (row-major), texel/bias/split,
  // and count, resolution, main camera id.
  std::array<std::array<float,16>,2> csm{};
  std::array<std::array<float,4>,2> csm_params{};
  std::array<float,4> csm_info{};
  // Strength (0: off), relative radius / ray length, steps, directions.
  std::array<float,4> gi{},ssr{},dof{},emission{},emission_style{};
  std::array<std::array<float,4>,64> emitters{},emitter_views{};
  std::array<std::array<float,4>,64> emitter_colors{};
};
static_assert(sizeof(Params)==5664);
unsigned current_view_id() noexcept;
// Start of one Aurora draw: the capture reason is classified once per draw.
void begin_draw() noexcept;
// CPU observers decode guest arrays, so gathered GPU indices must never be
// interpreted against those arrays. Scope the original FIFO payload to submit.
std::span<const std::uint8_t> exchange_geometry_source(std::span<const std::uint8_t>) noexcept;
struct GeometrySourceScope {
  std::span<const std::uint8_t> previous;
  explicit GeometrySourceScope(std::span<const std::uint8_t> source) noexcept
      : previous(exchange_geometry_source(source)) {}
  ~GeometrySourceScope(){exchange_geometry_source(previous);}
};
void observe_geometry(GXPrimitive,GXVtxFmt,unsigned,std::span<const std::uint8_t>) noexcept;
// GEKKOAOT_ENHANCED_VIEW=coverage: rejection code of the current draw (0 for
// an eligible receiver) and the per-draw switch that stores it in the capture.
unsigned diagnostic_reason() noexcept;
// While sun cascades shade the scene, the game's own shadows (dark
// alpha-blended overlays on 3D geometry) fade by this amount (x1000) so the
// scene is not shadowed twice. 0 for every other draw.
unsigned shadow_suppression() noexcept;
// The current draw is a view model (an FPS weapon): drawn in a depth slice
// whose near plane is far closer than its camera's world slice. Such draws do
// not cast sun shadows (they would follow the view as a dark block).
bool view_model_draw() noexcept;
unsigned diagnostic_coverage() noexcept;
void compile_startup_shaders();
void configure(const GekkoAOT::GX::GraphicsSettings &settings);
bool eligible() noexcept;
bool fragment_lighting() noexcept;
bool background_draw() noexcept;
// Replacement, alpha-over, additive/subtractive, modulation, unproved blend.
unsigned composition_mode() noexcept;
void record_classification(unsigned reason,unsigned composition) noexcept;
void record_texture_use(const GekkoAOT::GX::SceneTextureUse& use) noexcept;
void set_copy_provenance(uint64_t resource,uint32_t revision) noexcept;
void note_copy(const TextureHandle& texture,uint32_t revision);
void observe_draw() noexcept;
// Environment lighting evidence of a receiver draw: its model-view rotation
// (weighted by vertices) and the directional GX lights of its lit channels.
void observe_environment(unsigned vertices) noexcept;
void record_draw_resources(unsigned sampled) noexcept;
void capture_projection(const float *projection) noexcept;
unsigned observe_scene(const GekkoAOT::GX::SceneCapture &capture) noexcept;
bool inset_viewport(const std::array<float,4> &viewport) noexcept;
// Copy aliasing: a color texture copy of a pass with scene receivers gets an
// enhanced twin. Orthographic draws sampling exactly that copy revision (the
// scene composited back to the screen) bind the twin; all other uses keep the
// original copy.
TextureHandle copy_texture(const TextureHandle &copy, uint32_t revision) noexcept;
bool blended_scene_copy() noexcept;
// The current draw leaves the receivers under it (composition mode 5).
bool keeps_receivers() noexcept;
bool keeps_receivers_changed() noexcept;
bool keeps_receivers_under_clear(const detail::RenderPass &pass, int32_t width, int32_t height) noexcept;
// True when the current draw's alias eligibility differs from the bound one.
bool alias_bindings_stale() noexcept;
// A CPU EFB color read observes the finished frame, which may composite an
// enhanced twin. The first one permanently disables copy aliasing (any thread).
void note_color_peek() noexcept;
// A pass cleared the capture attachments (receivers of earlier passes are gone).
void note_capture_cleared() noexcept;
void seal(detail::RenderPass &pass);
void encode(const wgpu::CommandEncoder &encoder,
            const detail::RenderPass &pass);
const webgpu::TextureWithSampler &
present_source(const webgpu::TextureWithSampler &original);
void reset();
void report();
uint64_t output_graph(const webgpu::TextureWithSampler&) noexcept;
void record_presentation(uint64_t graph, bool success) noexcept;
} // namespace aurora::gfx::enhanced
