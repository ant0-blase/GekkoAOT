// SPDX-License-Identifier: GPL-3.0-or-later
#include "gekkoaot_shadow_replay.hpp"
#include "gekkoaot_enhanced_renderer.hpp"
#include "../gx/shader_info.hpp"
#include "frame_packet.hpp"
#include "recording.hpp"
#include "resources.hpp"
#include "resource_cache.hpp"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>

namespace aurora::gfx::shadow {
namespace {
std::array<float,16> lightClip{};
unsigned resolution=0;
uint64_t generation=0;
wgpu::RenderPipeline visualize;
wgpu::BindGroupLayout visualizeLayout;
wgpu::BindGroupLayout projectionLayout;
webgpu::TextureWithSampler latest;
uint64_t casterDraws=0, depthPasses=0;
// Sun cascades: atlas reused across passes (encoded one pass at a time).
unsigned cascadeResolution=0;
wgpu::Texture cascadeAtlas,cascadeDummy;
wgpu::TextureView cascadeAtlasView,cascadeDummyView;
uint64_t cascadePasses=0,cascadeDraws=0,cascadePlans=0,cascadeUniforms=0,cascadeRejected=0;
uint64_t cascadeEncodeNs=0;
unsigned maxCasterPlans=0;
gx::PipelineConfig lastCasterConfig{};
PipelineRef lastCasterPipeline{};
bool haveCasterPipeline=false;

void ensure_visualize() {
  if(visualize)return;
  constexpr auto source=R"(
@group(0) @binding(0) var depth: texture_depth_2d;
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
 let p=array<vec2f,3>(vec2f(-1,-1),vec2f(3,-1),vec2f(-1,3));return vec4f(p[i],0,1);
}
@fragment fn fs(@builtin(position) p:vec4f)->@location(0) vec4f {
 let z=textureLoad(depth,vec2i(p.xy),0);return vec4f(vec3f(z),1);
})";
  wgpu::ShaderSourceWGSL wgsl{};wgsl.code=source;
  wgpu::ShaderModuleDescriptor sd{.nextInChain=&wgsl};
  const auto shader=webgpu::g_device.CreateShaderModule(&sd);
  const wgpu::BindGroupLayoutEntry binding{.binding=0,.visibility=wgpu::ShaderStage::Fragment,
    .texture={.sampleType=wgpu::TextureSampleType::Depth,.viewDimension=wgpu::TextureViewDimension::e2D}};
  const wgpu::BindGroupLayoutDescriptor bd{.entryCount=1,.entries=&binding};
  visualizeLayout=webgpu::g_device.CreateBindGroupLayout(&bd);
  const wgpu::PipelineLayoutDescriptor ld{.bindGroupLayoutCount=1,.bindGroupLayouts=&visualizeLayout};
  const auto layout=webgpu::g_device.CreatePipelineLayout(&ld);
  const wgpu::ColorTargetState target{.format=wgpu::TextureFormat::RGBA8Unorm};
  const wgpu::FragmentState fragment{.module=shader,.entryPoint="fs",.targetCount=1,.targets=&target};
  const wgpu::RenderPipelineDescriptor pd{.label="GX shadow depth validation preview",.layout=layout,
    .vertex={.module=shader,.entryPoint="vs"},
    .primitive={.topology=wgpu::PrimitiveTopology::TriangleList},.fragment=&fragment};
  visualize=webgpu::g_device.CreateRenderPipeline(&pd);
}
}

bool validation_view(const float* matrix,unsigned size) {
  if(matrix && (size<16 || size>2048 || (size&(size-1))))return false;
  if(matrix){
    std::array<float,16> candidate{},inverse{};
    std::copy_n(matrix,16,candidate.begin());
    if(!GekkoAOT::GX::InvertProjection(candidate,inverse))return false;
  }
  // A request boundary seals existing work before changing its projection.
  if(!recording_boundary())return false;
  resolution=matrix ? size : 0;
  if(matrix)std::memcpy(lightClip.data(),matrix,sizeof(lightClip));
  ++generation;
  gx::g_gxState.dirty|=gx::DirtyAll;
  return true;
}

namespace {
PipelineRef caster_pipeline(const gx::PipelineConfig& original) {
  auto config=original;
  // Keep every field affecting the GX uniform layout. Only projection and
  // raster state change; the original frame-owned uniform is reused verbatim.
  config.shaderConfig.shadowReplay=1;
  config.shaderConfig.materialEnhancement=0;
  config.shaderConfig.logicOp=GX_LO_COPY;
  config.depthCompare=true;config.depthUpdate=true;config.depthFunc=GX_LEQUAL;
  config.colorUpdate=false;config.alphaUpdate=false;config.blendMode=GX_BM_NONE;
  // The draw's own cull mode, evaluated from the sun: closed objects cast
  // with the faces turned to the light, and an enclosing sky dome (its inner
  // faces turned away from a sun above) does not shadow the scene inside it.
  config.dstAlpha=UINT32_MAX;
  // Consecutive draws mostly share their pipeline configuration.
  if(haveCasterPipeline && std::memcmp(&config,&lastCasterConfig,sizeof(config))==0)return lastCasterPipeline;
  RenderTargetLayout layout{.depthStencilFormat=wgpu::TextureFormat::Depth32Float,.sampleCount=1};
  detail::finalize_render_target_layout(layout);
  lastCasterConfig=config;lastCasterPipeline=find_pipeline(config,layout);haveCasterPipeline=true;
  return lastCasterPipeline;
}
// The original draw owns vertices, skinning matrices, textures and uniforms.
// Each cascade supplies just one light projection in a shared uniform.
void prepare_cascade(gx::DrawData& draw,const gx::PipelineConfig& original,detail::RenderPass& pass) {
  if(!original.shaderConfig.enhancedEligible || original.shaderConfig.lineMode ||
     original.shaderConfig.zTexOp!=GX_ZT_DISABLE || !draw.uniformRange.size ||
     enhanced::view_model_draw())return;
  const auto pipeline=caster_pipeline(original);
  if(!pipeline)return;
  pass.shadowPlans.push_back({.pipeline=pipeline,.uniform=draw.uniformRange,.cascade=true,
    .view=enhanced::current_view_id()});
  draw.shadowIndex=static_cast<uint32_t>(pass.shadowPlans.size());
}
}

void arm_cascades(unsigned size) noexcept {cascadeResolution=size;}
bool cascades_armed() noexcept {return cascadeResolution!=0;}

wgpu::BindGroupLayout projection_layout() {
  // Initialized by GX before pipeline workers start; released after they join.
  if(!projectionLayout){
    const wgpu::BindGroupLayoutEntry e{.binding=0,.visibility=wgpu::ShaderStage::Vertex,
      .buffer={.type=wgpu::BufferBindingType::Uniform,.hasDynamicOffset=true,.minBindingSize=64}};
    const wgpu::BindGroupLayoutDescriptor d{.label="GX shadow projection",.entryCount=1,.entries=&e};
    projectionLayout=webgpu::g_device.CreateBindGroupLayout(&d);
  }
  return projectionLayout;
}

namespace {
BindGroupRef projection_binding() {
  const WGPUBindGroupEntry e{.binding=0,.buffer=detail::resources().uniformBuffer.Get(),.size=64};
  const WGPUBindGroupDescriptor d{.label={"GX shared shadow projection",WGPU_STRLEN},
    .layout=projection_layout().Get(),.entryCount=1,.entries=&e};
  return bind_group_ref(d);
}
}

std::array<unsigned,16> caster_counts(const detail::RenderPass& pass) noexcept {
  std::array<unsigned,16> draws{};
  for(const auto& plan:pass.shadowPlans)if(plan.cascade && plan.view<draws.size())++draws[plan.view];
  return draws;
}

unsigned finalize_cascades(detail::RenderPass& pass,const std::array<float,16>* clip,unsigned count,uint32_t views,bool loadPrevious) {
  pass.shadowCascades=0;
  pass.shadowLoadPrevious=loadPrevious;
  if(resolution || !count || count>2 || pass.shadowPlans.empty())return 0;
  unsigned casters=0;
  for(auto& plan:pass.shadowPlans){
    if(!plan.cascade)continue;
    plan.selected=plan.view && (views>>(plan.view-1)&1u);
    if(plan.selected)++casters;else ++cascadeRejected;
  }
  for(unsigned c=0;c<count;++c)pass.shadowProjection[c]=push_uniform(clip[c]);
  cascadeUniforms+=count;
  cascadePlans+=casters;
  maxCasterPlans=std::max(maxCasterPlans,casters);
  if(casters)pass.shadowCascades=count;
  pass.shadowResolution=cascadeResolution;
  return casters;
}

namespace {
void encode_cascades(const wgpu::CommandEncoder& cmd,detail::RenderPass& info) {
  static const bool measure=std::getenv("GEKKOAOT_ENHANCED_MEASURE")!=nullptr;
  const auto started=measure?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
  const unsigned size=info.shadowResolution,count=info.shadowCascades;
  bool loadPrevious=info.shadowLoadPrevious;
  if(!cascadeAtlas || cascadeAtlas.GetWidth()!=size*count || cascadeAtlas.GetHeight()!=size){
    loadPrevious=false;
    const wgpu::TextureDescriptor td{.label="GX sun cascade atlas",
      .usage=wgpu::TextureUsage::RenderAttachment|wgpu::TextureUsage::TextureBinding,
      .size={size*count,size,1},.format=wgpu::TextureFormat::Depth32Float};
    cascadeAtlas=webgpu::g_device.CreateTexture(&td);cascadeAtlasView=cascadeAtlas.CreateView();
  }
  const wgpu::RenderPassDepthStencilAttachment depth{.view=cascadeAtlasView,
    .depthLoadOp=loadPrevious?wgpu::LoadOp::Load:wgpu::LoadOp::Clear,.depthStoreOp=wgpu::StoreOp::Store,.depthClearValue=0.f};
  const wgpu::RenderPassDescriptor pd{.label="GX sun cascade replay",.depthStencilAttachment=&depth};
  auto pass=cmd.BeginRenderPass(&pd);
  // As the EFB pass encoder: untextured draws never bind group 2 themselves.
  pass.SetBindGroup(0,detail::resources().staticBindGroup);pass.SetBindGroup(2,gx::g_emptyTextureBindGroup);
  gx::reset_native_bind_group_cache();
  const auto binding=projection_binding();
  for(unsigned c=0;c<count;++c){
    pass.SetViewport(float(c*size),0,float(size),float(size),0,1);pass.SetScissorRect(c*size,0,size,size);
    for(const auto& command:info.commands){
      if(command.type!=detail::CommandType::Draw)continue;
      const auto* original=gx_payload(command.data.draw);
      if(!original || !original->shadowIndex || original->shadowIndex>info.shadowPlans.size())continue;
      const auto& plan=info.shadowPlans[original->shadowIndex-1];
      if(!plan.selected)continue;
      auto replay=*original;replay.pipeline=plan.pipeline;replay.uniformRange=plan.uniform;
      replay.logicDestination=binding;replay.shadowProjectionOffset=info.shadowProjection[c].offset;
      gx::render(replay,pass);++cascadeDraws;
    }
  }
  pass.End();gx::reset_native_bind_group_cache();++cascadePasses;
  if(measure)cascadeEncodeNs+=std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now()-started).count();
}
}

wgpu::TextureView cascade_view(const detail::RenderPass& pass) {
  if(pass.shadowCascades && cascadeAtlasView)return cascadeAtlasView;
  if(!cascadeDummy){
    const wgpu::TextureDescriptor td{.label="GX sun cascade none",
      .usage=wgpu::TextureUsage::RenderAttachment|wgpu::TextureUsage::TextureBinding,
      .size={1,1,1},.format=wgpu::TextureFormat::Depth32Float};
    cascadeDummy=webgpu::g_device.CreateTexture(&td);cascadeDummyView=cascadeDummy.CreateView();
  }
  return cascadeDummyView;
}

namespace {
void prepare_validation(gx::DrawData& draw,const gx::PipelineConfig& original,detail::RenderPass& pass);
}
void prepare(gx::DrawData& draw,const gx::PipelineConfig& original,detail::RenderPass& pass) {
  if(resolution)prepare_validation(draw,original,pass);
  else if(cascadeResolution)prepare_cascade(draw,original,pass);
}
namespace {
void prepare_validation(gx::DrawData& draw,const gx::PipelineConfig& original,detail::RenderPass& pass) {
  if(!original.shaderConfig.enhancedEligible ||
     original.shaderConfig.lineMode || original.shaderConfig.zTexOp!=GX_ZT_DISABLE ||
     !draw.uniformRange.size)return;
  const auto pipeline=caster_pipeline(original);
  if(!pipeline)return;
  if(pass.shadowPlans.empty()){
    pass.shadowGeneration=generation;pass.shadowResolution=resolution;
    pass.shadowProjection[0]=push_uniform(lightClip);
  }
  if(pass.shadowGeneration!=generation)return;
  pass.shadowPlans.push_back({.pipeline=pipeline,.uniform=draw.uniformRange});
  draw.shadowIndex=static_cast<uint32_t>(pass.shadowPlans.size());
}
}

namespace { bool frameReplay=false; }
void set_frame_replay(bool replaying) noexcept {frameReplay=replaying;}
bool frame_replay() noexcept {return frameReplay;}
void encode(const wgpu::CommandEncoder& cmd,detail::RenderPass& info) {
  if(frameReplay && info.shadowCascades)return;
  if(info.shadowPlans.empty())return;
  if(info.shadowCascades){encode_cascades(cmd,info);return;}
  if(info.shadowPlans.front().cascade)return; // unsealed cascade plans
  const unsigned size=info.shadowResolution;
  const wgpu::TextureDescriptor td{.label="GX geometry shadow validation depth",
    .usage=wgpu::TextureUsage::RenderAttachment|wgpu::TextureUsage::TextureBinding,
    .size={size,size,1},.format=wgpu::TextureFormat::Depth32Float};
  const auto texture=webgpu::g_device.CreateTexture(&td);
  const auto depthView=texture.CreateView();
  const wgpu::RenderPassDepthStencilAttachment depth{.view=depthView,
    .depthLoadOp=wgpu::LoadOp::Clear,.depthStoreOp=wgpu::StoreOp::Store,.depthClearValue=0.f};
  const wgpu::RenderPassDescriptor pd{.label="GX geometric shadow caster replay",.depthStencilAttachment=&depth};
  auto pass=cmd.BeginRenderPass(&pd);
  pass.SetViewport(0,0,size,size,0,1);pass.SetScissorRect(0,0,size,size);
  // As the EFB pass encoder: untextured draws never bind group 2 themselves.
  pass.SetBindGroup(0,detail::resources().staticBindGroup);pass.SetBindGroup(2,gx::g_emptyTextureBindGroup);
  gx::reset_native_bind_group_cache();
  const auto projectionBinding=projection_binding();
  for(const auto& command:info.commands){
    if(command.type!=detail::CommandType::Draw)continue;
    const auto* original=gx_payload(command.data.draw);
    if(!original || !original->shadowIndex || original->shadowIndex>info.shadowPlans.size())continue;
    const auto& plan=info.shadowPlans[original->shadowIndex-1];
    auto replay=*original;replay.pipeline=plan.pipeline;replay.uniformRange=plan.uniform;
    replay.logicDestination=projectionBinding;replay.shadowProjectionOffset=info.shadowProjection[0].offset;
    gx::render(replay,pass);++casterDraws;
  }
  pass.End();gx::reset_native_bind_group_cache();++depthPasses;
  ensure_visualize();
  auto previewDesc=td;previewDesc.label="GX shadow depth validation image";
  previewDesc.format=wgpu::TextureFormat::RGBA8Unorm;
  auto output=webgpu::g_device.CreateTexture(&previewDesc);
  const auto outputView=output.CreateView();
  const wgpu::BindGroupEntry entry{.binding=0,.textureView=depthView};
  const wgpu::BindGroupDescriptor bd{.layout=visualizeLayout,.entryCount=1,.entries=&entry};
  const auto binding=webgpu::g_device.CreateBindGroup(&bd);
  const wgpu::RenderPassColorAttachment color{.view=outputView,.loadOp=wgpu::LoadOp::Clear,.storeOp=wgpu::StoreOp::Store};
  const wgpu::RenderPassDescriptor cd{.label="GX shadow depth validation visualize",.colorAttachmentCount=1,.colorAttachments=&color};
  auto preview=cmd.BeginRenderPass(&cd);preview.SetPipeline(visualize);preview.SetBindGroup(0,binding);preview.Draw(3);preview.End();
  latest={.texture=output,.view=outputView,.size=td.size,.format=previewDesc.format,
    .sampler=webgpu::g_frameBuffer.sampler};
}

const webgpu::TextureWithSampler& preview(const webgpu::TextureWithSampler& source) {
  return resolution && latest.texture ? latest : source;
}
void reset(){
  if(cascadePasses)std::fprintf(stderr,"GEKKOAOT_SUN_CASCADE_STATS passes=%llu caster_draws=%llu caster_plans=%llu "
    "shared_projection_uniforms=%llu shared_projection_bytes=%llu copied_draw_uniform_bytes=0 "
    "max_caster_plans=%u encode_cpu_ns=%llu other_camera_uniforms=%llu\n",
    (unsigned long long)cascadePasses,(unsigned long long)cascadeDraws,
    (unsigned long long)cascadePlans,(unsigned long long)cascadeUniforms,
    (unsigned long long)(64*cascadeUniforms),maxCasterPlans,(unsigned long long)cascadeEncodeNs,
    (unsigned long long)cascadeRejected);
  cascadePasses=cascadeDraws=cascadePlans=cascadeUniforms=cascadeRejected=0;
  cascadeEncodeNs=0;maxCasterPlans=0;
  cascadeAtlas={};cascadeAtlasView={};cascadeDummy={};cascadeDummyView={};haveCasterPipeline=false;
  projectionLayout={};
  if(depthPasses)std::fprintf(stderr,"GEKKOAOT_SHADOW_REPLAY_STATS passes=%llu caster_draws=%llu scope=explicit-view-depth-validation\n",
    (unsigned long long)depthPasses,(unsigned long long)casterDraws);
  resolution=0;latest={};visualize={};visualizeLayout={};depthPasses=casterDraws=0;
}
}
