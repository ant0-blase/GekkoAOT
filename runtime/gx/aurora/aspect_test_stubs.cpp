#include <cstdint>

#include <webgpu/webgpu_cpp.h>

extern "C" float gekkoaot_native_gx_widescreen_x_scale() noexcept { return 1.0f; }
extern "C" float gekkoaot_native_gx_widescreen_target_aspect() noexcept { return 4.0f / 3.0f; }
extern "C" float gekkoaot_native_gx_widescreen_projection_scale(float, float, float, float) noexcept { return 1.f; }
extern "C" bool gekkoaot_native_gx_screen_copy(int, int, float, float) noexcept { return false; }
extern "C" bool gekkoaot_native_gx_texture_viewport(float, float, float, float) noexcept { return false; }
extern "C" bool gekkoaot_native_gx_screen_fill() noexcept { return false; }
extern "C" float gekkoaot_native_gx_ui_clip_x(float x, float, float, float) noexcept { return x; }
extern "C" void gekkoaot_native_gx_widescreen_diag(int, float, float, float, float,
    int, int, int, int, bool, bool, float) noexcept {}
extern "C" bool gekkoaot_native_gx_ui_inset(float, float) noexcept { return false; }
extern "C" bool gekkoaot_native_gx_ui_full_view(float, float) noexcept { return true; }
extern "C" float gekkoaot_native_gx_ui_canvas_x(float x) noexcept { return x; }
extern "C" bool gekkoaot_native_gx_replay_capture() noexcept { return false; }
extern "C" void gekkoaot_native_gx_replay_uniform(std::uint32_t, const std::uint8_t*, std::uint32_t,
                                                  std::uint32_t) noexcept {}
void gekkoaot_native_gx_draw_overlay(const wgpu::CommandEncoder&, const wgpu::TextureView&, std::uint32_t,
                                     std::uint32_t) noexcept {}
extern "C" bool gekkoaot_native_gx_perspective_ui() noexcept { return false; }
