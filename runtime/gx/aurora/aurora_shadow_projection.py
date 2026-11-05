"""Share original GX draw uniforms with depth replays; project once per cascade.

The shadow pipeline's group 3 holds a shared 64-byte projection uniform.
Uniform layouts, skinning, alpha tests and texture coordinates stay identical
to the original draw. Bounding-box fields stay in the layout, without atomics.
"""
from pathlib import Path
import sys

root = Path(sys.argv[1])
marker = 'GEKKOAOT_SHADOW_PROJECTION_V2'
updates = {}

def edit(name, changes):
    path = root / name
    source = path.read_text()
    if marker in source:
        return
    for change in changes:
        old, new = change[:2]
        count = change[2] if len(change) > 2 else 1
        if source.count(old) != count:
            raise RuntimeError(f'{name}: shadow projection anchor changed: {old[:100]}')
        source = source.replace(old, new)
    updates[path] = '// ' + marker + '\n' + source

edit('lib/gx/gx.hpp', [
    ('  u32 enhancedComposition = 0;', '  u32 enhancedComposition = 0;\n  u32 shadowReplay = 0;')])
edit('lib/gx/pipeline.hpp', [
    ('constexpr uint32_t GXPipelineConfigVersion = 21;', 'constexpr uint32_t GXPipelineConfigVersion = 22;'),
    ('  uint32_t shadowIndex = 0;',
     '  uint32_t shadowIndex = 0;\n  uint32_t shadowProjectionOffset = UINT32_MAX;')])
edit('lib/gx/gx.cpp', [
    ('#include "gx.hpp"', '#include "gx.hpp"\n#include "../gfx/gekkoaot_shadow_replay.hpp"'),
    ('wgpu::PipelineLayout sPipelineLayout;', 'wgpu::PipelineLayout sPipelineLayout, sShadowPipelineLayout;'),
    ('      .layout = sPipelineLayout,',
     '      .layout = config.shaderConfig.shadowReplay ? sShadowPipelineLayout : sPipelineLayout,'),
    ('    sPipelineLayout = g_device.CreatePipelineLayout(&desc);',
     '    sPipelineLayout = g_device.CreatePipelineLayout(&desc);\n'
     '    auto shadowDesc = desc;\n'
     '    shadowDesc.label = "GX Shadow Pipeline Layout";\n'
     '    auto shadowLayouts = layouts;\n'
     '    shadowLayouts[3] = gfx::shadow::projection_layout();\n'
     '    shadowDesc.bindGroupLayouts = shadowLayouts.data();\n'
     '    sShadowPipelineLayout = g_device.CreatePipelineLayout(&shadowDesc);'),
    ('  sPipelineLayout = {};', '  sPipelineLayout = {};\n  sShadowPipelineLayout = {};')])
edit('lib/gx/pipeline.cpp', [
    ('  if (bind_native_group(3, logic)) pass.SetBindGroup(3, logic);',
     '  if(data.shadowProjectionOffset!=UINT32_MAX){\n'
     '    if(bind_native_group(3,logic,data.shadowProjectionOffset))\n'
     '      pass.SetBindGroup(3,logic,1,&data.shadowProjectionOffset);\n'
     '  }else if (bind_native_group(3, logic)) pass.SetBindGroup(3, logic);')])
edit('lib/gx/shader.cpp', [
    ('    texBindings += "\\n@group(3) @binding(1) var<storage, read_write> bbox: array<atomic<u32>, 4>;";',
     '    if (!config.shadowReplay) texBindings += "\\n@group(3) @binding(1) var<storage, read_write> bbox: array<atomic<u32>, 4>;";'),
    ('        "\\n    out.pos = vec4f(mv_pos, 1.0) * ubuf.proj;",\n        vtx_attr(config, GX_VA_POS));',
     '        "\\n    out.pos = vec4f(mv_pos, 1.0) * {};",\n'
     '        vtx_attr(config, GX_VA_POS), config.shadowReplay ? "shadow_projection" : "ubuf.proj");'),
    ('  if (config.boundingBox) {\n    fragmentFn += R"(',
     '  if (config.boundingBox && !config.shadowReplay) {\n    fragmentFn += R"('),
    ('  return shaderSource;',
     '  if (config.shadowReplay) {\n'
     '    shaderSource += "\\n@group(3) @binding(0) var<uniform> shadow_projection: mat4x4f;\\n";\n'
     '  }\n  return shaderSource;')])
edit('lib/gfx/frame_packet.hpp', [
    ('    uint32_t projectionOffset=0, view=0; std::array<Range,2> cascades{}; };',
     '    uint32_t view=0; bool selected=false; };'),
    ('  uint32_t shadowResolution=0, shadowCascades=0;',
     '  uint32_t shadowResolution=0, shadowCascades=0;\n'
     '  std::array<Range,2> shadowProjection{};')])

for path, source in updates.items():
    path.write_text(source)
