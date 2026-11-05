"""Keep opaque provenance through affine GX lighting/transparency operations.
Requires the layered capture overlay. Preflight every edit before writing.
"""
from pathlib import Path
import sys
root=Path(sys.argv[1]);marker='GEKKOAOT_CONTRIBUTION_CAPTURE_V1';updates={}
def edit(name,changes):
    p=root/name;s=p.read_text()
    if marker in s:return
    for change in changes:
        old,new=change[:2];count=change[2] if len(change)>2 else 1
        if s.count(old)!=count:raise RuntimeError(f'{name}: contribution anchor changed {old[:100]} ({s.count(old)}/{count})')
        s=s.replace(old,new)
    updates[p]='// '+marker+'\n'+s
edit('include/aurora/gfx.hpp',[('  SceneCapture,','  SceneCapture,\n  SceneContribution,')])
edit('lib/webgpu/gpu.hpp',[('extern TextureWithSampler g_sceneCaptureBuffer;','extern TextureWithSampler g_sceneCaptureBuffer;\nextern TextureWithSampler g_sceneContributionBuffer,g_sceneContributionResolved;')])
edit('lib/webgpu/gpu.cpp',[
 ('TextureWithSampler g_sceneCaptureBuffer;','TextureWithSampler g_sceneCaptureBuffer;\nTextureWithSampler g_sceneContributionBuffer,g_sceneContributionResolved;'),
 ('if(!g_graphicsConfig.enhancedCapture){g_sceneCaptureBuffer={};return;}','if(!g_graphicsConfig.enhancedCapture){g_sceneCaptureBuffer={};g_sceneContributionBuffer={};g_sceneContributionResolved={};return;}'),
 ('.size={width,height,1},.format=wgpu::TextureFormat::R32Float,.sampleCount=samples};','.size={width,height,1},.format=wgpu::TextureFormat::RGBA16Float,.sampleCount=samples};'),
 ('  g_sceneCaptureBuffer={.texture=texture,.view=texture.CreateView(),.size=desc.size,.format=desc.format};',
  '  g_sceneCaptureBuffer={.texture=texture,.view=texture.CreateView(),.size=desc.size,.format=desc.format};\n'
  '  auto colorDesc=desc;colorDesc.label="GX opaque color contribution";\n'
  '  auto color=g_device.CreateTexture(&colorDesc);\n'
  '  g_sceneContributionBuffer={.texture=color,.view=color.CreateView(),.size=colorDesc.size,.format=colorDesc.format};\n'
  '  if(samples>1){colorDesc.sampleCount=1;auto resolved=g_device.CreateTexture(&colorDesc);\n'
  '    g_sceneContributionResolved={.texture=resolved,.view=resolved.CreateView(),.size=colorDesc.size,.format=colorDesc.format};\n'
  '  }else g_sceneContributionResolved=g_sceneContributionBuffer;'),
 ('  g_sceneCaptureBuffer = {};','  g_sceneCaptureBuffer = {};\n  g_sceneContributionBuffer={};g_sceneContributionResolved={};')])
edit('lib/gfx/frame.cpp',[
 ('.format=wgpu::TextureFormat::R32Float,.width=webgpu::g_sceneCaptureBuffer.size.width,.height=webgpu::g_sceneCaptureBuffer.size.height};',
  '.format=wgpu::TextureFormat::RGBA16Float,.width=webgpu::g_sceneCaptureBuffer.size.width,.height=webgpu::g_sceneCaptureBuffer.size.height};\n'
  '  if(webgpu::g_graphicsConfig.enhancedCapture && webgpu::g_sceneContributionBuffer.texture)\n'
  '    layout.colorAttachments[layout.colorAttachmentCount++]={.semantic=ColorAttachmentSemantic::SceneContribution,\n'
  '      .format=wgpu::TextureFormat::RGBA16Float,.width=webgpu::g_sceneContributionBuffer.size.width,.height=webgpu::g_sceneContributionBuffer.size.height};')])
edit('lib/gfx/frame_packet.hpp',[('  Range enhancedUniformRange;', '  Range enhancedUniformRange;\n  std::array<uint32_t,12> enhancedReasons{};\n  std::array<uint32_t,5> enhancedComposition{};'),('  wgpu::Texture copySourceCaptureTexture;','  wgpu::Texture copySourceCaptureTexture;\n  wgpu::Texture copySourceContributionTexture;')])
edit('lib/gfx/recording.cpp',[
 ('  if(fullColorClear)newPass.sceneCapture={};','  if(clearColor && fullTarget){newPass.sceneCapture={};enhanced::note_capture_cleared();}'),
 ('void gekkoaot_capture_peek_snapshot() noexcept {',
  'void enhanced::record_classification(unsigned reason,unsigned composition) noexcept {\n'
  '  if(!g_recorder.active()||g_recorder.inOffscreen||g_recorder.currentRenderPass==UINT32_MAX)return;\n'
  '  auto& pass=current_render_passes()[g_recorder.currentRenderPass];\n'
  '  if(reason<pass.enhancedReasons.size())++pass.enhancedReasons[reason];\n'
  '  if(composition<pass.enhancedComposition.size())++pass.enhancedComposition[composition];\n'
  '}\nvoid gekkoaot_capture_peek_snapshot() noexcept {'),
 ('      color.size=webgpu::g_sceneCaptureBuffer.size;color.view=webgpu::g_sceneCaptureBuffer.view;',
  '      color.size=webgpu::g_sceneCaptureBuffer.size;color.view=webgpu::g_sceneCaptureBuffer.view;\n'
  '    } else if(color.semantic==ColorAttachmentSemantic::SceneContribution){\n'
  '      color.size=webgpu::g_sceneContributionBuffer.size;color.view=webgpu::g_sceneContributionBuffer.view;\n'
  '      color.resolveView=layout.sampleCount>1?webgpu::g_sceneContributionResolved.view:nullptr;'),
 ('  pass.copySourceCaptureTexture=webgpu::g_sceneCaptureBuffer.texture;',
  '  pass.copySourceCaptureTexture=webgpu::g_sceneCaptureBuffer.texture;\n  pass.copySourceContributionTexture=webgpu::g_sceneContributionResolved.texture;'),
 ('      .copySourceCaptureTexture = prevPass.copySourceCaptureTexture,',
  '      .copySourceCaptureTexture = prevPass.copySourceCaptureTexture,\n      .copySourceContributionTexture = prevPass.copySourceContributionTexture,',2),
 ('(color.semantic==ColorAttachmentSemantic::SceneCapture && clearColor)',
  '((color.semantic==ColorAttachmentSemantic::SceneCapture || color.semantic==ColorAttachmentSemantic::SceneContribution) && clearColor)')])
edit('lib/gx/gx.hpp',[('struct ShaderConfig {','struct ShaderConfig {\n  u32 enhancedComposition = 0;')])
edit('lib/gx/gx.cpp',[
 ('  config.shaderConfig.enhancedEligible = gfx::enhanced::eligible();',
  '  config.shaderConfig.enhancedEligible = gfx::enhanced::eligible();\n  config.shaderConfig.enhancedComposition=gfx::enhanced::composition_mode();'),
 ('  colorTargets[gfx::SceneColorAttachmentIndex].blend = &blendState;',
  '  const wgpu::BlendState metadataBlend{\n'
  '    .color={.srcFactor=wgpu::BlendFactor::SrcAlpha,.dstFactor=wgpu::BlendFactor::OneMinusSrcAlpha},\n'
  '    .alpha={.srcFactor=wgpu::BlendFactor::One,.dstFactor=wgpu::BlendFactor::OneMinusSrcAlpha}};\n'
  '  auto contributionBlend=to_blend_state(config.blendMode,config.blendFacSrc,config.blendFacDst,config.blendOp,UINT32_MAX,false);\n'
  '  // Stored alpha stays 0 (the fragment alpha only drives the color blend):\n'
  '  // this target has no EFB alpha, and a destination-alpha mix by a\n'
  '  // non-receiver (composition_mode 2) must keep the receiver share.\n'
  '  contributionBlend.alpha={.srcFactor=wgpu::BlendFactor::Zero,.dstFactor=wgpu::BlendFactor::Zero};\n'
  '  for(uint32_t i=0;i<layout.colorAttachmentCount;++i){const auto semantic=layout.colorAttachments[i].semantic;\n'
  '    if(config.shaderConfig.enhancementMask && (semantic==gfx::ColorAttachmentSemantic::Normal || semantic==gfx::ColorAttachmentSemantic::SceneCapture)){\n'
  '      colorTargets[i].blend=&metadataBlend;colorTargets[i].writeMask=options.colorUpdate?wgpu::ColorWriteMask::All:wgpu::ColorWriteMask::None;\n'
  '    }else if(semantic==gfx::ColorAttachmentSemantic::SceneContribution){\n'
  '      colorTargets[i].blend=config.shaderConfig.enhancedComposition==4?nullptr:&contributionBlend;\n'
  '      colorTargets[i].writeMask=options.colorUpdate && config.shaderConfig.enhancedComposition!=5?wgpu::ColorWriteMask::All:wgpu::ColorWriteMask::None;\n'
  '    }\n'
  '  }\n'
  '  colorTargets[gfx::SceneColorAttachmentIndex].blend = &blendState;')])
edit('lib/gx/shader.cpp',[
 ('  if(config.enhancementMask && config.enhancedAlphaBlend)fragmentFn += "\\n    let gekkoaot_opaque = prev.a >= 1.0;";',
  '  if(config.enhancementMask)fragmentFn += "\\n    let gekkoaot_alpha=prev.a;";\n'
  '  if(config.enhancementMask && config.enhancedAlphaBlend)fragmentFn += "\\n    let gekkoaot_opaque = prev.a >= 1.0;";'),
 ('fragmentOutput.insert(end,fmt::format("    @location({}) capture:f32,\\n",normalAttachment+1));',
  'fragmentOutput.insert(end,fmt::format("    @location({}) capture:vec4f,\\n    @location({}) contribution:vec4f,\\n",normalAttachment+1,normalAttachment+2));'),
 ('    fragmentReturn.insert(ret,"out.capture = 0.0;\\n    if(out.normal.w > 0.99 && in.capture_z > 0.0 && ubuf.enhanced_view.x > 0u){"\n'
  '      "out.capture=bitcast<f32>((bitcast<u32>(in.capture_z)&0xfffffff0u)|ubuf.enhanced_view.x); }\\n    ");',
  '    const unsigned mode=config.enhancedComposition;\n'
  '    const std::string replace=mode==1?"gekkoaot_alpha >= 1.0":mode==2||mode==3||mode==5?"false":"true";\n'
  '    std::string code="let capture_valid=out.normal.w > 0.99 && in.capture_z > 0.0 && ubuf.enhanced_view.x > 0u;\\n"\n'
  '      "var capture_bits=0u; if(capture_valid){capture_bits=(bitcast<u32>(in.capture_z)&0xfffffff0u)|ubuf.enhanced_view.x;}\\n"\n'
  '      "let metadata_write=select(0.0,1.0,"+replace+");\\n"\n'
  '      "out.capture=vec4f(f32(capture_bits&2047u),f32((capture_bits>>11u)&2047u),f32(capture_bits>>22u),metadata_write);\\n"\n'
  '      "out.normal.w=metadata_write;\\n";\n'
  '    code+=mode==3?"out.contribution=vec4f(prev.rgb,gekkoaot_alpha);\\n":\n'
  '      "out.contribution=vec4f(select(vec3f(0.0),prev.rgb,capture_valid),gekkoaot_alpha);\\n";\n'
  '    fragmentReturn.insert(ret,code);')])
edit('lib/gfx/clear.cpp',[
 ('if(layout.colorAttachments[i].semantic==ColorAttachmentSemantic::SceneCapture){',
  'if(layout.colorAttachments[i].semantic==ColorAttachmentSemantic::SceneCapture || layout.colorAttachments[i].semantic==ColorAttachmentSemantic::SceneContribution){'),
 ('capture{0}:f32,','capture{0}:vec4f,'),
 ('out.capture{} = 0.0;','out.capture{} = vec4f(0.0);'),
 ('if(layout.colorAttachments[i].semantic==ColorAttachmentSemantic::SceneCapture)\n',
  'if(layout.colorAttachments[i].semantic==ColorAttachmentSemantic::SceneCapture || layout.colorAttachments[i].semantic==ColorAttachmentSemantic::SceneContribution)\n')])
edit('lib/gx/pipeline.hpp',[('GXPipelineConfigVersion = 19;','GXPipelineConfigVersion = 20;')])
for p,s in updates.items():p.write_text(s)
