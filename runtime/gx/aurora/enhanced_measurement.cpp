// SPDX-License-Identifier: GPL-3.0-or-later
#include "gekkoaot_enhanced_measurement.hpp"
#include "gekkoaot_enhanced_shaders.hpp"
#include "gekkoaot_enhanced_renderer.hpp"
#include "resources.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace aurora::gfx::enhanced::measurement {
namespace {
// Passes 0-4: depth, AO, blur X/Y, compose; 5-8: indirect, its blurs, reflections.
constexpr unsigned TimedPasses=10;
constexpr uint64_t CounterBytes=64, TimingBytes=16*TimedPasses, ReadbackBytes=CounterBytes+TimingBytes;
struct Slot {
  std::atomic<bool> busy{false};
  bool encoded=false;
  uint64_t graph=0;
  unsigned ao=0,contact=0,width=0,height=0,msaa=0;
  wgpu::Buffer counters, readback, resolve;
  wgpu::QuerySet queries;
  std::array<wgpu::PassTimestampWrites,TimedPasses> writes;
};
std::array<std::shared_ptr<Slot>,3> slots;
std::shared_ptr<Slot> active;
wgpu::ComputePipeline pipeline;
std::array<wgpu::BindGroupLayout,2> layouts;
bool enabled(){static const bool value=[] {const char* p=std::getenv("GEKKOAOT_ENHANCED_MEASURE");return p&&std::string_view(p)!="0";}();return value;}

void initialize() {
  if(pipeline)return;
  // The common shader supplies the exact camera reconstruction used by AO.
  const auto source=std::string(Shader)+R"(
@group(1) @binding(0) var result_color: texture_2d<f32>;
@group(1) @binding(1) var<storage,read_write> counts: array<atomic<u32>,16>;
@compute @workgroup_size(8,8) fn measure(@builtin(global_invocation_id) i:vec3u) {
 if(any(i.xy>=vec2u(64))){return;}
 if(all(i.xy==vec2u(0)) && p.emission.x>0. && p.emission.y>0.){
  for(var k=0u;k<8u;k++){
   let source=particle_sources[k*2u+1u];
   if(source.a>=1.){
    atomicAdd(&counts[12],1u);
    let source_id=(u32(source.a)>>4u)-1u;
    if(p.emitter_views[source_id].y>.5){atomicAdd(&counts[13],1u);}
    atomicOr(&counts[14u+source_id/32u],1u<<(source_id%32u));
   }
  }
 }
 let dims=vec2f(textureDimensions(color));let uv=(vec2f(i.xy)+.5)/64.;
 let pixel=vec2i(uv*dims);let q=vec2i(p.copy_rect.xy+uv*p.copy_rect.zw);
 atomicAdd(&counts[0],1u);
 let z=linear_at(q);if(!(z>0.)){return;}
 atomicAdd(&counts[1],1u);
 let normal=normal_at(q,z);let length2=dot(normal,normal);
 if(length2>.99 && length2<1.01){atomicAdd(&counts[2],1u);}
 let factor=textureLoad(occlusion,vec2i((vec2f(q)+.5)*vec2f(textureDimensions(occlusion))/p.depth.zw),0).xyz;
 if(factor.x<.996){atomicAdd(&counts[3],1u);}
 if(factor.y<.996){atomicAdd(&counts[4],1u);}
 if(factor.z<.996){atomicAdd(&counts[11],1u);}
 let material=u32(round(textureLoad(normals,q,0).z*7.))&3u;
 if(material>0u){atomicAdd(&counts[7u+material],1u);}
 let a=textureLoad(color,pixel,0).rgb;let b=textureLoad(result_color,pixel,0).rgb;
 let delta=abs(a-b);let difference=max(delta.x,max(delta.y,delta.z));
 if(difference>1.5/255.){atomicAdd(&counts[5],1u);}
 let quantized=u32(round(clamp(difference,0.,1.)*255.));
 atomicAdd(&counts[6],quantized);atomicMax(&counts[7],quantized);
})";
  wgpu::ShaderSourceWGSL wgsl{};wgsl.code=source.c_str();
  const wgpu::ShaderModuleDescriptor sd{.nextInChain=&wgsl};
  const auto shader=webgpu::g_device.CreateShaderModule(&sd);
  std::array<wgpu::BindGroupLayoutEntry,6> first{};
  unsigned at=0;
  for(unsigned binding:{0u,1u,2u,4u,5u,10u}){
    auto& e=first[at++];e.binding=binding;e.visibility=wgpu::ShaderStage::Compute;
    if(!binding)e.buffer={.type=wgpu::BufferBindingType::Uniform,.hasDynamicOffset=true,.minBindingSize=sizeof(Params)};
    else if(binding==10)e.buffer={.type=wgpu::BufferBindingType::ReadOnlyStorage,.minBindingSize=256};
    else e.texture={.sampleType=wgpu::TextureSampleType::UnfilterableFloat,.viewDimension=wgpu::TextureViewDimension::e2D};
  }
  const wgpu::BindGroupLayoutDescriptor d0{.entryCount=first.size(),.entries=first.data()};
  layouts[0]=webgpu::g_device.CreateBindGroupLayout(&d0);
  const std::array second{
    wgpu::BindGroupLayoutEntry{.binding=0,.visibility=wgpu::ShaderStage::Compute,
      .texture={.sampleType=wgpu::TextureSampleType::UnfilterableFloat,.viewDimension=wgpu::TextureViewDimension::e2D}},
    wgpu::BindGroupLayoutEntry{.binding=1,.visibility=wgpu::ShaderStage::Compute,
      .buffer={.type=wgpu::BufferBindingType::Storage,.minBindingSize=CounterBytes}}};
  const wgpu::BindGroupLayoutDescriptor d1{.entryCount=second.size(),.entries=second.data()};
  layouts[1]=webgpu::g_device.CreateBindGroupLayout(&d1);
  const wgpu::PipelineLayoutDescriptor ld{.bindGroupLayoutCount=layouts.size(),.bindGroupLayouts=layouts.data()};
  const auto layout=webgpu::g_device.CreatePipelineLayout(&ld);
  const wgpu::ComputePipelineDescriptor pd{.label="Enhanced sampled contribution evidence",.layout=layout,
    .compute={.module=shader,.entryPoint="measure"}};
  pipeline=webgpu::g_device.CreateComputePipeline(&pd);
}
}

void begin(uint64_t graph,unsigned ao,unsigned contact,unsigned width,unsigned height,unsigned msaa) {
  active={};
  if(!enabled() || (graph>4 && graph%SamplePeriod))return;
  for(auto& slot:slots){
    if(!slot){
      slot=std::make_shared<Slot>();
      const wgpu::BufferDescriptor storage{.label="Enhanced sample counters",
        .usage=wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopySrc|wgpu::BufferUsage::CopyDst,.size=CounterBytes};
      const wgpu::BufferDescriptor readback{.label="Enhanced sample readback",
        .usage=wgpu::BufferUsage::MapRead|wgpu::BufferUsage::CopyDst,.size=ReadbackBytes};
      slot->counters=webgpu::g_device.CreateBuffer(&storage);slot->readback=webgpu::g_device.CreateBuffer(&readback);
      if(webgpu::g_device.HasFeature(wgpu::FeatureName::TimestampQuery)){
        const wgpu::QuerySetDescriptor qd{.type=wgpu::QueryType::Timestamp,.count=2*TimedPasses};
        slot->queries=webgpu::g_device.CreateQuerySet(&qd);
        const wgpu::BufferDescriptor resolve{.usage=wgpu::BufferUsage::QueryResolve|wgpu::BufferUsage::CopySrc,.size=TimingBytes};
        slot->resolve=webgpu::g_device.CreateBuffer(&resolve);
        for(unsigned i=0;i<TimedPasses;++i)slot->writes[i]={.querySet=slot->queries,.beginningOfPassWriteIndex=2*i,.endOfPassWriteIndex=2*i+1};
      }
    }
    if(!slot->busy.exchange(true)){
      active=slot;slot->graph=graph;slot->ao=ao;slot->contact=contact;
      slot->width=width;slot->height=height;slot->msaa=msaa;return;
    }
  }
}
const wgpu::PassTimestampWrites* timestamps(unsigned pass) {
  return active && active->queries ? &active->writes[pass] : nullptr;
}
void finish(const wgpu::CommandEncoder& encoder,uint32_t uniformOffset,
            const wgpu::TextureView& original,const wgpu::TextureView& output,
            const wgpu::TextureView& depth,const wgpu::TextureView& normals,const wgpu::TextureView& occlusion,const wgpu::Buffer& particleSources) {
  if(!active)return;
  initialize();
  const std::array first{
    wgpu::BindGroupEntry{.binding=0,.buffer=detail::resources().uniformBuffer,.size=sizeof(Params)},
    wgpu::BindGroupEntry{.binding=1,.textureView=original},wgpu::BindGroupEntry{.binding=2,.textureView=normals},
    wgpu::BindGroupEntry{.binding=4,.textureView=depth},wgpu::BindGroupEntry{.binding=5,.textureView=occlusion},
    wgpu::BindGroupEntry{.binding=10,.buffer=particleSources,.size=256}};
  const std::array second{wgpu::BindGroupEntry{.binding=0,.textureView=output},
    wgpu::BindGroupEntry{.binding=1,.buffer=active->counters,.size=CounterBytes}};
  const wgpu::BindGroupDescriptor d0{.layout=layouts[0],.entryCount=first.size(),.entries=first.data()};
  const wgpu::BindGroupDescriptor d1{.layout=layouts[1],.entryCount=second.size(),.entries=second.data()};
  const auto b0=webgpu::g_device.CreateBindGroup(&d0),b1=webgpu::g_device.CreateBindGroup(&d1);
  encoder.ClearBuffer(active->counters);
  auto compute=encoder.BeginComputePass();compute.SetPipeline(pipeline);
  compute.SetBindGroup(0,b0,1,&uniformOffset);compute.SetBindGroup(1,b1);compute.DispatchWorkgroups(8,8);compute.End();
  encoder.CopyBufferToBuffer(active->counters,0,active->readback,0,CounterBytes);
  if(active->queries){
    encoder.ResolveQuerySet(active->queries,0,2*TimedPasses,active->resolve,0);
    encoder.CopyBufferToBuffer(active->resolve,0,active->readback,CounterBytes,TimingBytes);
  }
  active->encoded=true;active={};
}
void after_submit() {
  for(const auto& slot:slots){
    if(!slot || !slot->encoded)continue;
    slot->encoded=false;
    slot->readback.MapAsync(wgpu::MapMode::Read,0,ReadbackBytes,wgpu::CallbackMode::AllowSpontaneous,
      [slot](wgpu::MapAsyncStatus status,wgpu::StringView){
        if(status==wgpu::MapAsyncStatus::Success){
          const auto* raw=static_cast<const uint32_t*>(slot->readback.GetConstMappedRange());
          const auto* time=reinterpret_cast<const uint64_t*>(raw+16);
          std::array<uint64_t,TimedPasses> elapsed{};bool timed=bool(slot->queries);
          if(timed)for(unsigned i=0;i<5;++i){if(!time[2*i] || time[2*i+1]<time[2*i])timed=false;else elapsed[i]=time[2*i+1]-time[2*i];}
          // Optional passes: zero when not recorded in this graph.
          if(timed)for(unsigned i=5;i<TimedPasses;++i)if(time[2*i] && time[2*i+1]>=time[2*i])elapsed[i]=time[2*i+1]-time[2*i];
          std::fprintf(stderr,"GEKKOAOT_ENHANCED_MEASUREMENT graph=%llu ao_quality=%u contact_requested=%u width=%u height=%u msaa=%u samples=%u valid_depth=%u valid_geometric_normal=%u ao_changed=%u contact_changed=%u output_changed=%u delta_sum_u8=%u delta_max_u8=%u timings_valid=%u gpu_ns=%llu,%llu,%llu,%llu,%llu gi_ssr_gpu_ns=%llu,%llu,%llu,%llu diffuse_samples=%u specular_samples=%u environment_reflection_samples=%u csm_changed=%u particle_selected=%u particle_alpha=%u particle_resolve_gpu_ns=%llu particle_mask=%08x%08x scope=graph-output-grid64\n",
            (unsigned long long)slot->graph,slot->ao,slot->contact,slot->width,slot->height,slot->msaa,
            raw[0],raw[1],raw[2],raw[3],raw[4],raw[5],raw[6],raw[7],timed,
            (unsigned long long)elapsed[0],(unsigned long long)elapsed[1],(unsigned long long)elapsed[2],
            (unsigned long long)elapsed[3],(unsigned long long)elapsed[4],(unsigned long long)elapsed[5],
            (unsigned long long)elapsed[6],(unsigned long long)elapsed[7],(unsigned long long)elapsed[8],raw[8],raw[9],raw[10],raw[11],raw[12],raw[13],(unsigned long long)elapsed[9],raw[15],raw[14]);
          slot->readback.Unmap();
        }else std::fprintf(stderr,"GEKKOAOT_ENHANCED_MEASUREMENT graph=%llu readback=failed\n",(unsigned long long)slot->graph);
        slot->busy=false;
      });
  }
}
void reset(){active={};slots={};pipeline={};layouts={};}
}
