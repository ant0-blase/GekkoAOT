#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>

#include <webgpu/webgpu_cpp.h>

// v359 performance HUD, drawn into the presented image by the render worker,
// so it shows in any window (embedded, standalone, fullscreen) and in captures.
namespace GekkoAOT::GX::Hud {
// Live values the runtime publishes (gekkoaot_native_gx_publish_perf order).
enum RuntimeValue : std::uint32_t { Vps, NominalVps, GameFps, Speed, MainCpu, TargetHz, RuntimeValueCount };

void Configure(bool enabled, std::uint32_t items);
void PublishRuntime(const float* values, std::uint32_t count);
// The GX thread produced a guest frame (fallback game rate for old runtimes).
void NoteGameFrame();
// Render worker: draw over `target` (already holding the presented image).
void Draw(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& target, wgpu::TextureFormat format,
          std::uint32_t width, std::uint32_t height);
} // namespace GekkoAOT::GX::Hud
