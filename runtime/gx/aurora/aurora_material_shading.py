"""Native material maps, tangent-space normals and guarded POM in GX shaders."""
from pathlib import Path
import sys
root=Path(sys.argv[1]);marker='GEKKOAOT_MATERIAL_SHADING_V1'

def edit(name,changes):
    p=root/name;s=p.read_text()
    if marker in s:return
    for old,new,count in changes:
        if s.count(old)!=count:raise RuntimeError(f'material shading {name}: anchor changed {old[:100]}')
        s=s.replace(old,new)
    p.write_text('// '+marker+'\n'+s)

edit('lib/gx/gx.hpp',[
    ('struct ShaderConfig {','struct ShaderConfig {\n  u32 materialEnhancement = 0;',1),
    ('struct ShaderInfo {','struct ShaderInfo {\n  bool materialEnhancement = false;',1),
])
edit('lib/gx/pipeline.hpp',[('GXPipelineConfigVersion = 24;','GXPipelineConfigVersion = 25;',1)])
edit('lib/gx/command_processor.cpp',[
    ('#include "../gfx/gekkoaot_shadow_replay.hpp"','#include "../gfx/gekkoaot_shadow_replay.hpp"\n#include "../gfx/gekkoaot_hd_resources.hpp"',1),
    ('  hash.update(&fragmentLighting,sizeof(fragmentLighting));',
     '  hash.update(&fragmentLighting,sizeof(fragmentLighting));\n'
     '  const bool material=gfx::hd::material_enabled();hash.update(&material,sizeof(material));\n'
     '  const bool keepsReceivers=gfx::enhanced::keeps_receivers();hash.update(&keepsReceivers,sizeof(keepsReceivers));',1),
    ('  const bool pipelineValid = cache.hasPipeline && (state.dirty & DirtyPipeline) == 0 && cache.fmt == fmt &&',
     '  if (gfx::enhanced::keeps_receivers_changed()) state.dirty |= DirtyPipeline;\n'
     '  const bool pipelineValid = cache.hasPipeline && (state.dirty & DirtyPipeline) == 0 && cache.fmt == fmt &&',1),
])
edit('lib/gx/gx.cpp',[
    ('#include "gx.hpp"','#include "gx.hpp"\n#include "../gfx/gekkoaot_hd_resources.hpp"',1),
    ('  config.shaderConfig.enhancedEligible = gfx::enhanced::eligible();',
     '  config.shaderConfig.enhancedEligible = gfx::enhanced::eligible();\n'
     '  config.shaderConfig.materialEnhancement=gfx::hd::material_enabled() && config.shaderConfig.enhancedEligible &&\n'
     '    g_gxState.numTexGens==1 && g_gxState.numIndStages==0 && g_gxState.vtxDesc[GX_VA_PNMTXIDX]==GX_NONE &&\n'
     '    g_gxState.tevStages[0].texMapId==GX_TEXMAP0 && g_gxState.tevStages[0].texCoordId==GX_TEXCOORD0 &&\n'
     '    g_gxState.tcgs[0].src==GX_TG_TEX0 && primitive!=GX_LINES && primitive!=GX_LINESTRIP && primitive!=GX_POINTS;',1),
    ('std::array<WGPUBindGroupEntry, MaxTextures * 2> textureEntries{};',
     'std::array<WGPUBindGroupEntry, MaxTextures * 2 + 1> textureEntries{};',1),
    ('  const WGPUBindGroupDescriptor textureBindGroupDescriptor{',
     '  const auto& material=gfx::hd::bound_material();\n'
     '  textureEntries[MaxTextures*2]={.binding=MaxTextures*2,\n'
     '    .textureView=material.texture?material.texture->sampleTextureView.Get():sEmptyTextureView.Get()};\n'
     '  const WGPUBindGroupDescriptor textureBindGroupDescriptor{',1),
    ('std::array<wgpu::BindGroupLayoutEntry, MaxTextures * 2> textureEntries;',
     'std::array<wgpu::BindGroupLayoutEntry, MaxTextures * 2 + 1> textureEntries;',1),
    ('    const wgpu::BindGroupLayoutDescriptor descriptor{\n        .label = "GX Texture Bind Group Layout",',
     '    textureEntries[MaxTextures*2]={.binding=MaxTextures*2,.visibility=wgpu::ShaderStage::Fragment,\n'
     '      .texture={.sampleType=wgpu::TextureSampleType::Float,.viewDimension=wgpu::TextureViewDimension::e2D}};\n'
     '    const wgpu::BindGroupLayoutDescriptor descriptor{\n        .label = "GX Texture Bind Group Layout",',1),
    ('std::array<wgpu::BindGroupEntry, MaxTextures * 2> entries;',
     'std::array<wgpu::BindGroupEntry, MaxTextures * 2 + 1> entries;',1),
    ('    const wgpu::BindGroupDescriptor desc{\n        .label = "GX Empty Texture Bind Group",',
     '    entries[MaxTextures*2]={.binding=MaxTextures*2,.textureView=sEmptyTextureView};\n'
     '    const wgpu::BindGroupDescriptor desc{\n        .label = "GX Empty Texture Bind Group",',1),
])
edit('lib/gx/texture.cpp',[
    ('void resolve_sampled_textures(const ShaderInfo& info) noexcept {',r'''
static void resolve_material_for_draw(const ShaderInfo& info) {
  gfx::hd::bind_material({});
  if(!info.materialEnhancement)return;
  const auto& obj=g_gxState.loadedTextures[0];
  if(!obj.has_data() || g_gxState.copyTextures.contains(obj.data))return;
  const GXTlutObj_* palette=nullptr;
  if(is_palette_format(obj.format())){
    if(obj.tlut>=g_gxState.loadedTluts.size())return;
    palette=&g_gxState.loadedTluts[obj.tlut];if(!palette->data)return;
  }
  const auto keys=hash_texture_source(obj,palette,true);
  if(!keys.sourceKey)return;
  // User packs retain priority; no original-data inference is scheduled for them.
  if(gfx::texture_replacement::find_pointer_replacement(obj) ||
     gfx::texture_replacement::find_source_replacement(obj,*keys.sourceKey))return;
  gfx::hd::bind_material(gfx::hd::request_material(obj,*keys.sourceKey,palette));
}
void resolve_sampled_textures(const ShaderInfo& info) noexcept {''',1),
    ('} // namespace aurora::gx','} // namespace aurora::gx',1),
])
# The resolver runs once per binding update, after the ordinary texture loop.
p=root/'lib/gx/texture.cpp';s=p.read_text()
tail='    textureBind = gfx::TextureBind{obj, std::move(handle), s_bindGeneration};\n  }\n}'
if '  resolve_material_for_draw(info);' not in s:
    if s.count(tail)!=1:raise RuntimeError('material texture resolver tail changed')
    p.write_text(s.replace(tail,tail[:-1]+'  resolve_material_for_draw(info);\n}'))

edit('lib/gx/shader_info.cpp',[
    ('#include "shader_info.hpp"','#include "shader_info.hpp"\n#include "../gfx/gekkoaot_hd_resources.hpp"',1),
    ('  if(config.enhancementMask)info.uniformSize+=16;',
     '  if(config.enhancementMask)info.uniformSize+=16;\n'
     '  info.materialEnhancement=config.materialEnhancement && info.sampledTextures.count()==1 &&\n'
     '    info.sampledTextures[0] && info.sampledTexCoords[0] && info.sampledIndTextures.none() &&\n'
     '    gfx::align_uniform(info.uniformSize+32)<=MaxUniformSize;\n'
     '  if(info.materialEnhancement)info.uniformSize+=32;',1),
    ('    buf.append(texture_size_bias(get_texture(static_cast<GXTexMapID>(i))));\n  }\n}',
     '    buf.append(texture_size_bias(get_texture(static_cast<GXTexMapID>(i))));\n  }\n'
     '  if(info.materialEnhancement){const auto& m=gfx::hd::bound_material();const auto& p=m.parameters;const auto& d=m.detail;\n'
     '    buf.append(Vec4{p[0],p[1],p[2],p[3]});buf.append(Vec4{d[0],d[1],d[2],d[3]});}\n}',1),
])

edit('lib/gx/shader.cpp',[
    ('#include "shader_info.hpp"','#include "shader_info.hpp"\n#include "../gfx/gekkoaot_material_shaders.hpp"',1),
    ('  std::string fragmentFnPre;','  std::string fragmentFnPre;\n  std::string deferredMaterialLighting;\n  bool materialUsed=false;',1),
    ('  uniBufAttrs += "\\n    proj: mat4x4f,";',
     '  if(info.materialEnhancement){\n'
     '    vtxOutAttrs+=fmt::format("\\n    @location({}) material_pos:vec3f,",vtxOutIdx++);\n'
     '    vtxXfrAttrsPre+="\\n    out.material_pos=mv_pos;";\n'
     '  }\n  uniBufAttrs += "\\n    proj: mat4x4f,";',1),
    ('let fragment_normal = in.mv_nrm','var fragment_normal = in.mv_nrm',1),
    ('      fragmentFnPre += lighting_func(config, cc, i, false);\n      fragmentFnPre += lighting_func(config, cca, i, true);',
     '      auto& lighting=info.materialEnhancement?deferredMaterialLighting:fragmentFnPre;\n'
     '      lighting += lighting_func(config, cc, i, false);\n      lighting += lighting_func(config, cca, i, true);',1),
    ('    fragmentFnPre +=\n        fmt::format("\\n    var sampled{0} = textureSampleBias(tex{1}, tex{1}_samp, {2}, ubuf.tex{1}_size_bias.z);", i,\n                    underlying(stage.texMapId), uvIn);',r'''
    if(info.materialEnhancement && i==0){
      materialUsed=true;
      fragmentFnPre+=fmt::format("\n    let material_base_uv={};",uvIn);
      fragmentFnPre+="\n    let material_active=ubuf.material_params.w>0. && (ubuf.material_params.x>=0. || in.pos.x>=ubuf.render_viewport_size.x*.5);let material_dx=dpdx(material_base_uv);let material_dy=dpdy(material_base_uv);";
      fragmentFnPre+=useNormalTarget?"\n    let material_smooth=in.capture_nrm;":"\n    let material_smooth=vec3f(0.);";
      fragmentFnPre+="\n    let material_basis=material_frame(in.material_pos,material_base_uv,material_smooth);"
        "\n    let material_uv=select(material_base_uv,material_parallax(material_base_uv,material_dx,material_dy,in.material_pos,material_basis),material_active);"
        "\n    let material_data=textureSampleGrad(material_map,tex0_samp,material_uv,material_dx,material_dy);"
        "\n    let material_micro=material_micro_detail(material_uv,material_dx,material_dy);"
        "\n    let material_normal=material_surface(material_data,material_uv,material_basis,material_micro);";
      if(config.fragmentLighting && info.lightingEnabled)
        fragmentFnPre+="\n    fragment_normal=normalize(mix(fragment_normal,material_normal,"
          "smoothstep(.75,.95,dot(fragment_normal,material_basis.n))*select(0.,1.,material_active)));";
      fragmentFnPre+=fmt::format("\n    var sampled{}=textureSampleGrad(tex0,tex0_samp,material_uv,"
        "material_dx*exp2(ubuf.tex0_size_bias.z),material_dy*exp2(ubuf.tex0_size_bias.z));",i);
      fragmentFnPre+=fmt::format("\n    sampled{0}=vec4f(sampled{0}.rgb*(1.+select(0.,(material_micro.x+material_micro.y)*ubuf.material_detail.y,material_active)),sampled{0}.a);",i);
    }else fragmentFnPre +=
        fmt::format("\n    var sampled{0} = textureSampleBias(tex{1}, tex{1}_samp, {2}, ubuf.tex{1}_size_bias.z);", i,
                    underlying(stage.texMapId), uvIn);''',1),
    ('  if (info.usesPTTexMtx.any()) {','  fragmentFnPre+=deferredMaterialLighting;\n  if (info.usesPTTexMtx.any()) {',1),
    ('  if (!prevColorNormalized && !prevAlphaNormalized) {',
     '  if(info.materialEnhancement){uniBufAttrs+="\\n    material_params:vec4f,";texBindings+=gfx::hd::MaterialShader;}\n'
     '  if(info.materialEnhancement)uniBufAttrs+="\\n    material_detail:vec4f,";\n'
     '  if (!prevColorNormalized && !prevAlphaNormalized) {',1),
    ('    code+="let gx_normal=out.normal.xyz*2.-1.;\\n"',
     '    if(materialUsed)code+="if(material_active){out.normal.xyz=material_normal*.5+.5;}\\n";\n'
     '    code+="let gx_normal=out.normal.xyz*2.-1.;\\n"',1),
    ('      "let material_code=("+std::to_string(material)+".+select(0.,4.,dot(gx_normal,gx_normal)>.25))/7.;\\n"',
     '      "var surface_roughness="+std::to_string(material==2?.35f:material==3?.16f:1.f)+";\\n";\n'
     '    if(materialUsed && material==1)code+="if(material_active){surface_roughness=material_data.b;}\\n";\n'
     '    code+="let material_code=(f32(u32(round(surface_roughness*127.))<<3u)+"+std::to_string(material)+\n'
     '      ".+select(0.,4.,dot(gx_normal,gx_normal)>.25))/1023.;\\n"',1),
])
