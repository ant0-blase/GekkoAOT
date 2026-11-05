"""Retain frame-local caster plans and replay their real GX geometry to depth.

Two users: the explicit validation view, and sun cascades. A cascade plan keeps
the draw's original uniform; the sealed pass patches one projection per
cascade into copies of it. Transactional and pinned.
"""
from pathlib import Path
import sys
root=Path(sys.argv[1]);marker='GEKKOAOT_SHADOW_REPLAY_V1';updates={}
def edit(name,changes):
    path=root/name;source=path.read_text()
    if marker in source:return
    for change in changes:
        old,new=change[:2];count=change[2] if len(change)>2 else 1
        if source.count(old)!=count:raise RuntimeError(f'{name}: shadow replay anchor changed: {old[:100]}')
        source=source.replace(old,new)
    updates[path]='// '+marker+'\n'+source

edit('lib/gx/pipeline.hpp', [
    ('  uint32_t dstAlpha;\n};', '  uint32_t dstAlpha;\n  uint32_t shadowIndex = 0;\n};')])
edit('lib/gfx/frame_packet.hpp', [
    ('  Range enhancedUniformRange;',
     '  struct ShadowPlan { PipelineRef pipeline; Range uniform; bool cascade=false;\n'
     '    uint32_t projectionOffset=0, view=0; std::array<Range,2> cascades{}; };\n'
     '  std::vector<ShadowPlan> shadowPlans;\n'
     '  uint64_t shadowGeneration=0;\n  uint32_t shadowResolution=0, shadowCascades=0;\n  Range enhancedUniformRange;')])
edit('lib/gx/shader_info.hpp', [
    ('gfx::Range build_uniform(const ShaderInfo& info) noexcept;',
     'gfx::Range build_uniform(const ShaderInfo& info) noexcept;\n'
     'gfx::Range build_shadow_uniform(const ShaderInfo& info, const float* projection) noexcept;\n'
     '// Byte offset of the projection in the most recently built draw uniform.\n'
     'std::uint32_t replay_projection_offset() noexcept;')])
edit('lib/gx/shader_info.cpp', [
    ('gfx::Range build_uniform(const ShaderInfo& info) noexcept {',
     'gfx::Range build_shadow_uniform(const ShaderInfo& info, const float* projection) noexcept {\n'
     '  ByteBuffer buf;\n  fill_uniform(buf,info);\n'
     '  if(g_gekkoaotReplayProjOffset+64u>buf.size())return {};\n'
     '  std::memcpy(buf.data()+g_gekkoaotReplayProjOffset,projection,64);\n'
     '  return gfx::push_uniform(buf.data(),buf.size());\n}\n\n'
     'std::uint32_t replay_projection_offset() noexcept { return g_gekkoaotReplayProjOffset; }\n\n'
     'gfx::Range build_uniform(const ShaderInfo& info) noexcept {')])
edit('lib/gfx/recording.cpp', [
    ('#include "recording.hpp"', '#include "recording.hpp"\n#include "gekkoaot_shadow_replay.hpp"'),
    ('template <>\nvoid push_draw_command(gx::DrawData data) {',
     'const gx::DrawData* shadow::gx_payload(const detail::DrawCommand& command) {\n'
     '  if(command.encoder!=encode_draw<gx::render,gx::DrawData>)return nullptr;\n'
     '  return reinterpret_cast<const gx::DrawData*>(command.payload.data());\n}\n'
     'bool shadow::recording_boundary() {\n'
     '  if(!g_recorder.active() || g_recorder.inOffscreen || g_recorder.currentRenderPass==UINT32_MAX)return false;\n'
     '  auto& frame=current_frame_packet();\n'
     '  auto& previous=current_render_passes()[g_recorder.currentRenderPass];\n'
     '  enqueue_pass(frame,g_recorder.currentRenderPass);\n'
     '  resume_efb_pass_loading(previous);\n  return true;\n}\n'
     'bool shadow::copy_uniform(Range range,std::vector<uint8_t>& out) {\n'
     '  if(!g_recorder.active())return false;\n'
     '  auto& uniforms=current_frame_packet().uniforms;\n'
     '  if(!range.size || range.offset+range.size>uniforms.size())return false;\n'
     '  out.assign(uniforms.data()+range.offset,uniforms.data()+range.offset+range.size);\n  return true;\n}\n'
     'void shadow::record_draw(gx::DrawData& draw,const gx::PipelineConfig& config) {\n'
     '  if(!g_recorder.active() || g_recorder.inOffscreen || g_recorder.currentRenderPass==UINT32_MAX)return;\n'
     '  prepare(draw,config,current_render_passes()[g_recorder.currentRenderPass]);\n}\n\n'
     'template <>\nvoid push_draw_command(gx::DrawData data) {')])
edit('lib/gx/command_processor.cpp', [
    ('#include "pipeline.hpp"', '#include "pipeline.hpp"\n#include "../gfx/gekkoaot_shadow_replay.hpp"'),
    ('    }\n    return;\n  }\n  DrawData draw{',
     '      gfx::shadow::record_draw(*draw,cache.config);\n    }\n    return;\n  }\n  DrawData draw{')])
edit('lib/gfx/encoding.cpp', [
    ('#include "encoding.hpp"', '#include "encoding.hpp"\n#include "gekkoaot_shadow_replay.hpp"'),
    ('  std::array<wgpu::RenderPassColorAttachment, MaxColorAttachments> attachments{};',
     '  shadow::encode(cmd,passInfo);\n'
     '  std::array<wgpu::RenderPassColorAttachment, MaxColorAttachments> attachments{};')])
for path,source in updates.items():path.write_text(source)
