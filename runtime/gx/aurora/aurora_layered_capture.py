"""Capture visible view depth and camera identity before guest depth reuse.
Pinned, transactional overlay applied after the original enhancement overlay.
"""
from pathlib import Path
import sys
root=Path(sys.argv[1]);marker='GEKKOAOT_LAYERED_SCENE_CAPTURE_V1'
updates={}
def edit(name,changes):
    p=root/name;s=p.read_text()
    if marker in s:return
    for change in changes:
        old,new=change[:2];count=change[2] if len(change)>2 else 1
        if s.count(old)!=count:raise RuntimeError(f'{name}: layered scene anchor changed {old[:100]} ({s.count(old)}/{count})')
        s=s.replace(old,new)
    updates[p]='// '+marker+'\n'+s
edit('include/aurora/gfx.hpp', [('  Normal,\n  Auxiliary,', '  Normal,\n  SceneCapture,\n  Auxiliary,')])
edit('lib/webgpu/gpu.hpp', [('extern TextureWithSampler g_normalBufferResolved;',
  'extern TextureWithSampler g_normalBufferResolved;\nextern TextureWithSampler g_sceneCaptureBuffer;')])
edit('lib/webgpu/gpu.cpp', [('TextureWithSampler g_normalBufferResolved;',
  'TextureWithSampler g_normalBufferResolved;\nTextureWithSampler g_sceneCaptureBuffer;'),
 ('bool enable_normal_buffer() {',
  'static void ensure_scene_capture(uint32_t width,uint32_t height,uint32_t samples) {\n'
  '  if(!g_graphicsConfig.enhancedCapture){g_sceneCaptureBuffer={};return;}\n'
  '  if(g_sceneCaptureBuffer.texture && g_sceneCaptureBuffer.size.width==width &&\n'
  '     g_sceneCaptureBuffer.size.height==height && g_sceneCaptureBuffer.texture.GetSampleCount()==samples)return;\n'
  '  const wgpu::TextureDescriptor desc{.label="GX view depth and camera '
  'ID",.usage=wgpu::TextureUsage::RenderAttachment|wgpu::TextureUsage::TextureBinding,\n'
  '    .size={width,height,1},.format=wgpu::TextureFormat::R32Float,.sampleCount=samples};\n'
  '  auto texture=g_device.CreateTexture(&desc);\n'
  '  g_sceneCaptureBuffer={.texture=texture,.view=texture.CreateView(),.size=desc.size,.format=desc.format};\n'
  '}\n'
  'bool enable_normal_buffer() {\n'
  '  ensure_scene_capture(g_frameBuffer.size.width,g_frameBuffer.size.height,g_graphicsConfig.msaaSamples);'),
 ('  g_normalBufferResolved = {};', '  g_normalBufferResolved = {};\n  g_sceneCaptureBuffer = {};'),
 ('    g_normalBuffer = create_normal_texture(width, height, g_graphicsConfig.msaaSamples);',
  '    ensure_scene_capture(width,height,g_graphicsConfig.msaaSamples);\n'
  '    g_normalBuffer = create_normal_texture(width, height, g_graphicsConfig.msaaSamples);')])
edit('lib/gfx/frame.cpp', [('  finalize_render_target_layout(layout);\n  return layout;',
  '  if(webgpu::g_graphicsConfig.enhancedCapture && webgpu::g_sceneCaptureBuffer.texture)\n'
  '    layout.colorAttachments[layout.colorAttachmentCount++]={.semantic=ColorAttachmentSemantic::SceneCapture,\n'
  '      '
  '.format=wgpu::TextureFormat::R32Float,.width=webgpu::g_sceneCaptureBuffer.size.width,.height=webgpu::g_sceneCaptureBuffer.size.height};\n'
  '  finalize_render_target_layout(layout);\n'
  '  return layout;')])
edit('lib/gfx/frame_packet.hpp', [('  wgpu::Texture copySourceNormalTexture;',
  '  wgpu::Texture copySourceNormalTexture;\n  wgpu::Texture copySourceCaptureTexture;'),
 ('GekkoAOT::GX::SceneCapture sceneCapture;', 'GekkoAOT::GX::SceneCaptureLayers sceneCapture;')])
edit('lib/gfx/recording.cpp', [('GekkoAOT::GX::SceneCapture g_enhancedLastCapture;', 'GekkoAOT::GX::SceneCaptureLayers g_enhancedLastCapture;'),
 ('    if((preserveEfb & 1u)!=0u)pass.sceneCapture=g_enhancedLastCapture;\n',
  '    // Scene identity follows retained color, independently of guest Z.\n'),
 ('  if ((preserveEfb & 2u) != 0u) {',
  '  if ((preserveEfb & 1u) != 0u) pass.sceneCapture=g_enhancedLastCapture;\n'
  '  if ((preserveEfb & 2u) != 0u) {'),
 ('      color.resolveView = layout.sampleCount > 1 ? webgpu::g_normalBufferResolved.view : nullptr;\n    } else {',
  '      color.resolveView = layout.sampleCount > 1 ? webgpu::g_normalBufferResolved.view : nullptr;\n'
  '    } else if(color.semantic==ColorAttachmentSemantic::SceneCapture){\n'
  '      color.size=webgpu::g_sceneCaptureBuffer.size;color.view=webgpu::g_sceneCaptureBuffer.view;\n'
  '    } else {'),
 ('  pass.msaaSamples = layout.sampleCount;',
  '  pass.copySourceCaptureTexture=webgpu::g_sceneCaptureBuffer.texture;\n  pass.msaaSamples = layout.sampleCount;'),
 ('      .copySourceNormalTexture = prevPass.copySourceNormalTexture,',
  '      .copySourceNormalTexture = prevPass.copySourceNormalTexture,\n'
  '      .copySourceCaptureTexture = prevPass.copySourceCaptureTexture,',
  2),
 ('void enhanced::observe_scene(const GekkoAOT::GX::SceneCapture& capture) noexcept {\n'
  '  if(!g_recorder.active()||g_recorder.inOffscreen||g_recorder.currentRenderPass==UINT32_MAX)return;\n'
  '  auto& scene=current_render_passes()[g_recorder.currentRenderPass].sceneCapture;\n'
  '  if(scene.validity==GekkoAOT::GX::SceneValidity::Missing)scene=capture;\n'
  '  else '
  'if(capture.validity==GekkoAOT::GX::SceneValidity::Valid)scene.Observe(capture.projection,capture.viewport,capture.depth_range);\n'
  '  else scene.validity=capture.validity;\n'
  '}',
  'unsigned enhanced::observe_scene(const GekkoAOT::GX::SceneCapture& capture) noexcept {\n'
  '  if(!g_recorder.active()||g_recorder.inOffscreen||g_recorder.currentRenderPass==UINT32_MAX)return 0;\n'
  '  return current_render_passes()[g_recorder.currentRenderPass].sceneCapture.Observe(capture);\n'
  '}'),
 ('  if((clearDepth && fullTarget)||fullColorClear)newPass.sceneCapture={};',
  '  if(fullColorClear)newPass.sceneCapture={};'),
 ('    if (color.semantic == ColorAttachmentSemantic::Normal && (clearDepth || '
  '(webgpu::g_graphicsConfig.enhancedCapture && clearColor)) && fullTarget) {',
  '    if (((color.semantic == ColorAttachmentSemantic::Normal &&\n'
  '          ((clearDepth && !webgpu::g_graphicsConfig.enhancedCapture) || clearColor)) ||\n'
  '          (color.semantic==ColorAttachmentSemantic::SceneCapture && clearColor)) && fullTarget) {')])
edit('lib/gx/gx.hpp', [('struct ShaderInfo {', 'struct ShaderInfo {\n  bool enhancedCapture=false;')])
edit('lib/gx/gx.cpp', [('layout.colorAttachments[i].semantic == gfx::ColorAttachmentSemantic::Normal && (writesDepth || '
  '(config.shaderConfig.enhancementMask && options.colorUpdate))',
  '((layout.colorAttachments[i].semantic == gfx::ColorAttachmentSemantic::Normal &&\n'
  '          (writesDepth || (config.shaderConfig.enhancementMask && options.colorUpdate))) ||\n'
  '          (layout.colorAttachments[i].semantic==gfx::ColorAttachmentSemantic::SceneCapture && '
  'options.colorUpdate))')])
edit('lib/gx/shader_info.cpp', [('  if (config.boundingBox) {\n    info.usesBoundingBox = true;',
  '  info.enhancedCapture=config.enhancementMask;\n  if (config.boundingBox) {\n    info.usesBoundingBox = true;'),
 ('  info.uniformSize = gfx::align_uniform(info.uniformSize);',
  '  if(config.enhancementMask)info.uniformSize+=16;\n  info.uniformSize = gfx::align_uniform(info.uniformSize);'),
 ('  buf.append(proj);',
  '  buf.append(proj);\n  if(info.enhancedCapture)buf.append(Vec4<uint32_t>{gfx::enhanced::current_view_id(),0,0,0});')])
edit('lib/gx/shader.cpp', [('  uniBufAttrs += "\\n    proj: mat4x4f,";',
  '  uniBufAttrs += "\\n    proj: mat4x4f,";\n'
  '  if(config.enhancementMask)uniBufAttrs += "\\n    enhanced_view: vec4u,";\n'
  '  if(config.enhancementMask && normalAttachment!=UINT32_MAX){\n'
  '    vtxOutAttrs += fmt::format("\\n    @location({}) capture_z:f32,",vtxOutIdx++);\n'
  '    vtxXfrAttrsPre += "\\n    out.capture_z = -mv_pos.z;";\n'
  '  }'),
 ('  if (config.boundingBox) {\n    fragmentFn +=',
  '  if(config.enhancementMask && normalAttachment!=UINT32_MAX){\n'
  '    const auto end=fragmentOutput.rfind("};");\n'
  '    fragmentOutput.insert(end,fmt::format("    @location({}) capture:f32,\\n",normalAttachment+1));\n'
  '    const auto ret=fragmentReturn.rfind("return out;");\n'
  '    fragmentReturn.insert(ret,"out.capture = 0.0;\\n    if(out.normal.w > 0.99 && in.capture_z > 0.0 && '
  'ubuf.enhanced_view.x > 0u){"\n'
  '      "out.capture=bitcast<f32>((bitcast<u32>(in.capture_z)&0xfffffff0u)|ubuf.enhanced_view.x); }\\n    ");\n'
  '  }\n'
  '  if (config.boundingBox) {\n'
  '    fragmentFn +=')])
edit('lib/gfx/clear.cpp', [('(config.clearDepth || config.clearColor) && layout.colorAttachments[i].semantic == ColorAttachmentSemantic::Normal',
  '((config.clearDepth && !webgpu::g_graphicsConfig.enhancedCapture) || config.clearColor) && '
  'layout.colorAttachments[i].semantic == ColorAttachmentSemantic::Normal',
  2),
 ('  if (!outputs.empty()) {',
  '  if(config.clearColor)for(uint32_t i=0;i<layout.colorAttachmentCount;++i)\n'
  '    if(layout.colorAttachments[i].semantic==ColorAttachmentSemantic::SceneCapture){\n'
  '      outputs+=fmt::format("    @location({0}) capture{0}:f32,\\n",i);\n'
  '      values+=fmt::format("    out.capture{} = 0.0;\\n",i);\n'
  '    }\n'
  '  if (!outputs.empty()) {'),
 ('  colorTargets[SceneColorAttachmentIndex].blend = &blendState;',
  '  for(uint32_t i=0;i<layout.colorAttachmentCount;++i)\n'
  '    if(layout.colorAttachments[i].semantic==ColorAttachmentSemantic::SceneCapture)\n'
  '      colorTargets[i].writeMask=config.clearColor?wgpu::ColorWriteMask::All:wgpu::ColorWriteMask::None;\n'
  '  colorTargets[SceneColorAttachmentIndex].blend = &blendState;')])
edit('lib/gx/pipeline.hpp', [('GXPipelineConfigVersion = 18;', 'GXPipelineConfigVersion = 19;')])
for p,s in updates.items():p.write_text(s)
