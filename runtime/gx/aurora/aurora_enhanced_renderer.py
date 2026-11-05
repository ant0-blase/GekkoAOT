"""Install the owned scene capture and GPU enhancement integration in pinned Aurora.
All anchors are preflighted before source changes. Existing TEV color math is unchanged.
"""
from pathlib import Path
import sys
root = Path(sys.argv[1])
updates = {}
marker = 'GEKKOAOT_ENHANCED_RENDERER_V1'

def edit(name, fn):
    p = root / name
    s = p.read_text()
    if marker in s:
        return

    def replace(old, new, count=1):
        nonlocal s
        if s.count(old) != count:
            raise RuntimeError(f'{name}: enhanced anchor changed: {old[:100]} ({s.count(old)})')
        s = s.replace(old, new)
    fn(replace)
    updates[p] = '// ' + marker + '\n' + s
edit('lib/webgpu/gpu.hpp', lambda r: (r('bool normalBuffer = false;', 'bool normalBuffer = false;\n  bool enhancedCapture = false;'), r('extern TextureWithSampler g_normalBuffer;', 'extern TextureWithSampler g_normalBuffer;\nextern TextureWithSampler g_normalBufferResolved;')))

def gpu(r):
    r('TextureWithSampler g_normalBuffer;', 'TextureWithSampler g_normalBuffer;\nTextureWithSampler g_normalBufferResolved;')
    r('static TextureWithSampler create_normal_texture(uint32_t width, uint32_t height) {', 'static TextureWithSampler create_normal_texture(uint32_t width, uint32_t height, uint32_t samples = 1) {')
    r('.label = "Scene normals",\n      .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc,', '.label = "Scene normals",\n      .usage = wgpu::TextureUsage::RenderAttachment | (samples == 1 ? wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc : wgpu::TextureUsage::None),')
    r('.format = NormalBufferFormat,', '.format = NormalBufferFormat,\n      .sampleCount = samples,')
    r('if (!g_hasCoreFeatures || g_graphicsConfig.msaaSamples != 1 || !g_device || g_frameBuffer.size.width == 0 ||', 'if (!g_device || g_frameBuffer.size.width == 0 ||')
    r('g_normalBuffer = create_normal_texture(g_frameBuffer.size.width, g_frameBuffer.size.height);', 'g_normalBuffer = create_normal_texture(g_frameBuffer.size.width, g_frameBuffer.size.height, g_graphicsConfig.msaaSamples);\n  g_normalBufferResolved = g_graphicsConfig.msaaSamples > 1 ? create_normal_texture(g_frameBuffer.size.width,g_frameBuffer.size.height) : g_normalBuffer;')
    r('g_normalBuffer = create_normal_texture(width, height);', 'g_normalBuffer = create_normal_texture(width, height, g_graphicsConfig.msaaSamples);\n    g_normalBufferResolved = g_graphicsConfig.msaaSamples > 1 ? create_normal_texture(width,height) : g_normalBuffer;')
    r('g_normalBuffer = {};', 'g_normalBuffer = {};\n  g_normalBufferResolved = {};\n  g_graphicsConfig.enhancedCapture = false;')
edit('lib/webgpu/gpu.cpp', gpu)

def packet(r):
    r('#include "pipeline_cache.hpp"', '#include "pipeline_cache.hpp"\n#include "gekkoaot_scene_capture.h"')
    r('uint32_t msaaSamples = 1;', 'uint32_t msaaSamples = 1;\n  GekkoAOT::GX::SceneCapture sceneCapture;\n  Range enhancedUniformRange;\n  uint32_t enhancedWidth=0,enhancedHeight=0;')
edit('lib/gfx/frame_packet.hpp', packet)

def recording(r):
    r('#include "recording.hpp"', '#include "recording.hpp"\n#include "gekkoaot_enhanced_renderer.hpp"')
    r('color.view = webgpu::g_normalBuffer.view;', 'color.view = webgpu::g_normalBuffer.view;\n      color.resolveView = layout.sampleCount > 1 ? webgpu::g_normalBufferResolved.view : nullptr;')
    r('pass.copySourceNormalTexture = webgpu::g_normalBuffer.texture;', 'pass.copySourceNormalTexture = webgpu::g_normalBufferResolved.texture;')
    r('void seal_pass(FramePacket& frame, uint32_t passIndex) {', 'GekkoAOT::GX::SceneCapture g_enhancedLastCapture;\n\nvoid seal_pass(FramePacket& frame, uint32_t passIndex) {')
    r('  pass.sealed = true;', '  enhanced::seal(pass);\n  pass.sealed = true;')
    r('.msaaSamples = prevPass.msaaSamples,', '.msaaSamples = prevPass.msaaSamples,\n      .sceneCapture = prevPass.sceneCapture,')
    r('.msaaSamples = msaaSamples,\n      .clearDepthValue = clearDepthValue,', '.msaaSamples = msaaSamples,\n      .sceneCapture = prevPass.sceneCapture,\n      .clearDepthValue = clearDepthValue,')
    r('  const bool fullColorClear = clearColor && clearAlpha && fullTarget;', '  const bool fullColorClear = clearColor && clearAlpha && fullTarget;\n  if((clearDepth && fullTarget)||fullColorClear)newPass.sceneCapture={};')
    r('color.semantic == ColorAttachmentSemantic::Normal && clearDepth && fullTarget', 'color.semantic == ColorAttachmentSemantic::Normal && (clearDepth || (webgpu::g_graphicsConfig.enhancedCapture && clearColor)) && fullTarget')
    r('    pass.clearDepth = false;\n    pass.depthLoadOp = wgpu::LoadOp::Load;', '    pass.clearDepth = false;\n    pass.depthLoadOp = wgpu::LoadOp::Load;\n    if((preserveEfb & 1u)!=0u)pass.sceneCapture=g_enhancedLastCapture;')
    r('void gekkoaot_capture_peek_snapshot() noexcept {', 'void enhanced::observe_scene(const GekkoAOT::GX::SceneCapture& capture) noexcept {\n  if(!g_recorder.active()||g_recorder.inOffscreen||g_recorder.currentRenderPass==UINT32_MAX)return;\n  auto& scene=current_render_passes()[g_recorder.currentRenderPass].sceneCapture;\n  if(scene.validity==GekkoAOT::GX::SceneValidity::Missing)scene=capture;\n  else if(capture.validity==GekkoAOT::GX::SceneValidity::Valid)scene.Observe(capture.projection,capture.viewport,capture.depth_range);\n  else scene.validity=capture.validity;\n}\nvoid gekkoaot_capture_peek_snapshot() noexcept {')
    r('    if (!gekkoaotPeekMarked) {\n      pass.captureDepthSnapshot = true;\n    }\n    enqueue_pass(frame, g_recorder.currentRenderPass);', '    if (!gekkoaotPeekMarked) {\n      pass.captureDepthSnapshot = true;\n    }\n    g_enhancedLastCapture=pass.sceneCapture;\n    enqueue_pass(frame, g_recorder.currentRenderPass);')
edit('lib/gfx/recording.cpp', recording)

def encoding(r):
    r('#include "encoding.hpp"', '#include "encoding.hpp"\n#include "gekkoaot_enhanced_renderer.hpp"')
    r('  if (passInfo.snapshotColorDst) {', '  enhanced::encode(cmd,passInfo);\n\n  if (passInfo.snapshotColorDst) {')
edit('lib/gfx/encoding.cpp', encoding)
edit('lib/gx/gx.hpp', lambda r: r('u8 pad1 : 4 = 0;', 'u8 enhancementMask : 1 = false;\n  u8 enhancedEligible : 1 = false;\n  u8 enhancedAlphaBlend : 1 = false;\n  u8 pad1 : 1 = 0;'))

def gx(r):
    r('#include "gx.hpp"', '#include "gx.hpp"\n#include "../gfx/gekkoaot_enhanced_renderer.hpp"')
    r('config.shaderConfig.boundingBox = gekkoaot_bbox_active();', 'config.shaderConfig.boundingBox = gekkoaot_bbox_active();\n  config.shaderConfig.enhancementMask = webgpu::g_graphicsConfig.enhancedCapture;\n  config.shaderConfig.enhancedEligible = gfx::enhanced::eligible();\n  config.shaderConfig.enhancedAlphaBlend = g_gxState.blendMode == GX_BM_BLEND && g_gxState.blendFacDst == GX_BL_INVSRCALPHA && (g_gxState.blendFacSrc == GX_BL_SRCALPHA || g_gxState.blendFacSrc == GX_BL_ONE);')
    r('layout.colorAttachments[i].semantic == gfx::ColorAttachmentSemantic::Normal && writesDepth', 'layout.colorAttachments[i].semantic == gfx::ColorAttachmentSemantic::Normal && (writesDepth || (config.shaderConfig.enhancementMask && options.colorUpdate))')
edit('lib/gx/gx.cpp', gx)

def shader(r):
    r('  if (dstAlphaMode == DstAlphaMode::Replace) {', '  if(config.enhancementMask && config.enhancedAlphaBlend)fragmentFn += "\\n    let gekkoaot_opaque = prev.a >= 1.0;";\n  if (dstAlphaMode == DstAlphaMode::Replace) {')
    r('    fragmentReturn += "\\n    return out;";', '    if(config.enhancementMask)fragmentReturn += config.enhancedEligible ? (config.enhancedAlphaBlend ? "\\n    out.normal.w = select(0.0, 1.0, gekkoaot_opaque);" : "\\n    out.normal.w = 1.0;") : "\\n    out.normal.w = 0.0;";\n    fragmentReturn += "\\n    return out;";')
    r('      fragmentReturn += "\\n    out.depth = gx_depth;\\n    return out;";', '      if(config.enhancementMask)fragmentReturn += config.enhancedEligible ? (config.enhancedAlphaBlend ? "\\n    out.normal.w = select(0.0, 1.0, gekkoaot_opaque);" : "\\n    out.normal.w = 1.0;") : "\\n    out.normal.w = 0.0;";\n      fragmentReturn += "\\n    out.depth = gx_depth;\\n    return out;";')
edit('lib/gx/shader.cpp', shader)

def uniform(r):
    r('#include "shader_info.hpp"', '#include "shader_info.hpp"\n#include "../gfx/gekkoaot_enhanced_renderer.hpp"')
    r('  buf.append(proj);', '  gfx::enhanced::capture_projection(reinterpret_cast<const float*>(&proj));\n  buf.append(proj);')
edit('lib/gx/shader_info.cpp', uniform)

def commands(r):
    r('#include "command_processor.hpp"', '#include "command_processor.hpp"\n#include "../gfx/gekkoaot_enhanced_renderer.hpp"')
    r('  const bool bbox = gekkoaot_bbox_active();', '  const bool enhanced = webgpu::g_graphicsConfig.enhancedCapture, eligible = gfx::enhanced::eligible();\n  hash.update(&enhanced,sizeof(enhanced)); hash.update(&eligible,sizeof(eligible));\n  const bool bbox = gekkoaot_bbox_active();')
    r('  auto& cache = sDrawCache;\n\n  const u8 lineMode', '  auto& cache = sDrawCache;\n  gfx::enhanced::observe_draw();\n\n  const u8 lineMode')
edit('lib/gx/command_processor.cpp', commands)

def clear(r):
    r('config.clearDepth && layout.colorAttachments[i].semantic == ColorAttachmentSemantic::Normal', '(config.clearDepth || config.clearColor) && layout.colorAttachments[i].semantic == ColorAttachmentSemantic::Normal', 2)
edit('lib/gfx/clear.cpp', clear)
for path, source in updates.items():
    path.write_text(source)

# Deferred encoding must use the sources sealed with this pass, rather than
# the mutable candidate list of a later GX packet.
p=root/'lib/gfx/frame_packet.hpp'
s=p.read_text()
if 'bool enhancedParticleSources=false;' not in s:
    anchor='uint32_t enhancedWidth=0,enhancedHeight=0;'
    assert s.count(anchor)==1
    p.write_text(s.replace(anchor,anchor+'\n  bool enhancedParticleSources=false;'))
