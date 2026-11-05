// SPDX-License-Identifier: GPL-3.0-or-later
#include "../gx/gx.hpp"
#include "../webgpu/gpu_prof.hpp"
#include "frame_packet.hpp"
#include "gekkoaot_enhanced_renderer.hpp"
#include "gekkoaot_shadow_replay.hpp"
#include "gekkoaot_enhanced_shaders.hpp"
#include "gekkoaot_enhanced_measurement.hpp"
#include "gekkoaot_hd_resources.hpp"
#include "gekkoaot_background_geometry.h"
#include "gekkoaot_particle_radiance.hpp"
#include "gekkoaot_render_graph.h"
#include "gekkoaot_sun_cascades.h"
#include "recording.hpp"
#include "resources.hpp"
#include "tex_copy_conv.hpp"
#include "texture.hpp"
#include <atomic>
#include <bit>
#include <cstdio>
#include <chrono>
#include <fstream>
#include <string>
#include <unordered_map>
extern "C" bool gekkoaot_native_gx_screen_fill() noexcept;
extern "C" bool gekkoaot_native_gx_texture_viewport(float,float,float,float) noexcept;
extern "C" bool gekkoaot_native_gx_perspective_ui() noexcept;
namespace aurora::gfx::enhanced {
using namespace GekkoAOT::GX;
namespace {
GraphicsSettings settings;
SceneResourceHistory resourceHistory;
// Environment lighting: world frame and sun source of the receivers recorded
// since the previous scene pass (see gekkoaot_environment_lighting.h).
EnvironmentSettings environment;
EnvironmentFrame environmentFrame;
WorldFrameTracker environmentTracker;
std::array<gx::Light,::GX::MaxLights> observedLights{};
uint32_t observedLightMask=~0u;
uint64_t environmentPasses=0,environmentWorld=0,environmentCameraLights=0,environmentSources[3]{};
uint64_t environmentDraws=0,environmentSkinned=0,environmentNotOpaque=0,tracedDraws=0,tracedSkinned=0,tracedNotOpaque=0;
struct TextureLifetime {std::weak_ptr<TextureRef> texture;uint64_t id=0,age=0;};
std::unordered_map<const TextureRef*,TextureLifetime> textureLifetimes;
uint64_t nextResourceId=0,resourceClock=0,lastEfbPass=0;
wgpu::Texture lastEfbTexture;
uint64_t resource_id(const TextureHandle& texture) {
  if(!texture)return 0;
  auto it=textureLifetimes.find(texture.get());
  if(it!=textureLifetimes.end() && !it->second.texture.expired()){
    it->second.age=++resourceClock;return it->second.id;
  }
  if(it!=textureLifetimes.end())textureLifetimes.erase(it);
  if(textureLifetimes.size()>=SceneResourceHistory::Capacity){
    auto oldest=textureLifetimes.begin();
    for(auto e=textureLifetimes.begin();e!=textureLifetimes.end();++e)
      if(e->second.texture.expired()){oldest=e;break;}
      else if(e->second.age<oldest->second.age)oldest=e;
    textureLifetimes.erase(oldest);
  }
  const auto id=++nextResourceId;
  textureLifetimes.emplace(texture.get(),TextureLifetime{texture,id,++resourceClock});return id;
}
// Enhanced twins of texture copies, keyed by the copy texture. The weak
// reference rejects a recycled allocation that happens to reuse the address.
struct CopyAlias {std::weak_ptr<TextureRef> copy;TextureHandle alias;uint32_t revision=0;bool composed=false;};
// Counts capture clears: while it is unchanged, later passes continue the
// same scene (its receivers are still in the capture).
uint64_t captureEpoch=0;
SceneCapture lastCascadeCamera{};
bool haveCascadeCamera=false;
// Sun cascades of the last scene pass with casters. A pass that continues
// that scene (capture not cleared) composes it again and needs the same
// cascades: Tales of Symphonia (scene, depth-of-field redraw, presentation),
// Medal of Honor (world, then the weapon with its own camera one copy
// later). The atlas still holds them while no other pass encodes it.
struct CarriedCascades {
  uint64_t frame=~0ull,epoch=0;unsigned count=0,resolution=0,casters=0;float near=0;SceneCapture camera{};
  std::array<std::array<float,16>,MaxSunCascades> clip{};std::array<std::array<float,4>,MaxSunCascades> params{};
  std::array<float,4> info{};
} carriedCascades;
std::unordered_map<const TextureRef*,CopyAlias> copyAliases;
unsigned aliasBoundState=0;
// Frame of the last draw that needed a lit twin: composing one re-runs the whole
// graph on a pass that is not displayed (ToS: GPU 82% with it, 43% without).
uint64_t aliasWantedFrame=0;
std::atomic<bool> colorPeeksSeen{false};
uint64_t aliasBindings=0,aliasComposites=0,aliasFallbacks=0;
std::array<float, 16> lastProjection{};
bool haveProjection = false;
bool cameraBackground=false;
EyeCentredPrelude backgroundPrelude;
struct ParticleEmitter {std::array<float,4> position{};float near=0,focal=0;std::array<float,4> viewport{};bool alphaOver=false;unsigned cell=0;uint64_t order=0;uint32_t radianceKey=0;std::array<float,4> color{},raster{};uint64_t draw=0;};
std::array<ParticleEmitter,64> particleEmitters{};
std::span<const std::uint8_t> originalGeometry;
unsigned particleEmitterCount=0;
uint64_t particleEmitterFrame=~0ull;
uint64_t particleEmitterEpoch=~0ull;
uint64_t particleEmitterOrder=0;
uint64_t particleDrawId=0;
bool particleMaterialPending=false;

SceneCapture cachedCapture;
unsigned previousFlags = 0,currentView=0,lastDiagnosticReason=0;
// Optional laboratory control. Normal launches read only the environment;
// opted-in captures can switch diagnostics without restarting a game/menu.
unsigned parse_view(std::string_view view) noexcept {
  return view=="ao"?1u:view=="coverage"?2u:view=="depth"?3u:view=="split"?4u:
    view=="normals"?5u:view=="cameras"?6u:view=="environment"?7u:view=="environment-split"?8u:view=="cascades"?9u:view=="shadow-split"?10u:view=="indirect"?11u:view=="reflections"?12u:view=="cascade-atlas"?13u:0u;
}
std::atomic<unsigned>& diagnostic_view() noexcept {
  static std::atomic<unsigned> mode{[]{const char* v=std::getenv("GEKKOAOT_ENHANCED_VIEW");return v?parse_view(v):0u;}()};
  return mode;
}
unsigned view_mode() noexcept {return diagnostic_view().load(std::memory_order_relaxed);}
void poll_diagnostic_view() {
  static const std::string path=[]{const char* p=std::getenv("GEKKOAOT_ENHANCED_VIEW_FILE");return p?std::string(p):std::string{};}();
  if(path.empty())return;
  static uint64_t lastFrame=~uint64_t(0);const auto frame=gekkoaot_recording_frame_id();
  if(lastFrame==frame)return;lastFrame=frame;
  static std::chrono::steady_clock::time_point last;
  const auto now=std::chrono::steady_clock::now();
  if(now-last<std::chrono::milliseconds(250))return;last=now;
  std::ifstream input(path);std::string value;if(!std::getline(input,value))return;
  const unsigned mode=parse_view(value);
  if(diagnostic_view().exchange(mode,std::memory_order_relaxed)!=mode){
    gx::g_gxState.dirty|=gx::DirtyUniform;
    std::fprintf(stderr,"GEKKOAOT_ENHANCED_VIEW_LIVE mode=%u source=diagnostic-file\n",mode);
  }
}
std::array<float,12> prepassCamera{};
std::uint64_t prepassFrame=0;
bool havePrepass=false;
std::array<float,12> camera_key() noexcept {
  const auto& s=gx::g_gxState;const auto& p=s.proj;const auto& v=s.renderViewport;
  return {p.m0[0],p.m0[2],p.m1[1],p.m1[2],p.m2[2],p.m2[3],
    v.left,v.top,v.width,v.height,v.znear,v.zfar};
}
struct Statistics {
  uint64_t graphs = 0, missing = 0, invalid = 0, mixed = 0, evictions = 0,
           eligible = 0, excluded = 0, encodeCalls=0, encodeCpuNs=0,
           perspective=0, scenePasses=0, sealedPasses=0, maxCameras=0,
           aoPasses=0, contactPasses=0, presents=0, enhancedPresents=0;
  std::array<uint64_t,16> reasons{};
} stats;
struct EncodeTimer {
  static bool enabled() {static const bool value=std::getenv("GEKKOAOT_ENHANCED_PROFILE")!=nullptr;return value;}
  std::chrono::steady_clock::time_point begin{};
  EncodeTimer(){if(enabled())begin=std::chrono::steady_clock::now();}
  ~EncodeTimer(){if(enabled()){++stats.encodeCalls;stats.encodeCpuNs+=
    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();}}
};
struct Target {
  wgpu::Texture texture;
  wgpu::TextureView view;
};
struct Entry {
  wgpu::Texture source;
  webgpu::TextureWithSampler output;
  Target depth, ao, blurX, blurY, gi, giBlurX, giBlurY, ssr, litColor;
  wgpu::Buffer emitterLights;
  uint32_t width = 0, height = 0, workWidth = 0, workHeight = 0;
  uint64_t age = 0, graph = 0;
  bool valid = false;
};
std::array<Entry, 8> entries;
uint64_t age = 0;
struct Pipeline {
  wgpu::RenderPipeline pipeline;
  wgpu::BindGroupLayout layout;
};
std::array<Pipeline,2> giVariants{};
Pipeline dofP;
wgpu::ComputePipeline emitterP;
wgpu::BindGroupLayout emitterLayout;
wgpu::Buffer emitterHistory;
std::unordered_map<int,Pipeline> dofAliasPipelines;
Pipeline depthPipelines[2], aoPipeline, blurXP, blurYP, composeP, giP, giBlurXP, giBlurYP, ssrP;
// Environment confidences filtered per recorded frame, and the last logged
// state (diagnostic events explain every discontinuity).
EnvironmentConfidence environmentConfidence;
uint64_t environmentFrameId=~0ull,lastDisplayedScene=0,hiddenSceneFrame=~0ull;
float hiddenSceneWeight=0,hiddenScenePrevious=0;
bool environmentActive=false;
// Composition into a copy alias uses the copy texture's own format.
std::unordered_map<int,Pipeline> aliasComposePipelines;
wgpu::ShaderModule common;
Target target(uint32_t width, uint32_t height, wgpu::TextureFormat format,
              const char *name) {
  wgpu::TextureDescriptor desc{.label = name,
                               .usage = wgpu::TextureUsage::RenderAttachment |
                                        wgpu::TextureUsage::TextureBinding,
                               .size = {width, height, 1},
                               .format = format};
  auto t = webgpu::g_device.CreateTexture(&desc);
  return {t, t.CreateView()};
}
wgpu::ShaderModule module(const std::string &source) {
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = source.c_str();
  wgpu::ShaderModuleDescriptor desc{.nextInChain = &wgsl,
                                    .label = "GekkoAOT Enhanced WGSL"};
  return webgpu::g_device.CreateShaderModule(&desc);
}
Pipeline pipeline(const wgpu::ShaderModule &shader, const char *entry,
                  wgpu::TextureFormat format,
                  std::initializer_list<unsigned> bindings, bool msaa = false) {
  std::array<wgpu::BindGroupLayoutEntry, 10> b{};
  unsigned count = 0;
  for (auto binding : bindings) {
    auto &e = b[count++];
    e.binding = binding;
    e.visibility = wgpu::ShaderStage::Fragment;
    if(binding==10)
      e.buffer={.type=wgpu::BufferBindingType::ReadOnlyStorage,.minBindingSize=256};
    else if (binding == 0)
      e.buffer = {.type = wgpu::BufferBindingType::Uniform,
                  .hasDynamicOffset = true,
                  .minBindingSize = sizeof(Params)};
    else if (binding == 7)
      e.texture = {.sampleType = wgpu::TextureSampleType::Depth,
                   .viewDimension = wgpu::TextureViewDimension::e2D};
    else
      e.texture = {.sampleType =
                       wgpu::TextureSampleType::UnfilterableFloat,
                   .viewDimension = wgpu::TextureViewDimension::e2D,
                   .multisampled = binding == 3 && msaa};
  }
  wgpu::BindGroupLayoutDescriptor bgd{
      .label = entry, .entryCount = count, .entries = b.data()};
  auto layout = webgpu::g_device.CreateBindGroupLayout(&bgd);
  wgpu::PipelineLayoutDescriptor ld{.bindGroupLayoutCount = 1,
                                    .bindGroupLayouts = &layout};
  auto pl = webgpu::g_device.CreatePipelineLayout(&ld);
  wgpu::ColorTargetState color{.format = format};
  wgpu::ConstantEntry specialization{};
  const bool dofVariant=std::string_view(entry)=="dof_compose", particleVariant=std::string_view(entry)=="gi";
  specialization.key=dofVariant?"gekkoaot_dof_enabled":"gekkoaot_particle_enabled";
  specialization.value=dofVariant?double(settings.dof_quality!=0):double(settings.particle_lights!=0);
  wgpu::FragmentState fragment{.module = shader,
                               .entryPoint = entry,
                               .constantCount=unsigned(dofVariant||particleVariant),
                               .constants=(dofVariant||particleVariant)?&specialization:nullptr,
                               .targetCount = 1,
                               .targets = &color};
  wgpu::RenderPipelineDescriptor desc{
      .label = entry,
      .layout = pl,
      .vertex = {.module = shader, .entryPoint = "vs"},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
      .fragment = &fragment};
  return {webgpu::g_device.CreateRenderPipeline(&desc), layout};
}
void ensure_pipelines(bool msaa) {
  if (!common) {
    common = module(Shader);
    aoPipeline =
        pipeline(common, "ao", wgpu::TextureFormat::RGBA8Unorm, {0, 2, 4, 7});
    blurXP =
        pipeline(common, "blur_x", wgpu::TextureFormat::RGBA8Unorm, {0, 4, 5});
    blurYP =
        pipeline(common, "blur_y", wgpu::TextureFormat::RGBA8Unorm, {0, 4, 5});

    constexpr auto light = wgpu::TextureFormat::RGBA16Float;

    giBlurXP = pipeline(common, "blur_x", light, {0, 4, 5});
    giBlurYP = pipeline(common, "blur_y", light, {0, 4, 5});
    ssrP = pipeline(common, "ssr", light, {0, 1, 2, 4});
  }
  if(!composeP.pipeline)composeP=pipeline(common,"compose",wgpu::TextureFormat::RGBA8Unorm,{0,1,2,4,5,6,7,8,9});
  if(settings.dof_quality && !dofP.pipeline)dofP=pipeline(common,"dof_compose",wgpu::TextureFormat::RGBA8Unorm,{0,1,2,4});
  auto& giVariant=giVariants[settings.particle_lights!=0];
  if(!giVariant.pipeline)giVariant=pipeline(common,"gi",wgpu::TextureFormat::RGBA16Float,{0,1,2,4,6,10});
  if(giP.pipeline.Get()!=giVariant.pipeline.Get())giP=giVariant;
  if(settings.particle_lights && !emitterP){
    std::array<wgpu::BindGroupLayoutEntry,6> bindings{};unsigned at=0;
    for(unsigned binding:{0u,1u,4u,6u,11u,12u}){
      auto& b=bindings[at++];b.binding=binding;b.visibility=wgpu::ShaderStage::Compute;
      if(binding==0)b.buffer={.type=wgpu::BufferBindingType::Uniform,.hasDynamicOffset=true,.minBindingSize=sizeof(Params)};
      else if(binding>=11)b.buffer={.type=wgpu::BufferBindingType::Storage,.minBindingSize=binding==11?8192u:256u};
      else b.texture={.sampleType=wgpu::TextureSampleType::UnfilterableFloat,.viewDimension=wgpu::TextureViewDimension::e2D};
    }
    const wgpu::BindGroupLayoutDescriptor bd{.entryCount=bindings.size(),.entries=bindings.data()};
    emitterLayout=webgpu::g_device.CreateBindGroupLayout(&bd);
    const wgpu::PipelineLayoutDescriptor ld{.bindGroupLayoutCount=1,.bindGroupLayouts=&emitterLayout};
    const auto layout=webgpu::g_device.CreatePipelineLayout(&ld);
    const wgpu::ComputePipelineDescriptor pd{.label="Stable particle radiance",.layout=layout,.compute={.module=common,.entryPoint="resolve_emitters"}};
    emitterP=webgpu::g_device.CreateComputePipeline(&pd);
    const wgpu::BufferDescriptor hd{.label="Particle radiance history",.usage=wgpu::BufferUsage::Storage,.size=8192};
    emitterHistory=webgpu::g_device.CreateBuffer(&hd);
  }
  auto &depth = depthPipelines[msaa ? 1 : 0];
  if (!depth.pipeline) {
    const std::string declaration = msaa ?
      "@group(0) @binding(3) var scene_capture: texture_multisampled_2d<f32>;\n"
      "fn read_capture(q:vec2i)->f32 { for(var i=0;i<i32(textureNumSamples(scene_capture));i++){let v=decode_capture(textureLoad(scene_capture,q,i));"
      "if((bitcast<u32>(v)&15u)>0u){return v;}}return 0.;}\n" :
      "@group(0) @binding(3) var scene_capture: texture_2d<f32>;\n"
      "fn read_capture(q:vec2i)->f32{return decode_capture(textureLoad(scene_capture,q,0));}\n";
    auto shader = module(std::string(Shader) + declaration + DepthShader);
    depth = pipeline(shader, "prepare", wgpu::TextureFormat::R32Float,
                     {0, 2, 3}, msaa);
  }
}
// Copy textures lack CopySrc: the fallback renders the original into the twin.
std::unordered_map<int,Pipeline> passthroughPipelines;
void blit_original(const wgpu::CommandEncoder& encoder,const TextureRef& source,const TextureRef& target) {
  if(!common)common=module(Shader);
  auto& pipe=passthroughPipelines[int(target.format)];
  if(!pipe.pipeline)pipe=pipeline(common,"passthrough",target.format,{1});
  const wgpu::BindGroupEntry entry{.binding=1,.textureView=source.sampleTextureView};
  const wgpu::BindGroupDescriptor bgd{.label="Enhanced alias fallback",.layout=pipe.layout,.entryCount=1,.entries=&entry};
  const auto bg=webgpu::g_device.CreateBindGroup(&bgd);
  const wgpu::RenderPassColorAttachment attachment{.view=target.attachmentTextureView,
    .loadOp=wgpu::LoadOp::Clear,.storeOp=wgpu::StoreOp::Store};
  const wgpu::RenderPassDescriptor pd{.label="Enhanced alias fallback",.colorAttachmentCount=1,.colorAttachments=&attachment};
  auto raster=encoder.BeginRenderPass(&pd);
  raster.SetPipeline(pipe.pipeline);raster.SetBindGroup(0,bg);raster.Draw(3);raster.End();
}
void draw(const wgpu::CommandEncoder &encoder, const Pipeline &pipe,
          const wgpu::TextureView &output, const detail::RenderPass &pass,
          Entry &entry, std::initializer_list<unsigned> bindings,
          const wgpu::TextureView &occlusion, const char *label, const wgpu::TextureView& colorOverride = {}) {
  std::array<wgpu::BindGroupEntry, 10> b{};
  unsigned count = 0;
  for (auto binding : bindings) {
    auto &item = b[count++];
    item.binding = binding;
    if (binding == 0) {
      item.buffer = detail::resources().uniformBuffer;
      item.size = sizeof(Params);
    } else if (binding == 1)
      item.textureView = colorOverride ? colorOverride : pass.resolveTarget
                             ? pass.resolveTarget->sampleTextureView
                             : pass.copySourceView;
    else if (binding == 2)
      item.textureView = pass.copySourceNormalTexture.CreateView();
    else if (binding == 3)
      item.textureView = pass.copySourceCaptureTexture.CreateView();
    else if (binding == 4)
      item.textureView = entry.depth.view;
    else if (binding == 6)
      item.textureView = pass.copySourceContributionTexture.CreateView();
    else if (binding == 7)
      item.textureView = shadow::cascade_view(pass);
    else if (binding == 8)
      item.textureView = entry.giBlurY.view;
    else if (binding == 9)
      item.textureView = entry.ssr.view;
    else if (binding == 10)
      {item.buffer=entry.emitterLights;item.size=256;}
    else
      item.textureView = occlusion;
  }
  wgpu::BindGroupDescriptor desc{.label = label,
                                 .layout = pipe.layout,
                                 .entryCount = count,
                                 .entries = b.data()};
  auto bg = webgpu::g_device.CreateBindGroup(&desc);
  const std::string_view name(label);
  const unsigned operation=name=="Enhanced depth"?0:name=="Enhanced AO/contact"?1:
    name=="Enhanced bilateral X"?2:name=="Enhanced bilateral Y"?3:
    name=="Enhanced compose"||name=="Enhanced copy alias"?4:name=="Enhanced indirect"?5:
    name=="Enhanced indirect X"?6:name=="Enhanced indirect Y"?7:name=="Enhanced reflections"?8:
    name=="Enhanced resolve particle lights"?9:10;
  const auto* measured=operation<10?measurement::timestamps(operation):nullptr;
  wgpu::RenderPassColorAttachment attachment{.view = output,
                                             .loadOp = wgpu::LoadOp::Clear,
                                             .storeOp = wgpu::StoreOp::Store};
  wgpu::RenderPassDescriptor pd{.label = label,
                                .colorAttachmentCount = 1,
                                .colorAttachments = &attachment,
                                .timestampWrites =
                                    measured ? measured : webgpu::gpu_prof::pass_writes(label)};
  auto raster = encoder.BeginRenderPass(&pd);
  raster.SetPipeline(pipe.pipeline);
  uint32_t offset = pass.enhancedUniformRange.offset;
  raster.SetBindGroup(0, bg, 1, &offset);
  raster.Draw(3);
  raster.End();
}
void resolve_particle_sources(const wgpu::CommandEncoder& encoder,const detail::RenderPass& pass,Entry& entry){
  const std::array bindings{
    wgpu::BindGroupEntry{.binding=0,.buffer=detail::resources().uniformBuffer,.size=sizeof(Params)},
    wgpu::BindGroupEntry{.binding=1,.textureView=pass.resolveTarget?pass.resolveTarget->sampleTextureView:pass.copySourceView},
    wgpu::BindGroupEntry{.binding=4,.textureView=entry.depth.view},
    wgpu::BindGroupEntry{.binding=6,.textureView=pass.copySourceContributionTexture.CreateView()},
    wgpu::BindGroupEntry{.binding=11,.buffer=emitterHistory,.size=8192},
    wgpu::BindGroupEntry{.binding=12,.buffer=entry.emitterLights,.size=256}};
  const wgpu::BindGroupDescriptor bd{.label="Particle radiance resolve",.layout=emitterLayout,.entryCount=bindings.size(),.entries=bindings.data()};
  const auto group=webgpu::g_device.CreateBindGroup(&bd);
  const wgpu::ComputePassDescriptor pd{.label="Particle radiance resolve",.timestampWrites=measurement::timestamps(9)};
  auto compute=encoder.BeginComputePass(&pd);compute.SetPipeline(emitterP);
  const uint32_t offset=pass.enhancedUniformRange.offset;
  compute.SetBindGroup(0,group,1,&offset);compute.DispatchWorkgroups(1);compute.End();
}
} // namespace
void forget_sampled_provenance() noexcept;
void note_copy(const TextureHandle& texture,uint32_t revision) {
  forget_sampled_provenance();
  if(webgpu::g_graphicsConfig.enhancedCapture)set_copy_provenance(resource_id(texture),revision);
}
unsigned current_view_id() noexcept{return currentView;}
void compile_startup_shaders() {
  if (settings.enhanced)
    ensure_pipelines(webgpu::g_graphicsConfig.msaaSamples > 1);
}
bool background_draw() noexcept { return cameraBackground; }
bool fragment_lighting() noexcept { return settings.enhanced && settings.fragment_lighting && !cameraBackground; }
// The game's vertical axis, remembered across sessions in its user folder:
// a camera that never turned cannot reveal a Z-up world by itself.
std::string environment_memory_path() {
  const char* dir=std::getenv("GEKKOAOT_USER_DIR");
  return dir&&*dir?std::string(dir)+"/enhanced-environment.ini":std::string();
}
int remembered_up_axis() {
  std::ifstream file(environment_memory_path());std::string line;
  while(std::getline(file,line))
    if(line.rfind("up_axis=",0)==0 && line.size()==9 && line[8]>='0' && line[8]<='2')return line[8]-'0';
  return 1;
}
void remember_up_axis(int axis) {
  const auto path=environment_memory_path();
  if(path.empty() || axis<0 || axis>2)return;
  std::ofstream(path)<<"up_axis="<<axis<<"\n";
}
void configure(const GraphicsSettings &value) {
  hd::configure(value);
  if(std::getenv("GEKKOAOT_ENHANCED_DIAGNOSTICS") &&
     (value.enhanced!=settings.enhanced || value.ao_quality!=settings.ao_quality ||
      value.contact_shadows!=settings.contact_shadows || value.fragment_lighting!=settings.fragment_lighting))
    std::fprintf(stderr,"GEKKOAOT_ENHANCED_CONFIG requested=%u ao_quality=%u contact_shadows=%u fragment_lighting=%u post_processing=%u view=%u\n",
      value.enhanced,value.ao_quality,value.contact_shadows,value.fragment_lighting,value.post,view_mode());
  if(value.fragment_lighting!=settings.fragment_lighting || value.enhanced!=settings.enhanced)
    gx::g_gxState.dirty |= gx::DirtyPipeline | gx::DirtyUniform;
  environment=EnvironmentFromGraphics(value);
  // The world frame and the sun serve the environment light and the sun
  // cascades independently. Evidence is gathered only while one of them is
  // on: turning the first one on starts afresh.
  const bool active=value.enhanced && (environment.mode!=EnvironmentMode::Off || value.sun_shadows);
  if(active && !environmentActive){
    environmentTracker.Reset();environmentTracker.SetPriorAxis(remembered_up_axis());
    environmentFrame={};environmentConfidence={};
  }
  environmentActive=active;
  // Re-armed by the next scene pass that has a sun.
  if(!value.enhanced || !value.sun_shadows)shadow::arm_cascades(0);
  settings = value;
}
// A transparent interface draw of this frame's scene copy over the scene (Tales
// of Symphonia's far blur). The image under it is the same scene, so its receivers
// stay where they are and compose lights the result once, from the original copy.
// Attenuating them and mixing in a lit twin left a visible cut at the draw's edge.
bool blended_scene_copy() noexcept {
  const auto& s=gx::g_gxState;
  if(s.projType!=GX_ORTHOGRAPHIC || s.blendMode!=GX_BM_BLEND || s.blendFacDst!=GX_BL_INVSRCALPHA ||
     (s.blendFacSrc!=GX_BL_SRCALPHA && s.blendFacSrc!=GX_BL_ONE) || copyAliases.empty())return false;
  for(unsigned i=0;i<s.numTevStages && i<s.tevStages.size();++i){
    const auto map=unsigned(s.tevStages[i].texMapId);
    if(map>=s.loadedTextures.size())continue;
    const auto copy=s.copyTextures.find(s.loadedTextures[map].data);
    if(copy==s.copyTextures.end())continue;
    const auto known=copyAliases.find(copy->second.handle.get());
    if(known!=copyAliases.end() && known->second.revision==copy->second.revision)return true;
  }
  return false;
}
// A game's post-processing chain works in a corner of the EFB and restores that
// corner from a copy before presenting (GoldenEye: blur and bloom in the top-left
// quarter). Those draws are no surface and must leave the receivers of the scene
// under them, or the restored corner loses every effect. Whether a pass ends in a
// plain copy without any receiver is only known at its end, so its draws follow
// the same pass of the last two frames.
struct CopyChain {
  uint64_t frame=~uint64_t(0),current=0,previous=0,older=0;unsigned ordinal=0;
  bool workArea=false; // The pass just cleared a work area: no need to guess.
  void Roll(uint64_t now) noexcept {
    if(now==frame)return;
    older=previous;previous=current;current=0;ordinal=0;frame=now;
  }
} copyChain;
bool in_copy_chain() noexcept {
  copyChain.Roll(gekkoaot_recording_frame_id());
  return copyChain.workArea || (copyChain.ordinal<64u && ((copyChain.previous&copyChain.older)>>copyChain.ordinal&1u));
}
// A screen-space redraw of a copy brings no surface either, unless it shows a
// composed twin: those pixels already hold the effects and must not get them again.
bool copy_redraw_without_twin() noexcept {
  const auto& s=gx::g_gxState;
  if(s.projType!=GX_ORTHOGRAPHIC || s.copyTextures.empty())return false;
  bool sampled=false;
  for(unsigned i=0;i<s.numTevStages && i<s.tevStages.size();++i){
    const auto map=unsigned(s.tevStages[i].texMapId);
    if(map>=s.loadedTextures.size())continue;
    const auto copy=s.copyTextures.find(s.loadedTextures[map].data);
    if(copy==s.copyTextures.end())continue;
    const auto known=copyAliases.find(copy->second.handle.get());
    if(known!=copyAliases.end() && known->second.revision==copy->second.revision && known->second.composed)return false;
    sampled=true;
  }
  return sampled;
}
bool chains_keep_receivers() noexcept {
  static const bool enabled=[]{const char* v=std::getenv("GEKKOAOT_ENHANCED_KEEP_RECEIVERS");return !v || v[0]!='0';}();
  return enabled;
}
bool keeps_receivers() noexcept {
  if(!settings.enhanced)return false;
  if(blended_scene_copy())return true;
  if(!chains_keep_receivers())return false;
  return copy_redraw_without_twin() || (in_copy_chain() && !eligible());
}
unsigned composition_mode() noexcept {
  const auto& s=gx::g_gxState;
  if(keeps_receivers())return 5;
  if(s.blendMode==GX_BM_NONE ||
     (s.blendMode==GX_BM_LOGIC && s.blendOp==GX_LO_COPY))return 0;
  if(s.blendMode==GX_BM_SUBTRACT ||
     (s.blendMode==GX_BM_LOGIC && s.blendOp==GX_LO_NOOP))return 2;
  if(s.blendMode!=GX_BM_BLEND)return 4;
  if(s.blendFacSrc==GX_BL_ONE && s.blendFacDst==GX_BL_ZERO)return 0;
  if((s.blendFacSrc==GX_BL_SRCALPHA || s.blendFacSrc==GX_BL_ONE) &&
     s.blendFacDst==GX_BL_INVSRCALPHA)return 1;
  // Destination-alpha factors depend on the original EFB, not our auxiliary
  // contribution target. Do not pretend those equations are interchangeable.
  if(s.blendFacDst==GX_BL_ONE && s.blendFacSrc!=GX_BL_DSTALPHA &&
     s.blendFacSrc!=GX_BL_INVDSTALPHA)return 2;
  // ...except for a draw that is not a receiver: it writes no receiver share
  // (zero contribution), so keeping the destination (factor ONE) leaves the
  // receivers below exactly as they were, whatever scales what it adds.
  // A destination-alpha mix (src DSTALPHA, dst INVDSTALPHA) only replaces
  // where the game marked EFB alpha (Rayman 3's full-screen glow composite);
  // the scene stays everywhere else. As unknown equations both erased every
  // receiver under the screen.
  if(!eligible() && (s.blendFacDst==GX_BL_ONE ||
                     (s.blendFacSrc==GX_BL_DSTALPHA && s.blendFacDst==GX_BL_INVDSTALPHA)))return 2;
  // Multiplies (also 2x: dst*src + src*dst, a lightmap pass) scale the
  // receiver share by the same factor as the color.
  if((s.blendFacSrc==GX_BL_DSTCLR && s.blendFacDst==GX_BL_ZERO) ||
     (s.blendFacSrc==GX_BL_ZERO && s.blendFacDst==GX_BL_SRCCLR) ||
     (s.blendFacSrc==GX_BL_DSTCLR && s.blendFacDst==GX_BL_SRCCLR))return 3;
  static const bool diagnostics=std::getenv("GEKKOAOT_ENHANCED_DIAGNOSTICS")!=nullptr;
  if(diagnostics){
    static std::array<uint32_t,32> seen{};static unsigned count=0;
    const uint32_t key=uint32_t(s.blendMode)|uint32_t(s.blendFacSrc)<<4|uint32_t(s.blendFacDst)<<8|uint32_t(s.blendOp)<<12|
      uint32_t(s.projType)<<16;
    if(count<seen.size() && std::find(seen.begin(),seen.begin()+count,key)==seen.begin()+count){
      seen[count++]=key;
      std::fprintf(stderr,"GEKKOAOT_ENHANCED_UNKNOWN_COMPOSITION mode=%u src=%u dst=%u op=%u ortho=%u\n",unsigned(s.blendMode),
        unsigned(s.blendFacSrc),unsigned(s.blendFacDst),unsigned(s.blendOp),s.projType==GX_ORTHOGRAPHIC);
    }
  }
  return 4;
}
namespace {
enum CaptureReason : unsigned { Opaque, Disabled, Offscreen, TextureProducer,
  Interface, NoDepthTest, UnprovedDepth, ColorDisabled, ColorContribution,
  DepthTexture, ScreenFill, PerspectiveInterface, InsetViewport, CameraBackground, ReasonCount };
constexpr const char* reasonNames[]{"opaque","disabled","offscreen","texture-producer",
  "orthographic-interface","no-depth-test","unproved-depth","color-disabled",
  "color-contribution","depth-texture","screen-fill","perspective-interface","inset-viewport","camera-background"};
CaptureReason compute_reason() noexcept {
  const auto &s = gx::g_gxState;
  const auto &v = s.logicalViewport;
  if(!webgpu::g_graphicsConfig.enhancedCapture)return Disabled;
  if(is_offscreen())return Offscreen;
  if(gekkoaot_native_gx_texture_viewport(v.left,v.top,v.width,v.height))return TextureProducer;
  if(s.projType!=GX_PERSPECTIVE)return Interface;
  if(cameraBackground)return CameraBackground;
  if(const auto& r=s.renderViewport;inset_viewport({r.left,r.top,r.width,r.height}))return InsetViewport;
  if(!s.depthCompare)return NoDepthTest;
  if(!(s.depthUpdate || s.depthFunc==GX_EQUAL ||
          (s.depthFunc==GX_LEQUAL && havePrepass &&
           prepassFrame==gekkoaot_recording_frame_id() && prepassCamera==camera_key())))return UnprovedDepth;
  if(!s.colorUpdate)return ColorDisabled;
  if(!(s.blendMode == GX_BM_NONE ||
          (s.blendMode == GX_BM_BLEND &&
           ((s.blendFacSrc == GX_BL_ONE && s.blendFacDst == GX_BL_ZERO) ||
            ((s.blendFacSrc == GX_BL_SRCALPHA || s.blendFacSrc == GX_BL_ONE) &&
             s.blendFacDst == GX_BL_INVSRCALPHA))) ||
          (s.blendMode == GX_BM_LOGIC && s.blendOp == GX_LO_COPY)))return ColorContribution;
  if(s.zTexOp!=GX_ZT_DISABLE)return DepthTexture;
  if(gekkoaot_native_gx_screen_fill())return ScreenFill;
  if(gekkoaot_native_gx_perspective_ui())return PerspectiveInterface;
  return Opaque;
}
// Pipeline key, pipeline config, uniforms and resource tracking all ask for
// the reason of the same draw while no GX state can change; begin_draw() at
// each Aurora draw entry invalidates it.
CaptureReason drawReason=Opaque;
bool drawReasonValid=false;
bool env_switch(const char* name) noexcept {
  const char* value=std::getenv(name);return !value || std::string_view(value)!="0";
}
CaptureReason classify() noexcept {
  static const bool memo=env_switch("GEKKOAOT_ENHANCED_CLASSIFY_MEMO");
  if(!memo)return compute_reason();
  if(!drawReasonValid){drawReason=compute_reason();drawReasonValid=true;}
  return drawReason;
}
}
void begin_draw() noexcept {poll_diagnostic_view();drawReasonValid=false;cameraBackground=false;++particleDrawId;particleMaterialPending=false;}
std::span<const std::uint8_t> exchange_geometry_source(std::span<const std::uint8_t> source) noexcept {
  const auto previous=originalGeometry;originalGeometry=source;return previous;
}
void observe_geometry(GXPrimitive primitive,GXVtxFmt fmt,unsigned count,std::span<const std::uint8_t> raw) noexcept {
  if(!originalGeometry.empty())raw=originalGeometry;
  const auto& s=gx::g_gxState;
  static const bool trace=std::getenv("GEKKOAOT_BACKGROUND_TRACE")!=nullptr;
  const auto frame=gekkoaot_recording_frame_id();backgroundPrelude.Begin(frame);
  // GXCopyDisp may publish world and weapon as different recording frames.
  // Retained scene color still contains the world particles in the latter.
  // Their sources expire with receiver metadata, not the copy/frame counter.
  particleEmitterFrame=frame;
  if(particleEmitterEpoch!=captureEpoch){particleEmitterEpoch=captureEpoch;particleEmitterCount=0;particleEmitterOrder=0;}
  const bool alphaOver=s.blendFacDst==GX_BL_INVSRCALPHA && !s.depthUpdate;
  const bool distantBackground=s.projType==GX_PERSPECTIVE && s.depthCompare &&
    s.depthUpdate && s.depthFunc==GX_ALWAYS && s.colorUpdate &&
    s.vtxDesc[GX_VA_NRM]==GX_NONE && composition_mode()<=1 && !is_offscreen() && count<=128;
  const bool emission=settings.enhanced && settings.particle_lights && s.projType==GX_PERSPECTIVE &&
    s.depthCompare && s.colorUpdate && !is_offscreen() && s.blendMode==GX_BM_BLEND &&
    (s.blendFacDst==GX_BL_ONE || alphaOver) &&
    (s.blendFacSrc==GX_BL_ONE || s.blendFacSrc==GX_BL_SRCALPHA) && !eligible() &&
    (primitive==GX_QUADS || primitive==GX_TRIANGLES || primitive==GX_TRIANGLESTRIP);
  if(!backgroundPrelude.Open() && !trace && !emission && !distantBackground)return;
  // A scene with thousands of particles gets its light sources from the
  // first few hundred (each 4x4 screen cell keeps four). Past that budget one
  // batch in eight is enough for later flames to still register, and the
  // pattern repeats frame to frame, so sources do not flicker.
  static std::uint64_t emissionFrame=0;static unsigned emissionVertices=0,emissionSkip=0;
  if(emission && frame!=emissionFrame){emissionFrame=frame;emissionVertices=0;emissionSkip=0;}
  if(emission && !backgroundPrelude.Open() && !trace && !distantBackground){
    const bool over=emissionVertices>=4096u;
    emissionVertices+=count;
    if(over && (emissionSkip++&7u)!=0u)return;
  }
  static std::uint64_t traceFrame=0;static unsigned traceDraw=0;
  if(frame!=traceFrame){traceFrame=frame;traceDraw=0;}
  const bool tracing=trace && frame%120==0;
  if(tracing && s.projType==GX_PERSPECTIVE && (s.vtxDesc[GX_VA_NRM]==GX_NONE || count>128) && traceDraw++<128){
    const auto& tex=s.loadedTextures[0];const auto pn=s.currentPnMtx;
    std::fprintf(stderr,"GEKKOAOT_BG_LAYOUT frame=%llu vtx=%u pos_mode=%u pos_count=%u type=%u normal=%u tex=%ux%u fmt=%u t=%.2f,%.2f,%.2f projection=%.6f,%.6f depth=%u,%u,%u\n",(unsigned long long)frame,count,unsigned(s.vtxDesc[GX_VA_POS]),gx::comp_cnt_count(GX_VA_POS,s.vtxFmts[fmt].attrs[GX_VA_POS].cnt),unsigned(s.vtxFmts[fmt].attrs[GX_VA_POS].type),unsigned(s.vtxDesc[GX_VA_NRM]),tex.width(),tex.height(),unsigned(tex.format()),s.xfPosRows[pn][3],s.xfPosRows[pn+1][3],s.xfPosRows[pn+2][3],s.proj[2][2],s.proj[2][3],s.depthCompare,s.depthUpdate,unsigned(s.depthFunc));
  }
  if(!settings.enhanced || s.projType!=GX_PERSPECTIVE || count<3 || (!emission && count>128) || raw.size()%count)return;
  if(emission && count>128){
    // Inspect every particle, not just the first 128 vertices of a batch.
    // Camera-dependent batching otherwise makes later flames disappear.
    const unsigned stride=raw.size()/count;
    const unsigned chunk=primitive==GX_TRIANGLES?126u:128u;
    const unsigned overlap=primitive==GX_TRIANGLESTRIP?2u:0u;
    GeometrySourceScope scopedSource({});
    for(unsigned first=0;first<count;first+=chunk-overlap){
      const unsigned n=std::min(chunk,count-first);if(n<3)break;
      observe_geometry(primitive,fmt,n,raw.subspan(std::size_t(first)*stride,std::size_t(n)*stride));
      if(first+n==count)break;
    }
    return;
  }
  const auto mode=s.vtxDesc[GX_VA_POS];if(mode==GX_NONE)return;
  unsigned offset=0,pnOffset=0;bool indexedMatrix=false;
  for(unsigned a=0;a<GX_VA_POS;++a){if(s.vtxDesc[a]==GX_NONE)continue;if(s.vtxDesc[a]!=GX_DIRECT)return;if(a==GX_VA_PNMTXIDX){indexedMatrix=true;pnOffset=offset;}++offset;}
  const unsigned stride=raw.size()/count;count=std::min(count,128u);const auto& format=s.vtxFmts[fmt].attrs[GX_VA_POS];
  if(gx::comp_cnt_count(GX_VA_POS,format.cnt)!=3)return;
  const unsigned scalarSize=gx::comp_type_size(GX_VA_POS,format.type),posBytes=scalarSize*3;
  if(!scalarSize || offset+(mode==GX_DIRECT?posBytes:mode==GX_INDEX8?1u:2u)>stride)return;
  const auto matrix=indexedMatrix?unsigned(raw[pnOffset]):s.currentPnMtx;
  if(matrix+2>=s.xfPosRows.size())return;
  // Most draws have a translated world/model transform. Reject those before
  // decoding any vertex data; classification must remain cheap per draw.
  float maxCoefficient=0;for(unsigned r=0;r<3;++r)for(unsigned c=0;c<3;++c)maxCoefficient=std::max(maxCoefficient,std::abs(s.xfPosRows[matrix+r][c]));
  bool translated=false;for(unsigned r=0;r<3;++r)translated|=std::abs(s.xfPosRows[matrix+r][3])>maxCoefficient*1e-4f;
  if(translated){backgroundPrelude.Scene();if(!tracing && !emission && !distantBackground)return;}
  const auto read=[](const std::uint8_t* p,unsigned n,bool le){std::uint32_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint32_t(p[i])<<(8*(le?i:n-1-i));return v;};
  std::array<std::array<float,3>,128> positions{},emissionPositions{};
  bool mixedMatrices=false;
  for(unsigned v=0;v<count;++v){
    const auto vertexMatrix=indexedMatrix?unsigned(raw[v*stride+pnOffset]):matrix;
    if(vertexMatrix+2>=s.xfPosRows.size())return;
    if(vertexMatrix!=matrix){mixedMatrices=true;if(!emission)return;}
    auto* p=raw.data()+v*stride+offset;bool le=false;
    if(mode!=GX_DIRECT){const auto index=read(p,mode==GX_INDEX8?1:2,false);const auto& array=s.arrays[GX_VA_POS];
      if(!array.data || !array.stride || std::uint64_t(index)*array.stride+posBytes>array.size)return;
      p=static_cast<const std::uint8_t*>(array.data)+std::size_t(index)*array.stride;le=array.le;}
    std::array<float,3> point{};
    for(unsigned c=0;c<3;++c){const auto value=read(p+c*scalarSize,scalarSize,le);
      switch(format.type){case GX_F32:point[c]=std::bit_cast<float>(value);break;case GX_U8:case GX_U16:point[c]=std::ldexp(float(value),-int(format.frac));break;
      case GX_S8:point[c]=std::ldexp(float(std::int8_t(value)),-int(format.frac));break;case GX_S16:point[c]=std::ldexp(float(std::int16_t(value)),-int(format.frac));break;default:return;}}
    for(unsigned r=0;r<3;++r){const auto& row=s.xfPosRows[vertexMatrix+r];positions[v][r]=row[0]*point[0]+row[1]*point[1]+row[2]*point[2];emissionPositions[v][r]=positions[v][r]+row[3];}
  }
  std::array<float,3> translation{};float scale=0;
  for(unsigned r=0;r<3;++r){const auto& row=s.xfPosRows[matrix+r];translation[r]=row[3];scale=std::max(scale,std::sqrt(row[0]*row[0]+row[1]*row[1]+row[2]*row[2]));}
  const float near=std::abs(s.proj[2][3])/(1.f+std::abs(s.proj[2][2]));
  if(tracing){float low=INFINITY,high=0;for(unsigned i=0;i<count;++i){const auto radius=std::sqrt(EnvDot(positions[i],positions[i]));low=std::min(low,radius);high=std::max(high,radius);}
    const auto& tex=s.loadedTextures[0];if(low>near*30.f || s.vtxDesc[GX_VA_NRM]==GX_NONE)std::fprintf(stderr,"GEKKOAOT_BG_TRACE frame=%llu vtx=%u near=%.5f t=%.3f,%.3f,%.3f radial=%.3f,%.3f scale=%.3f tex=%ux%u fmt=%u lighting=%u normal=%u depth=%u,%u,%u\n",(unsigned long long)frame,count,near,translation[0],translation[1],translation[2],low,high,scale,tex.width(),tex.height(),unsigned(tex.format()),s.numChans,unsigned(s.vtxDesc[GX_VA_NRM]),s.depthCompare,s.depthUpdate,unsigned(s.depthFunc));}
  if(emission){
    // A long triangle strip is many particles, not one city-sized lamp.
    const unsigned group=primitive==GX_QUADS?4u:3u,step=primitive==GX_TRIANGLESTRIP?1u:group;
    // Cache radiance by original source and TEV tint, independently of screen
    // position. Alpha animation is deliberately excluded from the identity.
    uint32_t radianceKey=2166136261u;
    const auto hash=[&](uint32_t v){radianceKey=(radianceKey^v)*16777619u;};
    for(unsigned i=0;i<s.numTevStages;++i){const auto map=unsigned(s.tevStages[i].texMapId);if(map>=s.loadedTextures.size())continue;
      const auto& tex=s.loadedTextures[map];const auto address=reinterpret_cast<uintptr_t>(tex.data);
      hash(uint32_t(address));hash(uint32_t(uint64_t(address)>>32));hash(unsigned(tex.format()));hash(tex.width()|(tex.height()<<16));hash(tex.texDataVersion);
      if(gx::is_palette_format(tex.format()) && unsigned(tex.tlut)<s.loadedTluts.size())hash(s.loadedTluts[unsigned(tex.tlut)].tlutDataVersion);}
    for(const auto& channel:s.colorChannelState)for(unsigned c=0;c<3;++c)hash(uint32_t(int(std::clamp(channel.matColor[c],-4.f,4.f)*16.f)));
    for(const auto& color:s.colorRegs)for(unsigned c=0;c<3;++c)hash(uint32_t(int(std::clamp(color[c],-4.f,4.f)*16.f)));
    hash(unsigned(alphaOver));radianceKey&=0xffffffu;if(!radianceKey)radianceKey=1u;
    const auto attributeKey=[&](uint32_t key,unsigned attr,unsigned vertex,std::array<float,4>* vertexColor=nullptr){
      if(s.vtxDesc[attr]==GX_NONE)return key;
      unsigned at=offset;
      for(unsigned a=GX_VA_POS;a<attr;++a){if(s.vtxDesc[a]==GX_NONE)continue;
        const auto& f=s.vtxFmts[fmt].attrs[a];const bool nbt3=a==GX_VA_NRM && f.cnt==GX_NRM_NBT3;
        at+=s.vtxDesc[a]==GX_DIRECT?gx::comp_type_size(GXAttr(a),f.type)*gx::comp_cnt_count(GXAttr(a),f.cnt):
          (s.vtxDesc[a]==GX_INDEX8?1u:2u)*(nbt3?3u:1u);
      }
      const auto mode=s.vtxDesc[attr];const auto& f=s.vtxFmts[fmt].attrs[attr];
      const unsigned component=gx::comp_type_size(GXAttr(attr),f.type),components=gx::comp_cnt_count(GXAttr(attr),f.cnt);
      const unsigned bytes=component*components;
      if(at+(mode==GX_DIRECT?bytes:mode==GX_INDEX8?1u:2u)>stride)return key;
      const auto* data=raw.data()+vertex*stride+at;bool le=false;
      if(mode!=GX_DIRECT){const auto index=read(data,mode==GX_INDEX8?1:2,false);const auto& array=s.arrays[attr];
        if(!array.data || !array.stride || uint64_t(index)*array.stride+bytes>array.size)return key;
        data=static_cast<const uint8_t*>(array.data)+std::size_t(index)*array.stride;le=array.le;}
      const auto word=[&](uint32_t v){key=(key^v)*16777619u;};
      word(unsigned(f.type));word(f.frac);
      if(attr==GX_VA_CLR0){auto rgb=read(data,component,le);
        if(vertexColor){
          auto& c=*vertexColor;c={1,1,1,1};
          if(f.type==GX_RGBA8 || f.type==GX_RGBX8){c={float(rgb>>24)/255.f,float(rgb>>16&255)/255.f,float(rgb>>8&255)/255.f,f.type==GX_RGBA8?float(rgb&255)/255.f:1.f};}
          else if(f.type==GX_RGB8)c={float(rgb>>16)/255.f,float(rgb>>8&255)/255.f,float(rgb&255)/255.f,1};
          else if(f.type==GX_RGBA6)c={float(rgb>>18&63)/63.f,float(rgb>>12&63)/63.f,float(rgb>>6&63)/63.f,float(rgb&63)/63.f};
          else if(f.type==GX_RGBA4)c={float(rgb>>12&15)/15.f,float(rgb>>8&15)/15.f,float(rgb>>4&15)/15.f,float(rgb&15)/15.f};
          else if(f.type==GX_RGB565)c={float(rgb>>11&31)/31.f,float(rgb>>5&63)/63.f,float(rgb&31)/31.f,1};
        }
        if(f.type==GX_RGBA8 || f.type==GX_RGBX8)rgb>>=8;
        else if(f.type==GX_RGBA6)rgb>>=6;else if(f.type==GX_RGBA4)rgb>>=4;
        word(rgb); // Exclude animated coverage alpha, preserve vertex tint.
      }else for(unsigned c=0;c<components;++c)word(read(data+c*component,component,le));
      return key;
    };
    const auto& viewport=s.renderViewport;
    for(unsigned first=0;first+group<=count;first+=step){
      std::array<float,3> center{};
      for(unsigned i=first;i<first+group;++i)for(unsigned r=0;r<3;++r)center[r]+=emissionPositions[i][r]/float(group);
      float extent=0;for(unsigned i=first;i<first+group;++i){auto delta=emissionPositions[i];for(unsigned r=0;r<3;++r)delta[r]-=center[r];extent=std::max(extent,std::sqrt(EnvDot(delta,delta)));}
      if(!std::isfinite(extent) || extent<near*.005f || extent>near*80.f || center[2]>=-near)continue;
      bool finite=true;for(auto f:center)finite&=std::isfinite(f);if(!finite)continue;
      float clip[4]{};for(unsigned r=0;r<4;++r)clip[r]=s.proj[r][0]*center[0]+s.proj[r][1]*center[1]+s.proj[r][2]*center[2]+s.proj[r][3];
      if(clip[3]<=0 || std::abs(clip[0])>clip[3]*1.6f+extent*std::abs(s.proj[0][0]) ||
         std::abs(clip[1])>clip[3]+extent*std::abs(s.proj[1][1]))continue;
      const float radius=std::min(near*160.f,std::max(near*3.f,extent*4.f));
      // Four sources in each 4x4 screen region. Particle-rich vegetation or
      // smoke at the start of the draw list must not starve a later flame.
      const unsigned cellX=unsigned(std::clamp((clip[0]/clip[3]*.5f+.5f)*4.f,0.f,3.f));
      const unsigned cellY=unsigned(std::clamp((clip[1]/clip[3]*.5f+.5f)*4.f,0.f,3.f));
      const unsigned cell=cellY*4+cellX;
      bool merged=false;
      for(unsigned i=0;i<particleEmitterCount;++i){auto& other=particleEmitters[i];float distance=0;for(unsigned r=0;r<3;++r)distance+=(other.position[r]-center[r])*(other.position[r]-center[r]);
        if(other.cell==cell && std::abs(other.near-near)<near*.001f && distance<radius*radius*.16f){merged=true;break;}}
      if(!merged){
        auto key=radianceKey;std::array<float,4> raster{};
        for(unsigned v=first;v<first+group;++v){std::array<float,4> color{1,1,1,1};key=attributeKey(key,GX_VA_CLR0,v,&color);
          for(unsigned c=0;c<4;++c)raster[c]+=color[c]/float(group);}
        // Different atlas regions must never borrow each other's fire colour.
        for(unsigned a=GX_VA_TEX0;a<=GX_VA_TEX7;++a){key=attributeKey(key,a,first);key=attributeKey(key,a,first+group-1);}
        key&=0xffffffu;if(!key)key=1u;
        unsigned inCell=0,oldest=0;uint64_t oldestOrder=~0ull;
        for(unsigned i=0;i<particleEmitterCount;++i)if(particleEmitters[i].cell==cell){
          ++inCell;if(particleEmitters[i].order<oldestOrder){oldest=i;oldestOrder=particleEmitters[i].order;}}
        const unsigned slot=inCell>=4?oldest:particleEmitterCount++;
        particleEmitters[slot]={{center[0],center[1],center[2],radius},near,std::abs(s.proj[1][1]),{viewport.left,viewport.top,viewport.width,viewport.height},alphaOver,cell,++particleEmitterOrder,key,{},raster,particleDrawId};
        particleMaterialPending=true;
      }
    }
  }
  cameraBackground=!mixedMatrices &&
    ((distantBackground && GekkoAOT::GX::DistantBackgroundFace({emissionPositions.data(),count},translation,near)) ||
     backgroundPrelude.Observe({positions.data(),count},translation,scale,near));
  static unsigned logs=0;
  if(cameraBackground && logs++<16)std::fprintf(stderr,"GEKKOAOT_CAMERA_BACKGROUND vertices=%u near=%.6f translation=%.6f,%.6f,%.6f radius=%.3f role=eye-centred-prelude\n",count,near,translation[0],translation[1],translation[2],std::sqrt(EnvDot(positions[0],positions[0])));
  drawReasonValid=false;
}
bool eligible() noexcept {return classify()==Opaque;}
unsigned diagnostic_coverage() noexcept {
  return view_mode()==2 && webgpu::g_graphicsConfig.enhancedCapture ? 1u : 0u;
}
unsigned diagnostic_reason() noexcept {
  return unsigned(classify());
}
void capture_projection(const float *p) noexcept {
  if (!webgpu::g_graphicsConfig.enhancedCapture)
    return;
  const auto &v = gx::g_gxState.renderViewport;
  const std::array<float, 4> viewport{v.left, v.top, v.width, v.height};
  const std::array<float, 2> range{gx::UseReversedZ ? 1.f - v.zfar : v.znear,
                                   gx::UseReversedZ ? 1.f - v.znear : v.zfar};
  if (!haveProjection || !std::equal(p, p + 16, lastProjection.begin()) ||
      cachedCapture.viewport != viewport ||
      cachedCapture.depth_range != range) {
    std::copy_n(p, 16, lastProjection.begin());
    cachedCapture = {};
    cachedCapture.Observe(lastProjection, viewport, range);
    haveProjection = true;
    static const bool diagnostics=std::getenv("GEKKOAOT_ENHANCED_DIAGNOSTICS")!=nullptr;
    static unsigned invalid_logs=0;
    if(cachedCapture.validity!=SceneValidity::Valid && diagnostics && invalid_logs++<8)
      std::fprintf(stderr,"GEKKOAOT_ENHANCED_INVALID_CAMERA viewport=%.3f,%.3f,%.3f,%.3f depth_range=%.9f,%.9f projection=%.6f,%.6f,%.6f,%.6f\n",v.left,v.top,v.width,v.height,range[0],range[1],lastProjection[0],lastProjection[5],lastProjection[10],lastProjection[14]);
  }
  // Track every uploaded projection, including excluded overlays. A later
  // eligible draw may reuse those uniforms without another projection write.
  const auto id=eligible()?observe_scene(cachedCapture):0u;
  if(id!=currentView){currentView=id;gx::g_gxState.dirty|=gx::DirtyUniform;}
}
// Farthest near plane seen per camera (field of view and viewport): the
// world slice. Kept across passes; a camera unseen for long is replaced.
struct WorldSlice {SceneCapture camera;float near=0;uint64_t frame=0;};
std::array<WorldSlice,8> worldSlices{};
bool view_model_draw() noexcept {
  if(cachedCapture.validity!=SceneValidity::Valid)return false;
  const float near=NearDistance(cachedCapture);const uint64_t frame=gekkoaot_recording_frame_id();
  WorldSlice* slot=nullptr;
  for(auto& w:worldSlices)if(w.near>0 && SameCamera(w.camera,cachedCapture)){slot=&w;break;}
  if(!slot){
    slot=&worldSlices[0];
    for(auto& w:worldSlices)if(w.frame<slot->frame)slot=&w;
    *slot={cachedCapture,near,frame};
  }
  if(PreferWorldSlice(cachedCapture,slot->camera) || frame>slot->frame+600){slot->camera=cachedCapture;slot->near=near;}
  slot->frame=frame;
  return near<.25f*slot->near;
}
unsigned shadow_suppression() noexcept {
  const auto& s=gx::g_gxState;
  // Shadow decals blend over the scene without writing depth. Opaque draws
  // that merely keep alpha blending enabled (alpha 1) write depth: untouched.
  if(!webgpu::g_graphicsConfig.enhancedCapture || !settings.enhanced || !shadow::cascades_armed() ||
     s.projType!=GX_PERSPECTIVE || is_offscreen() || s.depthUpdate)return 0;
  // Alpha-over decals fade; other blends (destination alpha, logic ops:
  // Animaniacs character shadows) drop their dark fragments.
  if(const unsigned mode=composition_mode();mode!=1 && mode!=4)return 0;
  return 900;
}
void observe_draw() noexcept {
  const auto& state=gx::g_gxState;
  // GEKKOAOT_LOGIC_TRACE=1: draws per GX logic op (destination-reading ops
  // cost a framebuffer snapshot per primitive), every 4096 such draws.
  static const bool logicTrace=std::getenv("GEKKOAOT_LOGIC_TRACE")!=nullptr;
  if(logicTrace && state.blendMode==GX_BM_LOGIC){
    static std::array<uint64_t,16> draws{};static uint64_t total=0;
    ++draws[unsigned(state.blendOp)&15u];
    if(++total%4096u==0){
      std::fprintf(stderr,"GEKKOAOT_LOGIC_OPS total=%llu",(unsigned long long)total);
      for(unsigned op=0;op<16u;++op)if(draws[op])std::fprintf(stderr," op%u=%llu",op,(unsigned long long)draws[op]);
      std::fprintf(stderr," color=%u alpha=%u proj=%u\n",state.colorUpdate?1u:0u,state.alphaUpdate?1u:0u,unsigned(state.projType));
    }
  }
  // The suppression lives in the draw uniform: rebuild it when it changes.
  static unsigned lastSuppression=0;
  if(const auto suppression=shadow_suppression();suppression!=lastSuppression){
    lastSuppression=suppression;gx::g_gxState.dirty|=gx::DirtyUniform;
  }
  if(webgpu::g_graphicsConfig.enhancedCapture && !is_offscreen() &&
     state.projType==GX_PERSPECTIVE && state.depthCompare && state.depthUpdate && !state.colorUpdate){
    prepassFrame=gekkoaot_recording_frame_id();prepassCamera=camera_key();havePrepass=true;
    drawReasonValid=false;
  }
  const auto reason=classify();
  // The coverage view stores this reason in the draw uniform; a reason change
  // need not change the pipeline, so rebuild the uniform explicitly.
  if(reason!=lastDiagnosticReason){
    lastDiagnosticReason=reason;gx::g_gxState.dirty|=gx::DirtyUniform;
  }
  const bool enabled = webgpu::g_graphicsConfig.enhancedCapture,
             valid = reason==Opaque;
  if(enabled){
    ++stats.reasons[reason];
    if(state.projType==GX_PERSPECTIVE)++stats.perspective;
  }
  const unsigned flags = enabled ? ((valid ? 3 : 1) | (cameraBackground?4u:0u)) : 0;
  if (flags != previousFlags) {
    gx::g_gxState.dirty |= gx::DirtyPipeline;
    // Only a sky/world role transition changes texture reconstruction policy.
    // Receiver/particle alternation is common thousands of times per frame;
    // invalidating textures there defeats the static binding/hash cache.
    if((flags^previousFlags)&4u)gx::g_gxState.dirty |= gx::DirtyTextures | gx::DirtyUniform;
    previousFlags = flags;
  }
  if (!enabled)
    return;
  static const bool diagnostics =
      std::getenv("GEKKOAOT_ENHANCED_DIAGNOSTICS") != nullptr;
  const auto drawCount = stats.eligible + stats.excluded;
  if (diagnostics && (drawCount < 8 || drawCount % 10000 == 0)) {
    const auto &s = gx::g_gxState;
    std::fprintf(
        stderr,
        "GEKKOAOT_ENHANCED_DRAW draw=%llu eligible=%u reason=%s composition=%u perspective=%u "
        "depth_compare=%u depth_write=%u color_write=%u blend=%u src=%u dst=%u "
        "ztex=%u offscreen=%u screen_fill=%u perspective_ui=%u\n",
        static_cast<unsigned long long>(drawCount), valid ? 1u : 0u,
        reasonNames[reason],composition_mode(),
        s.projType == GX_PERSPECTIVE, s.depthCompare, s.depthUpdate,
        s.colorUpdate, unsigned(s.blendMode), unsigned(s.blendFacSrc),
        unsigned(s.blendFacDst), unsigned(s.zTexOp), is_offscreen(),
        gekkoaot_native_gx_screen_fill(), gekkoaot_native_gx_perspective_ui());
  }
  if (valid) {
    ++stats.eligible;
    if (haveProjection && !(gx::g_gxState.dirty & gx::DirtyUniform))
      capture_projection(lastProjection.data());
  } else
    ++stats.excluded;
}
// Cameras of the pass being recorded, depth slices merged (see SameCamera).
// Scene cameras of the environment evidence. Ids persist across passes (a
// camera keeps its id while it is drawn): the main camera and its hysteresis
// compare ids from pass to pass, whatever order the game draws in.
std::array<SceneCapture,15> environmentCameras{};
std::array<uint64_t,15> environmentCameraSeen{};
unsigned environmentCameraCount=0,environmentCameraLast=0;
unsigned environment_camera() noexcept {
  if(cachedCapture.validity!=SceneValidity::Valid)return 0;
  const uint64_t frame=gekkoaot_recording_frame_id();
  if(environmentCameraLast && SameCamera(environmentCameras[environmentCameraLast-1],cachedCapture)){
    environmentCameraSeen[environmentCameraLast-1]=frame;return environmentCameraLast;
  }
  for(unsigned i=0;i<environmentCameraCount;++i)
    if(SameCamera(environmentCameras[i],cachedCapture)){environmentCameraSeen[i]=frame;return environmentCameraLast=i+1;}
  unsigned slot=environmentCameraCount;
  if(slot==environmentCameras.size()){
    slot=0;for(unsigned i=1;i<environmentCameras.size();++i)if(environmentCameraSeen[i]<environmentCameraSeen[slot])slot=i;
  }else ++environmentCameraCount;
  environmentCameras[slot]=cachedCapture;environmentCameraSeen[slot]=frame;return environmentCameraLast=slot+1;
}
void observe_environment(unsigned vertices) noexcept {
  if(!environmentActive || !settings.enhanced)return;
  if(classify()!=Opaque){++environmentNotOpaque;return;}
  const auto& s=gx::g_gxState;
  ++environmentDraws;
  if(s.vtxDesc[GX_VA_PNMTXIDX]!=GX_NONE)++environmentSkinned;
  // Skinned draws select their matrices per vertex; they never define the world.
  float rows[3][4];
  if(s.vtxDesc[GX_VA_PNMTXIDX]==GX_NONE && s.currentPnMtx+2<s.xfPosRows.size()){
    for(unsigned r=0;r<3;++r)for(unsigned c=0;c<4;++c)rows[r][c]=s.xfPosRows[s.currentPnMtx+r][c];
    environmentTracker.Observe(rows,float(vertices),environment_camera());
  }else rows[0][3]=rows[1][3]=rows[2][3]=0;
  // Directional GX lights of the lit color channels: SDK suns sit at a
  // practically infinite view-space position without attenuation.
  uint32_t mask=0,spot=0;
  for(unsigned c=0;c<std::min<unsigned>(s.numChans,2);++c){
    const auto& config=s.colorChannelConfig[c];
    if(!config.lightingEnabled || config.diffFn==GX_DF_NONE || config.attnFn==GX_AF_SPEC)continue;
    const auto lights=uint32_t(s.colorChannelState[c].lightMask.to_ulong());
    mask|=lights;if(config.attnFn==GX_AF_SPOT)spot|=lights;
  }
  const uint32_t key=mask|spot<<8;
  if(!mask || (key==observedLightMask && std::equal(s.lights.begin(),s.lights.end(),observedLights.begin())))return;
  observedLightMask=key;observedLights=s.lights;
  const float reach=1000.f*std::max(1.f,std::sqrt(rows[0][3]*rows[0][3]+rows[1][3]*rows[1][3]+rows[2][3]*rows[2][3]));
  for(unsigned i=0;i<::GX::MaxLights;++i){
    if(!(mask&(1u<<i)))continue;
    const auto& light=s.lights[i];
    const EnvVec3 position{light.pos[0],light.pos[1],light.pos[2]};
    const float distance=std::sqrt(EnvDot(position,position));
    const bool attenuated=(spot&(1u<<i)) &&
      (light.distAtt[1]!=0 || light.distAtt[2]!=0 || light.cosAtt[1]!=0 || light.cosAtt[2]!=0);
    if(!std::isfinite(distance) || distance<reach || attenuated)continue;
    environmentTracker.ObserveDirectionalLight(position,{light.color[0],light.color[1],light.color[2]},i);
  }
}
// Per texture slot: the provenance of the texture it sampled last. Copy
// textures, their revisions and producers change only at an EFB copy or a pass
// seal, so consecutive draws over the same textures skip both map lookups.
// Every insertion into copyTextures goes through copy_tex (note_copy); removals
// (eviction, cache clears) shrink it, which the recorded size catches.
struct SampledProvenance { const void* data=nullptr; size_t copies=0; bool copy=false; uint64_t id=0, producer=0; uint32_t revision=0; };
std::array<SampledProvenance,gx::MaxTextures> sampledProvenance{};
void forget_sampled_provenance() noexcept {sampledProvenance.fill({});}
void record_draw_resources(unsigned sampled) noexcept {
  if(!webgpu::g_graphicsConfig.enhancedCapture)return;
  if(particleMaterialPending){
    // This hook follows Aurora's texture resolution on normal and merged
    // draws. Observing the cache here cannot change original GX bindings.
    const auto material=particle_material();
    for(unsigned i=0;i<particleEmitterCount;++i)if(particleEmitters[i].draw==particleDrawId)
      particleEmitters[i].color=particle_radiance(material,particleEmitters[i].raster);
    particleMaterialPending=false;
  }
  record_classification(classify(),composition_mode());
  const auto& state=gx::g_gxState;
  // The shader analysis includes TEV, indirect sampling and Z-texture use.
  // Bound but unsampled textures are not dependencies of this draw.
  for(unsigned map=0;map<gx::MaxTextures;++map){
    if(!(sampled&(1u<<map)))continue;
    const void* data=state.loadedTextures[map].data;
    auto& known=sampledProvenance[map];
    if(!data || known.data!=data || known.copies!=state.copyTextures.size()){
      known={data,state.copyTextures.size()};
      const auto found=data?state.copyTextures.find(data):state.copyTextures.end();
      if(found!=state.copyTextures.end() && found->second.handle){
        known.copy=true;known.revision=found->second.revision;
        known.id=resource_id(found->second.handle);known.producer=resourceHistory.Read(known.id,known.revision);
      }
    }
    if(known.copy)record_texture_use({known.id,known.producer,known.revision,1,1u<<map});
  }
}
void log_seal(const detail::RenderPass& pass) noexcept;
TextureHandle new_alias(const TextureRef& copy);
void seal_body(detail::RenderPass &pass);
void seal(detail::RenderPass &pass) {
  copyChain.Roll(gekkoaot_recording_frame_id());
  copyChain.workArea=false;
  if(copyChain.ordinal<64u && pass.resolveTarget && !pass.captureDepthSnapshot &&
     pass.enhancedReasons[Opaque]==0 && pass.shadowPlans.empty())copyChain.current|=uint64_t(1)<<copyChain.ordinal;
  ++copyChain.ordinal;
  seal_body(pass);log_seal(pass);
}
void seal_body(detail::RenderPass &pass) {
  forget_sampled_provenance();
  ++stats.sealedPasses;
  if(pass.sceneCapture.count)++stats.scenePasses;
  stats.maxCameras=std::max(stats.maxCameras,uint64_t(pass.sceneCapture.count));
  if(webgpu::g_graphicsConfig.enhancedCapture){
    pass.provenancePass=resourceHistory.NextPass();
    if(pass.copySourceTexture && pass.copySourceTexture.Get()==webgpu::g_frameBuffer.texture.Get()){
      if(lastEfbTexture.Get()!=pass.copySourceTexture.Get()){lastEfbTexture=pass.copySourceTexture;lastEfbPass=0;}
      if(!pass.colorAttachments[SceneColorAttachmentIndex].clear)pass.provenanceColorParent=lastEfbPass;
      lastEfbPass=pass.provenancePass;
    }
    if(pass.provenanceCopyResource)resourceHistory.Write(pass.provenanceCopyResource,
      pass.provenanceCopyRevision,pass.provenancePass);
  }
  static const bool diagnostics=std::getenv("GEKKOAOT_ENHANCED_DIAGNOSTICS")!=nullptr;
  static const bool allPasses=[] {const auto* v=std::getenv("GEKKOAOT_ENHANCED_DIAGNOSTICS");return v&&std::string_view(v)=="all";}();
  static uint64_t passSequence=0,nonemptySequence=0,sceneLogSequence=0;
  const bool classified=std::any_of(pass.enhancedReasons.begin(),pass.enhancedReasons.end(),[](auto n){return n!=0;});
  if(diagnostics && (allPasses || (classified && ((pass.sceneCapture.count && sceneLogSequence<64) || nonemptySequence%300==0)))){
    std::fprintf(stderr,"GEKKOAOT_ENHANCED_PASS sequence=%llu frame=%llu views=%u rejected=%u copy=%u source=%p target=%p rect=%d,%d,%d,%d reasons=",
      static_cast<unsigned long long>(passSequence),static_cast<unsigned long long>(gekkoaot_recording_frame_id()),
      pass.sceneCapture.count,pass.sceneCapture.rejected,bool(pass.resolveTarget),
      static_cast<void*>(pass.copySourceTexture.Get()),pass.resolveTarget?static_cast<void*>(pass.resolveTarget->texture.Get()):nullptr,
      pass.resolveRect.x,pass.resolveRect.y,pass.resolveRect.width,pass.resolveRect.height);
    for(unsigned i=0;i<ReasonCount;++i)if(pass.enhancedReasons[i])std::fprintf(stderr,"%s:%u,",reasonNames[i],pass.enhancedReasons[i]);
    std::fprintf(stderr," composition=%u,%u,%u,%u,%u provenance_pass=%llu color_parent=%llu copy_resource=%llu copy_revision=%u use_overflow=%u reads=",
      pass.enhancedComposition[0],pass.enhancedComposition[1],pass.enhancedComposition[2],pass.enhancedComposition[3],pass.enhancedComposition[4],
      (unsigned long long)pass.provenancePass,(unsigned long long)pass.provenanceColorParent,
      (unsigned long long)pass.provenanceCopyResource,pass.provenanceCopyRevision,pass.provenanceUseOverflow);
    for(unsigned i=0;i<pass.provenanceUseCount;++i){const auto& use=pass.provenanceUses[i];
      std::fprintf(stderr,"%llu@%u<-%llu:maps%02x:draws%u,",(unsigned long long)use.resource,use.revision,
        (unsigned long long)use.producer,use.maps,use.draws);}
    // Effective view-space clip distances of each retained camera.
    std::fprintf(stderr," lenses=");
    for(unsigned i=0;i<pass.sceneCapture.count;++i){
      const auto& v=pass.sceneCapture.views[i];
      const auto distance=[&](float ndc){
        float h[4]{};for(unsigned r=0;r<4;++r)h[r]=v.inverse[4*r+2]*ndc+v.inverse[4*r+3];
        return h[3]!=0.f?-h[2]/h[3]:0.f;};
      std::fprintf(stderr,"%u:%g..%g@%g,%g,%g,%g[%.6g,%.6g,%.6g,%.6g];",i+1,distance(1.f),distance(0.f),v.viewport[0],v.viewport[1],
        v.viewport[2],v.viewport[3],v.projection[0],v.projection[2],v.projection[5],v.projection[6]);
    }
    std::fprintf(stderr,"\n");
  }
  if(pass.sceneCapture.count)++sceneLogSequence;
  if(classified)++nonemptySequence;
  ++passSequence;
  // The coverage view also explains passes in which every camera was rejected.
  // A texture copy pass without receivers (Opaque draws) of its own has
  // nothing to compose: its graph is skipped. Displayed passes always compose
  // (they present receivers recorded by the passes they continue).
  const bool idleCopy=pass.resolveTarget && !pass.captureDepthSnapshot &&
    pass.enhancedReasons[Opaque]==0 && pass.shadowPlans.empty();
  if (!settings.enhanced || ((pass.sceneCapture.count==0 || idleCopy) && view_mode()!=2) ||
      !pass.copySourceCaptureTexture)return;
  const auto size=pass.colorAttachments[SceneColorAttachmentIndex].size;
  const unsigned quality=settings.ao_quality;
  // Occlusion and indirect light are low frequency and go through the bilateral
  // upsample anyway. Full resolution at 1920x1200 cost 18 ms of GPU time on an
  // integrated GPU (AO 8 ms, indirect 10 ms) for no visible difference, so only
  // small outputs keep it.
  const bool half=quality<3 || std::uint64_t(size.width)*size.height>1300000u;
  pass.enhancedWidth=half?(size.width+1)/2:size.width;
  pass.enhancedHeight=half?(size.height+1)/2:size.height;
  Params p;
  for(unsigned i=0;i<pass.sceneCapture.count;++i){const auto& v=pass.sceneCapture.views[i];
    p.views[i]={v.projection,v.inverse,v.viewport};}
  p.depth={float(view_mode()),1,float(size.width),float(size.height)};
  p.effects={quality?settings.ao_strength:0.f,settings.ao_radius,
    settings.contact_shadows?settings.shadow_strength:0.f,settings.shadow_distance};
  p.light={settings.light_x,settings.light_y,settings.light_z,settings.shadow_thickness};
  const float steps=quality==1?4.f:quality==3?8.f:6.f;
  p.quality={steps,steps,settings.shadow_softness,quality==3?32.f:20.f};
  p.work={float(pass.enhancedWidth),float(pass.enhancedHeight),0,0};
  // GI radius is a multiple of the AO radius; reflections march up to the
  // receiver's own view depth.
  p.gi={settings.gi?settings.gi_strength:0.f,6.f*settings.ao_radius,quality==3?6.f:4.f,quality==3?8.f:4.f};
  p.dof={settings.dof_quality?settings.dof_strength:0.f,settings.dof_focus,settings.dof_quality==2?24.f:12.f,0.f};
  p.ssr={settings.ssr?settings.ssr_strength:0.f,1.f,quality==3?32.f:16.f,settings.surface_specular};
  p.emission_style={settings.particle_light_radius,.7f,5.f,0.f};
  if(settings.particle_lights && particleEmitterEpoch==captureEpoch){
    for(unsigned l=0;l<particleEmitterCount;++l){const auto& light=particleEmitters[l];
      for(unsigned v=0;v<pass.sceneCapture.count;++v){const auto& camera=pass.sceneCapture.views[v];
        if(std::abs(NearDistance(camera)-light.near)>light.near*.01f ||
           std::abs(std::abs(camera.projection[5])-light.focal)>light.focal*.01f || camera.viewport!=light.viewport)continue;
        const auto index=unsigned(p.emission[1]);p.emitters[index]=light.position;p.emitter_views[index]={float(v+1),float(light.alphaOver),float(light.radianceKey),0};p.emitter_colors[index]=light.color;p.emission[1]+=1;break;
      }
    }
    p.emission[0]=settings.particle_light_strength;
    p.emission[2]=settings.particle_lights==2?8.f:4.f;
    p.emission[3]=float(uint32_t(gekkoaot_recording_frame_id())&65535u);
    unsigned knownSources=0,emissiveSources=0;
    for(unsigned i=0;i<unsigned(p.emission[1]);++i)if(p.emitter_colors[i][3]>.5f){++knownSources;if(p.emitter_colors[i][0]+p.emitter_colors[i][1]+p.emitter_colors[i][2]>.05f)++emissiveSources;}
    static unsigned logs=0;if(p.emission[1]>0 && (logs++<16 || particleEmitterFrame%120==0))std::fprintf(stderr,"GEKKOAOT_PARTICLE_LIGHTS candidates=%u matched=%u known_sources=%u emissive_sources=%u source=additive-or-alpha-gx-geometry color=decoded-texture-tev-or-gpu-fallback\n",particleEmitterCount,unsigned(p.emission[1]),knownSources,emissiveSources);
    if(std::getenv("GEKKOAOT_ENHANCED_DIAGNOSTICS") && p.emission[1]>0 && particleEmitterFrame%120==0){
      std::fprintf(stderr,"GEKKOAOT_PARTICLE_SOURCES frame=%llu",(unsigned long long)particleEmitterFrame);
      for(unsigned i=0;i<unsigned(p.emission[1]);++i){const auto& point=p.emitters[i];
        std::fprintf(stderr," %u:%.3f,%.3f,%.3f/%.3f[%u,%u]",i,point[0],point[1],point[2],point[3],unsigned(p.emitter_views[i][0]),unsigned(p.emitter_views[i][1]));}
      std::fprintf(stderr,"\n");
    }
  }
  p.copy_rect=pass.resolveTarget?std::array<float,4>{float(pass.resolveRect.x),float(pass.resolveRect.y),
    float(pass.resolveRect.width),float(pass.resolveRect.height)}:std::array<float,4>{0,0,float(size.width),float(size.height)};
  // Environment evidence of the receivers recorded into this pass. Without
  // the environment light the tracker still finds the game's sun for the
  // cascades (Auto rules: only a proven world-stable game light).
  auto tracking=environment;
  if(tracking.mode==EnvironmentMode::Off)tracking.mode=EnvironmentMode::Auto;
  const uint64_t frameId=gekkoaot_recording_frame_id();
  // Only what reaches the screen defines the environment: a pass copied into
  // a texture (a shadow or map render from another camera) keeps the visible
  // scene's state, unless no displayed pass has drawn receivers recently.
  // Games compositing their whole 3D view through a texture (Tales of
  // Symphonia) then use their heaviest hidden scene pass of the last frame.
  const float receivers=environmentTracker.PendingWeight();
  const bool displayed=pass.captureDepthSnapshot || !pass.resolveTarget;
  if(displayed && receivers>0)lastDisplayedScene=frameId;
  if(frameId!=hiddenSceneFrame){hiddenScenePrevious=hiddenSceneWeight;hiddenSceneWeight=0;hiddenSceneFrame=frameId;}
  if(!displayed)hiddenSceneWeight=std::max(hiddenSceneWeight,receivers);
  const bool geometryContinuation=!pass.shadowPlans.empty() && carriedCascades.count &&
    carriedCascades.epoch==captureEpoch && frameId==carriedCascades.frame;
  const bool evidence=displayed || geometryContinuation || (frameId>lastDisplayedScene+30 && receivers>0 && receivers>=.5f*hiddenScenePrevious);
  static const bool candidateTrace=[]{const char* v=std::getenv("GEKKOAOT_ENVIRONMENT_TRACE");return v && v[0]=='2';}();
  if(candidateTrace && environmentActive && evidence && frameId%30==0)
    environmentTracker.ForEachCandidate([&](float w,unsigned view,const std::array<EnvVec3,3>& a,const EnvVec3& o,unsigned origins){
      std::fprintf(stderr,"GEKKOAOT_ENV_CAND frame=%llu w=%.0f view=%u x=%.3f,%.3f,%.3f y=%.3f,%.3f,%.3f z=%.3f,%.3f,%.3f o=%.2f,%.2f,%.2f origins=%u\n",
        (unsigned long long)frameId,w,view,a[0][0],a[0][1],a[0][2],a[1][0],a[1][1],a[1][2],a[2][0],a[2][1],a[2][2],o[0],o[1],o[2],origins);});
  const bool tracked=environmentActive && evidence;
  if(tracked)environmentFrame=environmentTracker.Finish(tracking,frameId);
  const auto environmentEvents=tracked?environmentFrame.events:0u;
  environmentTracker.BeginPass();
  observedLightMask=~0u;
  // Confidences follow the evidence within a few frames; a new scene or
  // another sun starts from zero and fades in.
  if(environmentEvents&(EnvSceneReset|EnvSunSwitched|EnvSunRejected))environmentConfidence.sun=0;
  if(environmentEvents&(EnvSceneReset|EnvUpSwitched))environmentConfidence.sky=0;
  if(environmentEvents&(EnvUpLocked|EnvUpSwitched) && environmentFrame.up_axis!=remembered_up_axis())
    remember_up_axis(environmentFrame.up_axis);
  if(frameId!=environmentFrameId){
    environmentFrameId=frameId;
    const float sky=environmentFrame.valid?1.f:0.f,sun=environmentFrame.source!=SunSource::None?1.f:0.f;
    environmentConfidence.sky+=.25f*(sky-environmentConfidence.sky);
    environmentConfidence.sun+=.25f*(sun-environmentConfidence.sun);
  }
  const auto env=MakeEnvironmentUniforms(environment,environmentFrame,environmentConfidence);
  const bool sunKnown=environmentActive && environmentFrame.source!=SunSource::None;
  p.env_sun=env.sun;p.env_sun_color=env.sun_color;p.env_sky=env.sky;p.env_ground=env.ground;p.env_up=env.up;
  // Contact shadows march towards the sun once one is known.
  if(sunKnown)p.light={env.sun[0],env.sun[1],env.sun[2],settings.shadow_thickness};
  // Sun cascades of the camera drawing most casters (the world camera); plans
  // of the next passes are recorded while a sun is known. They need a light
  // direction, not the environment light: with it off they only darken.
  const unsigned cascadeResolution=sunKnown?SunShadowResolution(settings.sun_shadows):0;
  const unsigned passCascades=evidence?cascadeResolution:0;
  SunCascades cascades;
  unsigned casters=0,cascadeView=0;
  uint32_t cascadeViews=0;
  if(passCascades){
    // The camera (all its depth slices) drawing most casters; cascades are
    // framed on its world slice, the one with the farthest near plane.
    const auto counts=shadow::caster_counts(pass);
    // The camera of the last cascades is kept while it draws at least half
    // as many casters as the best one (a game alternating a full-screen and
    // a partial view must not move its shadows between them).
    unsigned bestDraws=0,keptDraws=0;uint32_t keptViews=0;
    for(unsigned id=1;id<=pass.sceneCapture.count;++id){
      const auto mask=CameraMask(pass.sceneCapture,id);
      unsigned draws=0;
      for(unsigned i=0;i<pass.sceneCapture.count;++i)if(mask>>i&1u)draws+=counts[i+1];
      if(draws>bestDraws){bestDraws=draws;cascadeViews=mask;}
      if(haveCascadeCamera && draws>keptDraws && SameCamera(pass.sceneCapture.views[id-1],lastCascadeCamera)){keptDraws=draws;keptViews=mask;}
    }
    if(keptViews && 2*keptDraws>=bestDraws)cascadeViews=keptViews;
    float nearest=-1;
    for(unsigned i=0;i<pass.sceneCapture.count;++i){
      float n,f;
      if((cascadeViews>>i&1u) && CameraDepthRange(pass.sceneCapture.views[i],n,f) && n>nearest){nearest=n;cascadeView=i+1;}
    }
    const bool append=geometryContinuation && cascadeView &&
      SameCamera(pass.sceneCapture.views[cascadeView-1],carriedCascades.camera);
    if(append){
      cascades.clip=carriedCascades.clip;cascades.params=carriedCascades.params;
      cascades.count=carriedCascades.count;cascades.resolution=carriedCascades.resolution;
    }
    const bool fitted=append || (cascadeView && FitSunCascades(pass.sceneCapture.views[cascadeView-1],
      {env.sun[0],env.sun[1],env.sun[2]},{env.up[0],env.up[1],env.up[2]},MaxSunCascades,cascadeResolution,cascades,3000.f,
      environmentFrame.anchored?&environmentFrame.anchor:nullptr,environmentFrame.valid?&environmentFrame.horizontal:nullptr));
    casters=fitted?shadow::finalize_cascades(pass,cascades.clip.data(),cascades.count,cascadeViews,append):0;
    // Another camera continuing the scene (a weapon: near plane under a
    // quarter of the world's, or fewer casters) keeps the scene's cascades;
    // its own plans are dropped so the atlas is not overwritten.
    if(casters && carriedCascades.count && carriedCascades.epoch==captureEpoch && frameId<=carriedCascades.frame+2){
      const auto& camera=pass.sceneCapture.views[cascadeView-1];
      if(!SameCamera(camera,carriedCascades.camera) &&
         (NearDistance(camera)<.25f*carriedCascades.near || casters<carriedCascades.casters)){
        pass.shadowPlans.clear();pass.shadowCascades=0;casters=0;
      }
    }
    if(casters){
      lastCascadeCamera=pass.sceneCapture.views[cascadeView-1];haveCascadeCamera=true;
      p.csm=cascades.clip;p.csm_params=cascades.params;
      p.csm_info={float(cascades.count),float(cascades.resolution),float(cascadeViews),
        environment.mode==EnvironmentMode::Off?settings.shadow_strength*environmentConfidence.sun:0.f};
      carriedCascades={frameId,captureEpoch,cascades.count,cascades.resolution,casters,
        NearDistance(pass.sceneCapture.views[cascadeView-1]),pass.sceneCapture.views[cascadeView-1],
        cascades.clip,cascades.params,p.csm_info};
    }
  }
  if(!casters && cascadeResolution && carriedCascades.count && frameId<=carriedCascades.frame+2 &&
     carriedCascades.epoch==captureEpoch){
    p.csm=carriedCascades.clip;p.csm_params=carriedCascades.params;p.csm_info=carriedCascades.info;
    pass.shadowCascades=carriedCascades.count;pass.shadowResolution=carriedCascades.resolution;
  }
  if(evidence)shadow::arm_cascades(cascadeResolution);
  if(environmentActive && evidence){
    ++environmentPasses;environmentWorld+=environmentFrame.valid;
    environmentCameraLights+=environmentFrame.light_camera_relative;++environmentSources[unsigned(environmentFrame.source)];
    static const bool environmentDiagnostics=std::getenv("GEKKOAOT_ENHANCED_DIAGNOSTICS")!=nullptr;
    static const bool environmentTrace=std::getenv("GEKKOAOT_ENVIRONMENT_TRACE")!=nullptr;
    const auto d=environmentTracker.Debug();
    if(environmentTrace){
      const auto e=environmentTracker.LastEvidence();
      std::fprintf(stderr,"GEKKOAOT_ENV_TRACE frame=%llu pass=%llu views=%u main=%u cand=%u dom=%.0f tot=%.0f world=%u up=%d "
        "src=%u score=%d sun=%u unseen=%u events=%x sunw=%.3f,%.3f,%.3f sunv=%.3f,%.3f,%.3f upv=%.3f,%.3f,%.3f "
        "upscore=%.2f,%.2f,%.2f swing=%.2f,%.2f,%.2f conf=%.2f,%.2f weights=%.2f,%.2f casc=%u casters=%u cview=%u "
        "ref=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f heavy=%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f "
        "draws=%llu skinned=%llu sheared=%u blended=%llu plans=%zu\n",
        (unsigned long long)frameId,(unsigned long long)environmentPasses,pass.sceneCapture.count,environmentFrame.main_view,
        e.candidates,e.dominant,e.total,environmentFrame.valid,d.up_axis,unsigned(environmentFrame.source),d.light_score,
        d.have_sun,d.unseen,environmentFrame.events,d.sun_world[0],d.sun_world[1],d.sun_world[2],env.sun[0],env.sun[1],env.sun[2],
        env.up[0],env.up[1],env.up[2],d.score[0],d.score[1],d.score[2],d.swing[0],d.swing[1],d.swing[2],
        environmentConfidence.sky,environmentConfidence.sun,env.sky[3],env.sun[3],pass.shadowCascades,casters,cascadeView,
        d.world_axes[0][0],d.world_axes[0][1],d.world_axes[0][2],d.world_axes[1][0],d.world_axes[1][1],d.world_axes[1][2],
        d.world_axes[2][0],d.world_axes[2][1],d.world_axes[2][2],e.heaviest[0][0],e.heaviest[0][1],e.heaviest[0][2],
        e.heaviest[1][0],e.heaviest[1][1],e.heaviest[1][2],e.heaviest[2][0],e.heaviest[2][1],e.heaviest[2][2],
        (unsigned long long)(environmentDraws-tracedDraws),(unsigned long long)(environmentSkinned-tracedSkinned),e.sheared,
        (unsigned long long)(environmentNotOpaque-tracedNotOpaque),pass.shadowPlans.size());
      tracedDraws=environmentDraws;tracedSkinned=environmentSkinned;tracedNotOpaque=environmentNotOpaque;
    }
    // Events: one line per discontinuity, with its cause.
    if(environmentDiagnostics && environmentFrame.events){
      static constexpr const char* names[]{"scene_reset","camera_cut","up_locked","up_switched","sun_locked",
        "sun_switched","sun_rejected_camera_relative","main_camera_changed"};
      std::fprintf(stderr,"GEKKOAOT_ENV_EVENT frame=%llu events=",(unsigned long long)frameId);
      for(unsigned i=0;i<8;++i)if(environmentFrame.events&(1u<<i))std::fprintf(stderr,"%s,",names[i]);
      std::fprintf(stderr," main_camera=%u up_axis=%d sun_world=%.3f,%.3f,%.3f source=%u light_score=%d\n",
        environmentFrame.main_view,d.up_axis,d.sun_world[0],d.sun_world[1],d.sun_world[2],
        unsigned(environmentFrame.source),d.light_score);
    }
    if(environmentDiagnostics && (environmentPasses<=4 || environmentPasses%300==0))
      std::fprintf(stderr,"GEKKOAOT_ENVIRONMENT pass=%llu mode=%u world=%u up_axis=%d confidence=%.3f source=%s "
        "camera_relative_light=%u sun=%.3f,%.3f,%.3f up=%.3f,%.3f,%.3f conf_sky=%.2f conf_sun=%.2f cascades=%u casters=%u\n",
        (unsigned long long)environmentPasses,unsigned(environment.mode),environmentFrame.valid,environmentFrame.up_axis,
        environmentFrame.up_confidence,environmentFrame.source==SunSource::GameLight?"game_light":
        environmentFrame.source==SunSource::WorldFrame?"world_frame":"none",environmentFrame.light_camera_relative,
        env.sun[0],env.sun[1],env.sun[2],env.up[0],env.up[1],env.up[2],environmentConfidence.sky,environmentConfidence.sun,
        pass.shadowCascades,casters);
  }
  pass.enhancedParticleSources=p.emission[1]>0;
  pass.enhancedUniformRange = push_uniform(p);
  // A plain color texture copy gets an enhanced twin of the same size and
  // format; encode() composes into it right after the original copy.
  if(pass.provenanceCopyResource && pass.resolveTarget && std::getenv("GEKKOAOT_ENHANCED_DIAGNOSTICS") &&
     (gx::is_depth_format(pass.resolveFormat) || tex_copy_conv::needs_conversion(pass.resolveFormat))){
    static unsigned skips=0;
    if(skips++<16)std::fprintf(stderr,"GEKKOAOT_ENHANCED_ALIAS_SKIP format=%u depth=%u conversion=%u views=%u\n",
      unsigned(pass.resolveFormat),gx::is_depth_format(pass.resolveFormat),tex_copy_conv::needs_conversion(pass.resolveFormat),
      pass.sceneCapture.count);
  }
  static const bool aliases=env_switch("GEKKOAOT_ENHANCED_COPY_ALIAS");
  // Display copies (GXCopyTex into the XFB) are marked for presentation/peek
  // snapshots and keep the presented enhanced output instead of a twin.
  if(aliases && !pass.captureDepthSnapshot &&
     pass.provenanceCopyResource && pass.resolveTarget &&
     !gx::is_depth_format(pass.resolveFormat) &&
     (!tex_copy_conv::needs_conversion(pass.resolveFormat) || pass.resolveFormat==GX_TF_RGB565)) {
    const auto& copy=pass.resolveTarget;auto& alias=copyAliases[copy.get()];
    if(alias.copy.lock()!=copy || !alias.alias || alias.alias->size.width!=copy->size.width ||
       alias.alias->size.height!=copy->size.height || alias.alias->format!=copy->format)
      alias={copy,new_alias(*copy),0};
    alias.revision=pass.provenanceCopyRevision;
    // The table stays current for blended_scene_copy(); the twin itself is only
    // composed while draws read it. CPU color peeks need the original EFB.
    alias.composed=!colorPeeksSeen.load(std::memory_order_relaxed) && aliasWantedFrame && gekkoaot_recording_frame_id()-aliasWantedFrame<=120;
    if(alias.composed)pass.enhancedAlias=alias.alias;
    static unsigned logs=0;
    if(std::getenv("GEKKOAOT_ENHANCED_DIAGNOSTICS") && (logs++<8 || logs%2000==0))
      std::fprintf(stderr,"GEKKOAOT_ENHANCED_ALIAS_REGISTER copy=%p revision=%u views=%u size=%ux%u\n",
        static_cast<const void*>(copy.get()),alias.revision,pass.sceneCapture.count,copy->size.width,copy->size.height);
  }
}
void log_seal(const detail::RenderPass& pass) noexcept {
  static const bool allPasses=[] {const auto* v=std::getenv("GEKKOAOT_ENHANCED_DIAGNOSTICS");return v&&std::string_view(v)=="all";}();
  if(!allPasses)return;
  std::fprintf(stderr,"GEKKOAOT_ENHANCED_SEAL frame=%llu copy=%p format=%u views=%u params=%u copy_resource=%llu revision=%u alias=%p\n",
    static_cast<unsigned long long>(gekkoaot_recording_frame_id()),pass.resolveTarget?static_cast<const void*>(pass.resolveTarget.get()):nullptr,
    unsigned(pass.resolveFormat),pass.sceneCapture.count,pass.enhancedUniformRange.size!=0,
    static_cast<unsigned long long>(pass.provenanceCopyResource),pass.provenanceCopyRevision,
    pass.enhancedAlias?static_cast<const void*>(pass.enhancedAlias.get()):nullptr);
}
// The twin has the copy's exact size, format and GX format: only its texels
// differ, so every sampler/swizzle decision made for the copy still applies.
TextureHandle new_alias(const TextureRef& copy) {
  const wgpu::TextureDescriptor desc{.label="Enhanced copy alias",
    .usage=wgpu::TextureUsage::TextureBinding|wgpu::TextureUsage::RenderAttachment,
    .size=copy.size,.format=copy.format};
  auto texture=webgpu::g_device.CreateTexture(&desc);
  auto view=texture.CreateView();
  return std::make_shared<TextureRef>(std::move(texture),view,view,copy.size,copy.format,1,copy.gxFormat);
}
void note_color_peek() noexcept {
  if(colorPeeksSeen.exchange(true,std::memory_order_relaxed))return;
  std::fprintf(stderr,"GEKKOAOT_ENHANCED_COPY_ALIAS state=disabled reason=cpu-efb-color-peek\n");
}
TextureHandle copy_texture(const TextureHandle& copy,uint32_t revision) noexcept {
  static unsigned logs=0;
  static const bool diagnostics=std::getenv("GEKKOAOT_ENHANCED_DIAGNOSTICS")!=nullptr;
  if(diagnostics && copy && !copyAliases.empty() && gx::g_gxState.projType==GX_ORTHOGRAPHIC &&
     (logs++<16 || logs%1000==0)){
    const auto known=copyAliases.find(copy.get());
    std::fprintf(stderr,"GEKKOAOT_ENHANCED_ALIAS_LOOKUP copy=%p revision=%u ortho=%u registered=%d enhanced=%u\n",
      static_cast<const void*>(copy.get()),revision,gx::g_gxState.projType==GX_ORTHOGRAPHIC,
      known==copyAliases.end()?-1:int(known->second.revision),settings.enhanced);
  }
  if(!copy || !settings.enhanced || copyAliases.empty() || gx::g_gxState.projType!=GX_ORTHOGRAPHIC ||
     colorPeeksSeen.load(std::memory_order_relaxed))return copy;
  const auto found=copyAliases.find(copy.get());
  if(found==copyAliases.end() || found->second.revision!=revision || found->second.copy.lock()!=copy)return copy;
  if(blended_scene_copy())return copy;
  aliasWantedFrame=gekkoaot_recording_frame_id();
  if(!found->second.composed)return copy;
  ++aliasBindings;return found->second.alias;
}
// The draw's composition depends on the bound textures, which alone do not mark
// the pipeline dirty: a draw with the same state may have just used another mode.
bool keeps_receivers_changed() noexcept {
  static bool last=false;
  const bool now=keeps_receivers();
  if(now==last)return false;
  last=now;return true;
}
bool alias_bindings_stale() noexcept {
  if(!settings.enhanced || copyAliases.empty())return false;
  const unsigned state=(gx::g_gxState.projType==GX_ORTHOGRAPHIC?1u:0u)|(blended_scene_copy()?2u:0u);
  if(state==aliasBoundState)return false;
  aliasBoundState=state;return true;
}
// GoldenEye's bloom saves the top-left quarter of the EFB, clears it, draws its
// glow there and restores the color before presenting. Such a clear ends a copy
// that drew no surface and covers a part of the EFB only: it clears the color of
// a work area, not the scene, whose receivers must still be there afterwards.
bool keeps_receivers_under_clear(const detail::RenderPass& pass,int32_t width,int32_t height) noexcept {
  if(!settings.enhanced || !chains_keep_receivers() || pass.captureDepthSnapshot ||
     pass.enhancedReasons[Opaque]!=0 || !pass.shadowPlans.empty())return false;
  const auto& size=pass.colorAttachments[SceneColorAttachmentIndex].size;
  if(uint64_t(width)*uint64_t(height)*2u>uint64_t(size.width)*size.height)return false;
  copyChain.workArea=true;
  return true;
}
void note_capture_cleared() noexcept {++captureEpoch;}
void encode(const wgpu::CommandEncoder &encoder,
            const detail::RenderPass &pass) {
  // A registered alias must always hold this copy revision: the original when
  // no enhanced composition is produced for it.
  const auto alias_fallback=[&] {
    if(!pass.enhancedAlias || !pass.resolveTarget)return;
    blit_original(encoder,*pass.resolveTarget,*pass.enhancedAlias);++aliasFallbacks;
  };
  // End-of-frame/peek passes and texture copies with an enhanced twin. The
  // twin is composed inline, right after its copy and before later passes
  // reuse the capture attachments.
  // A replay composes its displayed pass again from its own scene: leaving the
  // real frame's output in place would present the same image once more.
  const bool replay=shadow::frame_replay();
  if (!webgpu::g_graphicsConfig.enhancedCapture ||
      (!pass.captureDepthSnapshot && !pass.enhancedAlias && !(replay && pass.gekkoaotReplayDisplayed))) {
    alias_fallback();return;
  }
  const EncodeTimer timer;
  const auto source =
      pass.resolveTarget ? pass.resolveTarget->texture : pass.copySourceTexture;
  Entry *slot = nullptr;
  for (auto &e : entries)
    if (e.source.Get() == source.Get()) {
      slot = &e;
      break;
    }
  if (slot)
    slot->valid = false; // Never present a previous enhanced version of a new
                         // original copy.
  if (pass.enhancedUniformRange.size == 0) {
    static const bool diagnostics =
        std::getenv("GEKKOAOT_ENHANCED_DIAGNOSTICS") != nullptr;
    const auto skipped = stats.missing + stats.invalid + stats.mixed;
    if(diagnostics && (skipped<8 || skipped%300==0))
      std::fprintf(stderr,"GEKKOAOT_ENHANCED_CAPTURE views=%u rejected=%u normals=%u\n",
        pass.sceneCapture.count,pass.sceneCapture.rejected,bool(pass.copySourceNormalTexture));
    ++stats.missing;
    alias_fallback();
    return;
  }
  const auto size = pass.colorAttachments[SceneColorAttachmentIndex].size;
  if (pass.resolveTarget && gx::is_depth_format(pass.resolveFormat)) {
    ++stats.invalid;
    alias_fallback();
    return;
  }
  if (!slot) {
    slot = &*std::min_element(
        entries.begin(), entries.end(),
        [](const auto &a, const auto &b) { return a.age < b.age; });
    if (slot->source)
      ++stats.evictions;
    *slot = {};
    slot->source = source;
  }
  auto &e = *slot;
  e.age = ++age;
  if (e.width != size.width || e.height != size.height ||
      e.workWidth != pass.enhancedWidth ||
      e.workHeight != pass.enhancedHeight) {
    e.width = size.width;
    e.height = size.height;
    e.workWidth = pass.enhancedWidth;
    e.workHeight = pass.enhancedHeight;
    e.depth = target(e.width, e.height, wgpu::TextureFormat::R32Float,
                     "Enhanced linear view depth");
    e.ao = target(e.workWidth, e.workHeight, wgpu::TextureFormat::RGBA8Unorm,
                  "Enhanced AO/contact");
    e.blurX = target(e.workWidth, e.workHeight, wgpu::TextureFormat::RGBA8Unorm,
                     "Enhanced bilateral X");
    e.blurY = target(e.workWidth, e.workHeight, wgpu::TextureFormat::RGBA8Unorm,
                     "Enhanced bilateral Y");
    constexpr auto light = wgpu::TextureFormat::RGBA16Float;
    e.gi = target(e.workWidth, e.workHeight, light, "Enhanced indirect");
    e.giBlurX = target(e.workWidth, e.workHeight, light, "Enhanced indirect X");
    e.giBlurY = target(e.workWidth, e.workHeight, light, "Enhanced indirect Y");
    e.ssr = target(e.workWidth, e.workHeight, light, "Enhanced reflections");
    const wgpu::BufferDescriptor lights{.label="Enhanced selected particle lights",.usage=wgpu::BufferUsage::Storage,.size=256};
    e.emitterLights=webgpu::g_device.CreateBuffer(&lights);
    e.litColor={};
    auto output =
        target(source.GetWidth(), source.GetHeight(),
               wgpu::TextureFormat::RGBA8Unorm, "Enhanced composition");
    e.output = {.texture = output.texture,
                .view = output.view,
                .size = {source.GetWidth(), source.GetHeight(), 1},
                .format = wgpu::TextureFormat::RGBA8Unorm,
                .sampler = webgpu::g_frameBuffer.sampler};
  }
  ensure_pipelines(pass.msaaSamples > 1);
  const auto colorResource = pass.resolveTarget ? RenderResource::DisplayCopy
                                                : RenderResource::EfbColor;
  OrderedRenderGraph graph(Resource(colorResource) |
                           Resource(RenderResource::SceneCapture) |
                           Resource(RenderResource::SceneNormals) |
                           Resource(RenderResource::OpaqueContribution));
  graph.Add({RenderPass::PrepareDepth,
             Resource(RenderResource::SceneCapture) |
                 Resource(RenderResource::SceneNormals),
             Resource(RenderResource::ViewDepth)});
  graph.Add({RenderPass::ScreenOcclusion,
             Resource(RenderResource::ViewDepth) |
                 Resource(RenderResource::SceneNormals),
             Resource(RenderResource::Occlusion)});
  graph.Add({RenderPass::BilateralX,
             Resource(RenderResource::ViewDepth) |
                 Resource(RenderResource::Occlusion),
             Resource(RenderResource::FilteredOcclusion)});
  graph.Add({RenderPass::BilateralY,
             Resource(RenderResource::ViewDepth) |
                 Resource(RenderResource::FilteredOcclusion),
             Resource(RenderResource::Occlusion)});
  // Indirect light and reflections read the original color of this pass.
  const bool indirectLight = settings.gi != 0 || (settings.particle_lights && pass.enhancedParticleSources), mirrors = settings.ssr != 0;
  if (indirectLight) {
    graph.Add({RenderPass::ScreenIndirect,
               Resource(colorResource) | Resource(RenderResource::ViewDepth) | Resource(RenderResource::SceneNormals) | Resource(RenderResource::OpaqueContribution),
               Resource(RenderResource::Indirect)});
    graph.Add({RenderPass::IndirectBlurX, Resource(RenderResource::ViewDepth) | Resource(RenderResource::Indirect),
               Resource(RenderResource::FilteredIndirect)});
    graph.Add({RenderPass::IndirectBlurY,
               Resource(RenderResource::ViewDepth) | Resource(RenderResource::FilteredIndirect),
               Resource(RenderResource::Indirect)});
  }
  if (mirrors)
    graph.Add({RenderPass::ScreenReflections,
               Resource(colorResource) | Resource(RenderResource::ViewDepth) | Resource(RenderResource::SceneNormals),
               Resource(RenderResource::Reflections)});
  graph.Add({RenderPass::ComposeEnhanced,
             Resource(colorResource) | Resource(RenderResource::Occlusion) |
                 Resource(RenderResource::ViewDepth) | Resource(RenderResource::OpaqueContribution) |
                 (indirectLight ? Resource(RenderResource::Indirect) : 0) |
                 (mirrors ? Resource(RenderResource::Reflections) : 0),
             Resource(RenderResource::EnhancedColor)});
  if(!replay)measurement::begin(stats.graphs+1,settings.ao_quality,settings.contact_shadows,size.width,size.height,pass.msaaSamples);
  e.valid = graph.Execute([&](RenderPass operation) {
    // Occlusion and indirect light are low frequency and at most one guest
    // frame old here: a replay keeps the real frame's and only composes them.
    if(replay && operation!=RenderPass::PrepareDepth && operation!=RenderPass::ComposeEnhanced)return true;
    switch (operation) {
    case RenderPass::PrepareDepth:
      draw(encoder, depthPipelines[pass.msaaSamples > 1], e.depth.view, pass, e,
           {0, 2, 3}, {}, "Enhanced depth");
      break;
    case RenderPass::ScreenOcclusion:
      draw(encoder, aoPipeline, e.ao.view, pass, e, {0, 2, 4, 7}, {},
           "Enhanced AO/contact");
      break;
    case RenderPass::BilateralX:
      draw(encoder, blurXP, e.blurX.view, pass, e, {0, 4, 5}, e.ao.view,
           "Enhanced bilateral X");
      break;
    case RenderPass::BilateralY:
      draw(encoder, blurYP, e.blurY.view, pass, e, {0, 4, 5}, e.blurX.view,
           "Enhanced bilateral Y");
      break;
    case RenderPass::ScreenIndirect:
      if(settings.particle_lights)
        resolve_particle_sources(encoder,pass,e);
      draw(encoder, giP, e.gi.view, pass, e, {0, 1, 2, 4, 6,10}, {}, "Enhanced indirect");
      break;
    case RenderPass::IndirectBlurX:
      draw(encoder, giBlurXP, e.giBlurX.view, pass, e, {0, 4, 5}, e.gi.view, "Enhanced indirect X");
      break;
    case RenderPass::IndirectBlurY:
      draw(encoder, giBlurYP, e.giBlurY.view, pass, e, {0, 4, 5}, e.giBlurX.view, "Enhanced indirect Y");
      break;
    case RenderPass::ScreenReflections:
      draw(encoder, ssrP, e.ssr.view, pass, e, {0, 1, 2, 4}, {}, "Enhanced reflections");
      break;
    case RenderPass::ComposeEnhanced:
      // DOF consumes the completed lit/shadowed image. Blurring the original
      // before adding a sharp receiver contribution produces a ghost layer.
      if(settings.dof_quality && (view_mode()==0 || view_mode()==4)){
        if(!e.litColor.texture)e.litColor=target(e.output.size.width,e.output.size.height,wgpu::TextureFormat::RGBA8Unorm,"Enhanced lit image before DOF");
        draw(encoder,composeP,e.litColor.view,pass,e,{0,1,2,4,5,6,7,8,9},e.blurY.view,"Enhanced compose");
        const auto format=pass.enhancedAlias?pass.enhancedAlias->format:wgpu::TextureFormat::RGBA8Unorm;
        auto& dof=format==wgpu::TextureFormat::RGBA8Unorm?dofP:dofAliasPipelines[int(format)];
        if(!dof.pipeline)dof=pipeline(common,"dof_compose",format,{0,1,2,4});
        draw(encoder,dof,pass.enhancedAlias?pass.enhancedAlias->attachmentTextureView:e.output.view,
             pass,e,{0,1,2,4},{},"Enhanced final depth of field",e.litColor.view);
        break;
      }
      if(pass.enhancedAlias){
        const auto format=pass.enhancedAlias->format;
        auto& compose=format==wgpu::TextureFormat::RGBA8Unorm?composeP:aliasComposePipelines[int(format)];
        if(!compose.pipeline)compose=pipeline(common,"compose",format,{0,1,2,4,5,6,7,8,9});
        draw(encoder,compose,pass.enhancedAlias->attachmentTextureView,pass,e,{0,1,2,4,5,6,7,8,9},
             e.blurY.view,"Enhanced copy alias");
        ++aliasComposites;
      }else
        draw(encoder, composeP, e.output.view, pass, e, {0, 1, 2, 4, 5, 6, 7, 8, 9},
             e.blurY.view, "Enhanced compose");
      break;
    default:
      return false;
    }
    return true;
  });
  // The original output entry never presents a texture copy.
  const bool composed=e.valid;
  if(composed && !replay){
    if(settings.ao_quality)++stats.aoPasses;
    if(settings.contact_shadows)++stats.contactPasses;
    measurement::finish(encoder,pass.enhancedUniformRange.offset,
      pass.resolveTarget?pass.resolveTarget->sampleTextureView:pass.copySourceView,
      pass.enhancedAlias?pass.enhancedAlias->attachmentTextureView:e.output.view,
      e.depth.view,pass.copySourceNormalTexture.CreateView(),e.blurY.view,e.emitterLights);
  }
  if(pass.enhancedAlias){
    if(!composed)alias_fallback();
    e.valid=false;
  }
  if (composed && !replay) {
    ++stats.graphs;
    e.graph=stats.graphs;
    if (stats.graphs <= 4 || stats.graphs % 300 == 0)
      std::fprintf(
          stderr,
          "GEKKOAOT_ENHANCED_GRAPH graph=%llu passes=5 scene=valid efb=%ux%u "
          "work=%ux%u msaa=%u views=%u rejected_views=%u vertical_fov=%.2f\n",
          static_cast<unsigned long long>(stats.graphs), e.width, e.height,
          e.workWidth, e.workHeight, pass.msaaSamples,pass.sceneCapture.count,pass.sceneCapture.rejected,
          114.591559f *
              std::atan(1.f / std::abs(pass.sceneCapture.views[0].projection[5])));
  }
}
const webgpu::TextureWithSampler &
present_source(const webgpu::TextureWithSampler &source) {
  const auto& diagnostic=shadow::preview(source);
  if(diagnostic.texture.Get()!=source.texture.Get())return diagnostic;
  if (settings.enhanced)
    for (auto &e : entries)
      if (e.valid && e.source.Get() == source.texture.Get())
        return e.output;
  return source;
}
void reset() {
  backgroundPrelude={};cameraBackground=false;particleEmitterCount=0;particleEmitterFrame=~0ull;
  measurement::reset();
  environmentConfidence={};environmentFrameId=~0ull;
  giP={};giBlurXP={};giBlurYP={};ssrP={};giVariants={};dofP={};emitterP={};emitterLayout={};emitterHistory={};dofAliasPipelines.clear();
  originalGeometry={};particleEmitterCount=0;particleEmitterFrame=particleEmitterEpoch=~0ull;
  shadow::reset();
  resourceHistory.Reset();textureLifetimes.clear();nextResourceId=resourceClock=lastEfbPass=0;lastEfbTexture={};
  havePrepass=false;prepassFrame=0;prepassCamera={};
  for (auto &e : entries)
    e = {};
  for (auto &p : depthPipelines)
    p = {};
  aoPipeline = {};
  blurXP = {};
  blurYP = {};
  composeP = {};
  aliasComposePipelines.clear();passthroughPipelines.clear();
  copyAliases.clear();aliasBoundState=0;aliasWantedFrame=0;aliasBindings=aliasComposites=aliasFallbacks=0;
  colorPeeksSeen=false;
  common = {};
  haveProjection = false;
  cachedCapture = {};
  previousFlags = 0;currentView=0;
}
uint64_t output_graph(const webgpu::TextureWithSampler& source) noexcept {
  for(const auto& entry:entries)if(entry.valid && entry.output.texture.Get()==source.texture.Get())return entry.graph;
  return 0;
}
void record_presentation(uint64_t graph,bool success) noexcept {
  if(!success)return;
  ++stats.presents;
  if(graph)++stats.enhancedPresents;
  static uint64_t lastMeasuredGraph=0;
  const bool measured=graph && (graph<=4 || graph%measurement::SamplePeriod==0) && graph!=lastMeasuredGraph;
  if(measured)lastMeasuredGraph=graph;
  if(std::getenv("GEKKOAOT_ENHANCED_DIAGNOSTICS") && (stats.presents<8 || stats.presents%300==0 || measured))
    std::fprintf(stderr,"GEKKOAOT_ENHANCED_PRESENT sequence=%llu graph=%llu view=%u wsi_success=1 alias_content=unmeasured\n",
      (unsigned long long)stats.presents,(unsigned long long)graph,view_mode());
}
void report() {
  std::fprintf(stderr,"GEKKOAOT_ENHANCED_COVERAGE requested=%u initialized=%u draws_3d=%llu sealed_passes=%llu scene_passes=%llu max_cameras=%llu ao_passes=%llu contact_passes=%llu gi_passes=0 shadow_map_passes=0 ssr_passes=0 wsi_presents=%llu direct_enhanced_presents=%llu rejection_counts=",
    settings.enhanced,webgpu::g_graphicsConfig.enhancedCapture,
    (unsigned long long)stats.perspective,(unsigned long long)stats.sealedPasses,
    (unsigned long long)stats.scenePasses,(unsigned long long)stats.maxCameras,
    (unsigned long long)stats.aoPasses,(unsigned long long)stats.contactPasses,
    (unsigned long long)stats.presents,(unsigned long long)stats.enhancedPresents);
  for(unsigned i=0;i<ReasonCount;++i)std::fprintf(stderr,"%s:%llu,",reasonNames[i],(unsigned long long)stats.reasons[i]);
  std::fprintf(stderr,"\n");
  std::fprintf(
      stderr,
      "GEKKOAOT_ENHANCED_STATS graphs=%llu eligible=%llu excluded=%llu "
      "missing=%llu invalid_projection_or_copy=%llu mixed_projection=%llu "
      "pool_evictions=%llu encode_calls=%llu encode_cpu_ns=%llu copy_aliases=%zu alias_composites=%llu "
      "alias_fallbacks=%llu alias_bindings=%llu world_camera_proven=0 csm=0\n",
      (unsigned long long)stats.graphs, (unsigned long long)stats.eligible,
      (unsigned long long)stats.excluded, (unsigned long long)stats.missing,
      (unsigned long long)stats.invalid, (unsigned long long)stats.mixed,
      (unsigned long long)stats.evictions,(unsigned long long)stats.encodeCalls,
      (unsigned long long)stats.encodeCpuNs,copyAliases.size(),(unsigned long long)aliasComposites,
      (unsigned long long)aliasFallbacks,(unsigned long long)aliasBindings);
  if(environmentActive)
    std::fprintf(stderr,"GEKKOAOT_ENVIRONMENT_REPORT mode=%u passes=%llu world_frame=%llu sun_game_light=%llu "
      "sun_world_frame=%llu no_sun=%llu camera_relative_light=%llu\n",unsigned(environment.mode),
      (unsigned long long)environmentPasses,(unsigned long long)environmentWorld,
      (unsigned long long)environmentSources[unsigned(SunSource::GameLight)],
      (unsigned long long)environmentSources[unsigned(SunSource::WorldFrame)],
      (unsigned long long)environmentSources[unsigned(SunSource::None)],(unsigned long long)environmentCameraLights);
}
} // namespace aurora::gfx::enhanced
