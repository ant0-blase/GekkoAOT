// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "gekkoaot_color_snapshot_layout.h"
#include <webgpu/webgpu_cpp.h>
#include <array>

namespace GekkoAOT::GX::ColorPeek {
// Compatibility-level WebGPU has no compute shaders. Read GX EFB color with a
// real GPU raster pass into R32Uint, preserving the exact packed ARGB bits used
// by Aurora's compute implementation. No CPU resampling or synchronous wait.
inline constexpr char Shader[] = R"(
struct Params { dstSize: vec2u, srcSize: vec2u, offset: vec2f, scale: vec2f };
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(2) var<uniform> params: Params;
@vertex fn vs_main(@builtin(vertex_index) id: u32) -> @builtin(position) vec4f {
    var xy = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
    return vec4f(xy[id], 0.0, 1.0);
}
@fragment fn fs_main(@builtin(position) position: vec4f) -> @location(0) u32 {
    let pixel = clamp(vec2i(floor(params.offset + position.xy * params.scale)),
                      vec2i(0), vec2i(params.srcSize) - vec2i(1));
    let c = vec4u(clamp(textureLoad(src, pixel, 0), vec4f(0.0), vec4f(1.0)) * 255.0 + vec4f(0.5));
    return ((c.w & 255u) << 24u) | ((c.x & 255u) << 16u) | ((c.y & 255u) << 8u) | (c.z & 255u);
}
)";

inline wgpu::BindGroupLayout CreateLayout(const wgpu::Device& device) {
  const std::array entries{
      wgpu::BindGroupLayoutEntry{.binding = 0, .visibility = wgpu::ShaderStage::Fragment,
          .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e2D}},
      wgpu::BindGroupLayoutEntry{.binding = 2, .visibility = wgpu::ShaderStage::Fragment,
          .buffer = {.type = wgpu::BufferBindingType::Uniform}}};
  const wgpu::BindGroupLayoutDescriptor descriptor{
      .label = "Color Peek Raster Layout", .entryCount = entries.size(), .entries = entries.data()};
  return device.CreateBindGroupLayout(&descriptor);
}

inline wgpu::RenderPipeline CreatePipeline(const wgpu::Device& device, const wgpu::BindGroupLayout& layout) {
  const wgpu::ShaderSourceWGSL source{wgpu::ShaderSourceWGSL::Init{.code = Shader}};
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &source, .label = "Color Peek Raster Shader"};
  const auto module = device.CreateShaderModule(&moduleDescriptor);
  const wgpu::PipelineLayoutDescriptor layoutDescriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &layout};
  const auto pipelineLayout = device.CreatePipelineLayout(&layoutDescriptor);
  const wgpu::ColorTargetState target{.format = wgpu::TextureFormat::R32Uint};
  const wgpu::FragmentState fragment{.module = module, .entryPoint = "fs_main", .targetCount = 1, .targets = &target};
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = "Color Peek Raster Pipeline", .layout = pipelineLayout,
      .vertex = {.module = module, .entryPoint = "vs_main"},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList}, .fragment = &fragment};
  return device.CreateRenderPipeline(&descriptor);
}

struct Snapshot {
  wgpu::Texture texture;
  wgpu::TextureView view;
  ColorSnapshotLayout layout{};
  bool Create(const wgpu::Device& device, std::uint32_t width, std::uint32_t height) {
    const auto extent = ColorSnapshotExtent(width, height);
    if (!extent) return false;
    layout = *extent;
    const wgpu::TextureDescriptor descriptor{
        .label = "Color Peek Raster Snapshot", .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc,
        .size = {width, height, 1}, .format = wgpu::TextureFormat::R32Uint};
    texture = device.CreateTexture(&descriptor);
    view = texture.CreateView();
    return texture && view;
  }
  void Encode(const wgpu::Device& device, const wgpu::CommandEncoder& encoder,
              const wgpu::RenderPipeline& pipeline, const wgpu::BindGroupLayout& bindLayout,
              const wgpu::TextureView& source, const wgpu::Buffer& params, std::uint64_t paramsSize,
              const wgpu::Buffer& readback, std::uint32_t width, std::uint32_t height) const {
    const std::array entries{
        wgpu::BindGroupEntry{.binding = 0, .textureView = source},
        wgpu::BindGroupEntry{.binding = 2, .buffer = params, .size = paramsSize}};
    const wgpu::BindGroupDescriptor descriptor{
        .label = "Color Peek Raster Bind Group", .layout = bindLayout,
        .entryCount = entries.size(), .entries = entries.data()};
    const auto bindings = device.CreateBindGroup(&descriptor);
    const wgpu::RenderPassColorAttachment attachment{
        .view = view, .loadOp = wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store};
    const wgpu::RenderPassDescriptor passDescriptor{
        .label = "Color Peek Raster Pass", .colorAttachmentCount = 1, .colorAttachments = &attachment};
    const auto pass = encoder.BeginRenderPass(&passDescriptor);
    pass.SetPipeline(pipeline); pass.SetBindGroup(0, bindings); pass.Draw(3); pass.End();
    const wgpu::TexelCopyTextureInfo from{.texture = texture};
    const wgpu::TexelCopyBufferInfo to{.layout = {.bytesPerRow = layout.row_pitch, .rowsPerImage = height}, .buffer = readback};
    const wgpu::Extent3D extent{width, height, 1};
    encoder.CopyTextureToBuffer(&from, &to, &extent);
  }
};
} // namespace GekkoAOT::GX::ColorPeek
