"""Wire optional native fragment bounds into the pinned Aurora GX renderer."""
from pathlib import Path
import sys

root = Path(sys.argv[1])


def overlay(relative, replacements):
    path = root / relative
    text = path.read_text()
    if 'GEKKOAOT_BOUNDING_BOX_V257' in text:
        return
    for before, after in replacements:
        if text.count(before) != 1:
            raise RuntimeError(f'{path}: bounding box anchor changed: {before[:90]}')
        text = text.replace(before, after)
    path.write_text('// GEKKOAOT_BOUNDING_BOX_V257\n' + text)


overlay('lib/gx/gx.hpp', [
    ('  u8 pad1 : 5 = 0;', '  u8 boundingBox : 1 = false;\n  u8 pad1 : 4 = 0;'),
    ('  bool usesZTexture : 1 = false;',
     '  bool usesBoundingBox : 1 = false;\n  bool usesZTexture : 1 = false;'),
])

overlay('lib/gx/gx.cpp', [
    ('namespace aurora::gx {',
     'extern "C" const wgpu::Buffer& gekkoaot_bbox_buffer();\n'
     'extern "C" bool gekkoaot_bbox_active();\n\nnamespace aurora::gx {'),
    ('  config.shaderConfig = {};',
     '  config.shaderConfig = {};\n  config.shaderConfig.boundingBox = gekkoaot_bbox_active();'),
    ('''    const wgpu::BindGroupLayoutDescriptor logicDesc{.label = "GX Logic Destination Layout",
        .entryCount = 1, .entries = &logicEntry};''',
     '''    const std::array logicEntries{logicEntry, wgpu::BindGroupLayoutEntry{
        .binding = 1, .visibility = wgpu::ShaderStage::Fragment,
        .buffer = wgpu::BufferBindingLayout{.type = wgpu::BufferBindingType::Storage,
                                           .minBindingSize = 16}}};
    const wgpu::BindGroupLayoutDescriptor logicDesc{.label = "GX Logic Destination + Bounding Box",
        .entryCount = logicEntries.size(), .entries = logicEntries.data()};'''),
    ('''    const wgpu::BindGroupDescriptor emptyLogicDesc{.label = "GX Empty Logic Destination",
        .layout = sLogicBindGroupLayout, .entryCount = 1, .entries = &emptyLogicEntry};''',
     '''    const std::array emptyLogicEntries{emptyLogicEntry, wgpu::BindGroupEntry{
        .binding = 1, .buffer = gekkoaot_bbox_buffer(), .size = 16}};
    const wgpu::BindGroupDescriptor emptyLogicDesc{.label = "GX Empty Logic Destination",
        .layout = sLogicBindGroupLayout, .entryCount = emptyLogicEntries.size(),
        .entries = emptyLogicEntries.data()};'''),
    ('''  const WGPUBindGroupDescriptor desc{.label = {"GX Logic Destination", WGPU_STRLEN},
      .layout = sLogicBindGroupLayout.Get(), .entryCount = 1, .entries = &entry};''',
     '''  const std::array entries{entry, WGPUBindGroupEntry{
      .binding = 1, .buffer = gekkoaot_bbox_buffer().Get(), .size = 16}};
  const WGPUBindGroupDescriptor desc{.label = {"GX Logic Destination", WGPU_STRLEN},
      .layout = sLogicBindGroupLayout.Get(), .entryCount = entries.size(), .entries = entries.data()};'''),
])

overlay('lib/gx/command_processor.cpp', [
    ('namespace aurora::gx::fifo {',
     'extern "C" bool gekkoaot_bbox_active();\n\nnamespace aurora::gx::fifo {'),
    ('  hash.update(&lineMode, sizeof(lineMode));',
     '  hash.update(&lineMode, sizeof(lineMode));\n'
     '  const bool bbox = gekkoaot_bbox_active();\n  hash.update(&bbox, sizeof(bbox));'),
])

overlay('lib/gx/shader_info.cpp', [
    ('  if (config.lineMode != 0) {',
     '  if (config.boundingBox) {\n    info.usesBoundingBox = true;\n'
     '    info.uniformSize += 48; // origin, pixel scale and exact native scissor\n  }\n\n'
     '  if (config.lineMode != 0) {'),
    ('  if (info.usesZTexture) {', '''  if (info.usesBoundingBox) {
    const auto& render = g_gxState.renderViewport;
    const auto& logical = g_gxState.logicalViewport;
    buf.append<f32>(render.left); buf.append<f32>(render.top);
    buf.append<f32>(logical.left); buf.append<f32>(logical.top);
    buf.append<f32>(logical.width / std::max(render.width, 1.f));
    buf.append<f32>(logical.height / std::max(render.height, 1.f));
    buf.append<f32>(std::max(render.width / std::max(logical.width, 1.f), 1.f));
    buf.append<f32>(std::max(render.height / std::max(logical.height, 1.f), 1.f));
    const auto& clip = g_gxState.logicalScissor;
    buf.append<f32>(static_cast<f32>(clip.x)); buf.append<f32>(static_cast<f32>(clip.y));
    buf.append<f32>(static_cast<f32>(clip.x + clip.width));
    buf.append<f32>(static_cast<f32>(clip.y + clip.height));
  }
  if (info.usesZTexture) {'''),
])

overlay('lib/gx/shader.cpp', [
    ('  const bool zTextureEnabled = config.zTexOp',
     '''  if (config.boundingBox) {
    uniBufAttrs += "\\n    bbox_origin: vec4f,\\n    bbox_scale: vec4f,\\n    bbox_scissor: vec4f,";
    texBindings += "\\n@group(3) @binding(1) var<storage, read_write> bbox: array<atomic<u32>, 4>;";
  }
  const bool zTextureEnabled = config.zTexOp'''),
    ('  auto shaderSource =\n',
     '''  // Bounding reduction follows alpha discard and uses native raster
  // pixel centers. Shader variants without bbox contain no atomic operations.
  if (config.boundingBox) {
    fragmentFn += R"(
    let bbox_logical = floor((in.pos.xy - ubuf.bbox_origin.xy) * ubuf.bbox_scale.xy
                            + ubuf.bbox_origin.zw + vec2f(0.0001));
    let bbox_sample = floor((bbox_logical + vec2f(0.5) - ubuf.bbox_origin.zw)
                           / ubuf.bbox_scale.xy + ubuf.bbox_origin.xy);
    if (all(floor(in.pos.xy) == bbox_sample)) {
      if (all(bbox_logical >= max(ubuf.bbox_scissor.xy, vec2f(0.0))) &&
          all(bbox_logical < min(ubuf.bbox_scissor.zw, vec2f(640.0, 528.0)))) {
        let bbox_pos = vec2u(bbox_logical);
        let bbox_min = bbox_pos & vec2u(0xfffffffeu);
        let bbox_max = bbox_pos | vec2u(1u);
        if (atomicLoad(&bbox[0]) > bbox_min.x) { atomicMin(&bbox[0], bbox_min.x); }
        if (atomicLoad(&bbox[1]) < bbox_max.x) { atomicMax(&bbox[1], bbox_max.x); }
        if (atomicLoad(&bbox[2]) > bbox_min.y) { atomicMin(&bbox[2], bbox_min.y); }
        if (atomicLoad(&bbox[3]) < bbox_max.y) { atomicMax(&bbox[3], bbox_max.y); }
      }
    }
)";
  }
  auto shaderSource =
'''),
])

overlay('lib/gx/pipeline.hpp', [
    ('constexpr uint32_t GXPipelineConfigVersion = 15;',
     'constexpr uint32_t GXPipelineConfigVersion = 16;'),
])
