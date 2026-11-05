// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../webgpu/gpu.hpp"
namespace aurora::gfx::enhanced::measurement {
// Opt-in sampled GPU evidence. Never waits for readback or changes eligibility.
inline constexpr unsigned SamplePeriod=127;
void begin(uint64_t graph,unsigned ao,unsigned contact,unsigned width,unsigned height,unsigned msaa);
const wgpu::PassTimestampWrites* timestamps(unsigned pass);
void finish(const wgpu::CommandEncoder&, uint32_t uniformOffset,
            const wgpu::TextureView& original, const wgpu::TextureView& output,
            const wgpu::TextureView& depth, const wgpu::TextureView& normals,
            const wgpu::TextureView& occlusion, const wgpu::Buffer& particleSources);
void after_submit();
void reset();
}
