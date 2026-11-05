// SPDX-License-Identifier: GPL-3.0-or-later
#include "compact_sr_gpu.h"
#include "../webgpu/gpu.hpp"
#include <chrono>
#include <cstdio>
#include <memory>
#include <atomic>
#include <ctime>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <deque>
#include <cstdlib>
namespace aurora::gfx::hd {
namespace {
double ThreadCpuMs(){
#if defined(CLOCK_THREAD_CPUTIME_ID)
 timespec t{};if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)==0)return t.tv_sec*1000.+t.tv_nsec/1e6;
#endif
 return -1;
}
constexpr char Shader[]=R"(
struct Params { dims:vec4u, offsets:vec4u, region:vec4u }
@group(0) @binding(0) var<storage,read> input:array<f32>;
@group(0) @binding(1) var<storage,read_write> output:array<vec4f>;
@group(0) @binding(2) var<storage,read> weights:array<vec4f>;
@group(0) @binding(3) var<uniform> p:Params;
@compute @workgroup_size(8,8,1)
fn main(@builtin(global_invocation_id) id:vec3u){
 let w=p.dims.x;let h=p.dims.y;let ic=p.dims.z;let oc=p.dims.w;let o=id.z*4u;
 let x=id.x+p.region.x;let y=id.y+p.region.y;
 if(id.x>=p.region.z || id.y>=p.region.w || o>=oc){return;}
 var sum=weights[(p.offsets.y+o)/4u];
 for(var ky=0u;ky<3u;ky++){
  let sy=i32(y)+i32(ky)-1;if(sy<0 || sy>=i32(h)){continue;}
  for(var kx=0u;kx<3u;kx++){
   let sx=i32(x)+i32(kx)-1;if(sx<0 || sx>=i32(w)){continue;}
   let at=(u32(sy)*w+u32(sx))*ic;
   let kernel=p.offsets.x+(ky*3u+kx)*ic*oc+o;
   for(var i=0u;i<ic;i++) {sum+=input[at+i]*weights[(kernel+i*oc)/4u];}
  }
 }
 if(p.offsets.w!=0u){let slope=weights[(p.offsets.z+o)/4u];sum=select(sum,sum*slope,sum<vec4f(0));}
 output[(y*w+x)*(oc/4u)+id.z]=sum;
}
)";
// Implicit im2col GEMM: 8 pixels x 32 output channels per workgroup.
// Shared blocks reuse both activations and coefficients eight times, without
// allocating an im2col image or changing the pretrained network arithmetic.
constexpr char TiledShader[]=R"(
struct Params { dims:vec4u, offsets:vec4u, region:vec4u }
@group(0) @binding(0) var<storage,read> input:array<f32>;
@group(0) @binding(1) var<storage,read_write> output:array<vec4f>;
@group(0) @binding(2) var<storage,read> weights:array<vec4f>;
@group(0) @binding(3) var<uniform> p:Params;
var<workgroup> activations:array<f32,128>;
var<workgroup> kernels:array<vec4f,128>;
@compute @workgroup_size(8,8,1)
fn main(@builtin(workgroup_id) group:vec3u,@builtin(local_invocation_id) local:vec3u){
 let w=p.dims.x;let h=p.dims.y;let oc=p.dims.w;
 let roiPixel=group.x*8u+local.y;let block=group.y*8u+local.x;
 let pixel=(roiPixel/p.region.z+p.region.y)*w+roiPixel%p.region.z+p.region.x;
 let thread=local.y*8u+local.x;
 var sum=vec4f(0);if(block*4u<oc){sum=weights[p.offsets.y/4u+block];}
 for(var base=0u;base<576u;base+=16u){
  for(var j=thread;j<128u;j+=64u){
   let roiSample=group.x*8u+j/16u;let k=base+j%16u;
   let samplePixel=(roiSample/p.region.z+p.region.y)*w+roiSample%p.region.z+p.region.x;
   let sx=i32(samplePixel%w)+i32((k/64u)%3u)-1;
   let sy=i32(samplePixel/w)+i32(k/192u)-1;
   var value=0.0;
   if(roiSample<p.region.z*p.region.w && sx>=0 && sx<i32(w) && sy>=0 && sy<i32(h)){
    value=input[(u32(sy)*w+u32(sx))*64u+k%64u];
   }
   activations[j]=value;
   let o=group.y*8u+j%8u;
   var coefficient=vec4f(0);
   if(o*4u<oc){coefficient=weights[p.offsets.x/4u+(base+j/8u)*(oc/4u)+o];}
   kernels[j]=coefficient;
  }
  workgroupBarrier();
  for(var k=0u;k<16u;k++){
   sum+=activations[local.y*16u+k]*kernels[k*8u+local.x];
  }
  workgroupBarrier();
 }
 if(roiPixel<p.region.z*p.region.w && block*4u<oc){
  if(p.offsets.w!=0u){let slope=weights[p.offsets.z/4u+block];sum=select(sum,sum*slope,sum<vec4f(0));}
  output[pixel*(oc/4u)+block]=sum;
 }
}
)";
struct Context {
 wgpu::Instance instance;wgpu::Adapter adapter;wgpu::Device device;wgpu::Queue queue;
 wgpu::ComputePipeline pipeline,tiledPipeline;wgpu::Buffer coefficients;
 wgpu::Buffer workspaceA,workspaceB,readback,parameters;
 std::array<wgpu::BindGroup,34> bindings{};
 std::uint64_t workspacePixels=0;
 std::array<std::array<unsigned,3>,34> offsets{};
 std::atomic<bool> failed{false};bool attempted=false;wgpu::BackendType backendUsed=wgpu::BackendType::Undefined;
 ~Context(){if(device)device.Destroy();bindings={};workspaceA=nullptr;workspaceB=nullptr;readback=nullptr;parameters=nullptr;coefficients=nullptr;pipeline=nullptr;tiledPipeline=nullptr;queue=nullptr;device=nullptr;adapter=nullptr;instance=nullptr;}
 bool Init(const GekkoAOT::GX::CompactSR::Model& model){
  if(attempted)return bool(device)&&!failed.load();attempted=true;
  constexpr std::array features{wgpu::InstanceFeatureName::TimedWaitAny};
  wgpu::InstanceDescriptor id{.requiredFeatureCount=features.size(),.requiredFeatures=features.data()};instance=wgpu::CreateInstance(&id);if(!instance)return false;
  auto preferred=webgpu::g_backendType;
#if defined(__linux__)
  // Mesa OpenGL compute is substantially slower for this network locally.
  // The independent compute device may use Vulkan while presentation stays GL.
  if(preferred==wgpu::BackendType::OpenGL || preferred==wgpu::BackendType::OpenGLES)preferred=wgpu::BackendType::Vulkan;
#endif
  wgpu::Future future;
  for(auto backend:{preferred,webgpu::g_backendType}){
    wgpu::RequestAdapterOptions options{.featureLevel=wgpu::FeatureLevel::Compatibility,.powerPreference=wgpu::PowerPreference::HighPerformance,.backendType=backend};
    future=instance.RequestAdapter(&options,wgpu::CallbackMode::WaitAnyOnly,[this](wgpu::RequestAdapterStatus status,wgpu::Adapter result,wgpu::StringView){if(status==wgpu::RequestAdapterStatus::Success)adapter=std::move(result);});
    if(instance.WaitAny(future,5000000000)==wgpu::WaitStatus::Success && adapter){wgpu::AdapterInfo info;adapter.GetInfo(&info);if(info.adapterType!=wgpu::AdapterType::CPU){backendUsed=backend;break;}}
    adapter=nullptr;if(preferred==webgpu::g_backendType)break;
  }
  if(!adapter)return false;
  wgpu::DeviceDescriptor dd{};
  dd.SetUncapturedErrorCallback([](const wgpu::Device&,wgpu::ErrorType,wgpu::StringView message,Context* self){self->failed=true;std::fprintf(stderr,"GEKKOAOT_COMPACT_GPU_ERROR %.*s\n",int(message.length),message.data);},this);
  future=adapter.RequestDevice(&dd,wgpu::CallbackMode::WaitAnyOnly,[this](wgpu::RequestDeviceStatus status,wgpu::Device result,wgpu::StringView){if(status==wgpu::RequestDeviceStatus::Success)device=std::move(result);});
  if(instance.WaitAny(future,5000000000)!=wgpu::WaitStatus::Success || !device)return false;queue=device.GetQueue();
  wgpu::ShaderSourceWGSL wgsl;wgsl.code=Shader;wgpu::ShaderModuleDescriptor sd{.nextInChain=&wgsl,.label="Native compact SR convolution"};auto module=device.CreateShaderModule(&sd);
  wgpu::ComputePipelineDescriptor pd{.label="Native compact SR compute",.compute={.module=module,.entryPoint="main"}};pipeline=device.CreateComputePipeline(&pd);
  wgsl.code=TiledShader;auto tiledModule=device.CreateShaderModule(&sd);
  pd.compute.module=tiledModule;tiledPipeline=device.CreateComputePipeline(&pd);
  std::vector<float> data;
  for(unsigned i=0;i<34;++i){const auto& l=model.layers[i];offsets[i][0]=data.size();data.insert(data.end(),l.weights.begin(),l.weights.end());offsets[i][1]=data.size();data.insert(data.end(),l.bias.begin(),l.bias.end());offsets[i][2]=data.size();data.insert(data.end(),l.slope.begin(),l.slope.end());}
  coefficients=Buffer(data.size()*4,wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopyDst);queue.WriteBuffer(coefficients,0,data.data(),data.size()*4);
  std::fprintf(stderr,"GEKKOAOT_COMPACT_GPU initialized=1 backend=%u renderer_backend=%u model_bytes=%zu separate_device=1 runtime_dependencies=none\n",unsigned(backendUsed),unsigned(webgpu::g_backendType),data.size()*4);
  return bool(pipeline)&&bool(tiledPipeline)&&!failed.load();
 }
 wgpu::Buffer Buffer(std::uint64_t n,wgpu::BufferUsage usage){wgpu::BufferDescriptor d{.label="Native SR workspace",.usage=usage,.size=n};return device.CreateBuffer(&d);}
 std::vector<float> Run(const GekkoAOT::GX::CompactSR::Model& model,std::span<const float> source,unsigned w,unsigned h,unsigned crop=0){
  if(crop>34u || w<=2u*crop || h<=2u*crop)return {};
  if(!Init(model))return {};
  const auto started=std::chrono::steady_clock::now();const auto cpuStarted=ThreadCpuMs();std::uint64_t n=std::uint64_t(w)*h;
  if(n>workspacePixels){
   workspacePixels=n;bindings={};
   workspaceA=Buffer(n*64*4,wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopyDst|wgpu::BufferUsage::CopySrc);
   workspaceB=Buffer(n*64*4,wgpu::BufferUsage::Storage|wgpu::BufferUsage::CopyDst|wgpu::BufferUsage::CopySrc);
   readback=Buffer(n*48*4,wgpu::BufferUsage::MapRead|wgpu::BufferUsage::CopyDst);
   parameters=Buffer(34*256,wgpu::BufferUsage::Uniform|wgpu::BufferUsage::CopyDst);
   auto a=workspaceA,b=workspaceB;
   for(unsigned layer=0;layer<34;++layer){
    const auto& compute=layer==0?pipeline:tiledPipeline;
    const std::array<wgpu::BindGroupEntry,4> entries{{{.binding=0,.buffer=a},{.binding=1,.buffer=b},{.binding=2,.buffer=coefficients},{.binding=3,.buffer=parameters,.offset=layer*256u,.size=48}}};
    wgpu::BindGroupDescriptor bd{.layout=compute.GetBindGroupLayout(0),.entryCount=entries.size(),.entries=entries.data()};bindings[layer]=device.CreateBindGroup(&bd);std::swap(a,b);
   }
  }
  queue.WriteBuffer(workspaceA,0,source.data(),source.size()*4);
  std::array<unsigned,34*64> params{};
  for(unsigned layer=0;layer<34;++layer){const auto& l=model.layers[layer];const auto& off=offsets[layer];
   const unsigned margin=std::min(crop,layer+1u);
   const std::array<unsigned,12> values{w,h,l.input,l.output,off[0],off[1],off[2],layer<33?1u:0u,margin,margin,w-2u*margin,h-2u*margin};std::copy(values.begin(),values.end(),params.begin()+layer*64);
  }
  queue.WriteBuffer(parameters,0,params.data(),sizeof(params));
  auto encoder=device.CreateCommandEncoder();
  for(unsigned layer=0;layer<34;++layer){
   auto pass=encoder.BeginComputePass();pass.SetPipeline(layer==0?pipeline:tiledPipeline);pass.SetBindGroup(0,bindings[layer]);
   const unsigned margin=std::min(crop,layer+1u),rw=w-2u*margin,rh=h-2u*margin;
   if(layer==0)pass.DispatchWorkgroups((rw+7)/8,(rh+7)/8,16);
   else pass.DispatchWorkgroups((std::uint64_t(rw)*rh+7)/8,(model.layers[layer].output+31)/32);
   pass.End();
  }
  encoder.CopyBufferToBuffer(workspaceA,0,readback,0,n*48*4);auto command=encoder.Finish();queue.Submit(1,&command);
  auto mapped=std::make_shared<bool>(false);auto future=readback.MapAsync(wgpu::MapMode::Read,0,n*48*4,wgpu::CallbackMode::WaitAnyOnly,[mapped](wgpu::MapAsyncStatus status,wgpu::StringView){*mapped=status==wgpu::MapAsyncStatus::Success;});
  if(instance.WaitAny(future,30000000000)!=wgpu::WaitStatus::Success || !*mapped || failed.load()){failed=true;return {};}
  const auto* data=static_cast<const float*>(readback.GetConstMappedRange(0,n*48*4));std::vector<float> result(data,data+n*48);readback.Unmap();
  static unsigned logs=0;if(logs++<8)std::fprintf(stderr,"GEKKOAOT_COMPACT_GPU_JOB width=%u height=%u wall_ms=%.3f worker_cpu_ms=%.3f workspace_payload_bytes=%llu cpu_convolutions=0 kernel=shared-gemm workspace_reused=1\n",w,h,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count(),cpuStarted<0?-1.:ThreadCpuMs()-cpuStarted,(unsigned long long)(n*(64+64+48)*4));
  return result;
 }
};
std::vector<float> InferOnWorker(std::unique_ptr<Context>& context,const GekkoAOT::GX::CompactSR::Model& model,std::span<const float> source,unsigned w,unsigned h,unsigned crop){
 if(!context){
  context=std::make_unique<Context>();
  std::vector<float> probe(48);for(unsigned i=0;i<48;++i)probe[i]=float((i*17+3)%101)/100;
  const auto gpu=context->Run(model,probe,4,4);
  constexpr unsigned indices[]={0,1,15,16,47,99,255,767};
  constexpr float expected[]={-.168940797f,-.201813191f,.276396185f,-.184050024f,.332936853f,-.0457234345f,.043806687f,.127055898f};
  bool valid=gpu.size()==768;
  if(valid)for(unsigned i=0;i<8;++i)valid&=std::isfinite(gpu[indices[i]])&&std::abs(gpu[indices[i]]-expected[i])<.001f;
  if(!valid)context->failed=true;
  std::fprintf(stderr,"GEKKOAOT_COMPACT_GPU_VALIDATION independent_reference=%u\n",unsigned(valid));
 }
 auto result=context->Run(model,source,w,h,crop);
 // Opt-in regression oracle: compare only the promised interior, against
 // the same pretrained network evaluated over the entire padded image.
 static bool checked=false;
 if(crop && !checked && std::getenv("GEKKOAOT_SR_VALIDATE_REGION")) {
  checked=true;const auto full=context->Run(model,source,w,h);
  bool valid=result.size()==full.size() && !full.empty();double maxError=0;
  if(valid)for(unsigned y=crop;y<h-crop;++y)for(unsigned x=crop;x<w-crop;++x)for(unsigned ch=0;ch<48;++ch){
   const auto i=(std::size_t(y)*w+x)*48+ch;
   maxError=std::max(maxError,double(std::abs(result[i]-full[i])));
   valid=valid && std::isfinite(result[i]) && std::abs(result[i]-full[i])<.00001f;
  }
  std::fprintf(stderr,"GEKKOAOT_COMPACT_GPU_REGION_VALIDATION valid=%u max_error=%.9g width=%u height=%u halo=%u\n",unsigned(valid),maxError,w,h,crop);
  if(!valid){context->failed=true;return {};}
 }
 if(!result.empty())return result;
 static bool reported=false;if(!reported){reported=true;std::fprintf(stderr,"GEKKOAOT_COMPACT_GPU unavailable=1 fallback=native-cpu\n");}
 return GekkoAOT::GX::CompactSR::Infer(model,source,w,h);
}
// OpenGL contexts have thread affinity. Asset workers may change between
// images, so keep the compute device and every GPU call on one owned thread.
class Executor {
 struct Work {const GekkoAOT::GX::CompactSR::Model* model;std::span<const float> source;unsigned w,h,crop;std::promise<std::vector<float>> promise;};
 std::mutex mutex;std::condition_variable ready;bool stopping=false;
 std::deque<std::unique_ptr<Work>> queue;std::thread worker;
 void Run(){
  std::unique_ptr<Context> context;
  for(;;){std::unique_ptr<Work> task;
   {std::unique_lock lock(mutex);ready.wait(lock,[&]{return stopping||!queue.empty();});if(stopping&&queue.empty())return;task=std::move(queue.front());queue.pop_front();}
   try{task->promise.set_value(InferOnWorker(context,*task->model,task->source,task->w,task->h,task->crop));}
   catch(...){task->promise.set_exception(std::current_exception());}
  }
 }
public:
 Executor():worker([this]{Run();}){}
 ~Executor(){{std::lock_guard lock(mutex);stopping=true;}ready.notify_one();worker.join();}
 std::vector<float> Submit(const GekkoAOT::GX::CompactSR::Model& model,std::span<const float> source,unsigned w,unsigned h,unsigned crop=0){
  auto task=std::make_unique<Work>();task->model=&model;task->source=source;task->w=w;task->h=h;task->crop=crop;auto future=task->promise.get_future();
  {std::lock_guard lock(mutex);queue.push_back(std::move(task));}ready.notify_one();return future.get();
 }
};
std::unique_ptr<Executor> executor;
}
std::vector<float> compact_gpu_infer(const GekkoAOT::GX::CompactSR::Model& model,std::span<const float> source,unsigned w,unsigned h){
 if(!executor)executor=std::make_unique<Executor>();return executor->Submit(model,source,w,h);
}
// Enhance supplies 34 authored-wrap halo pixels and consumes only the center.
// Each convolution shrinks the required region by one pixel. Halo outputs
// which cannot influence the consumed center are never dispatched.
std::vector<float> compact_gpu_infer_tile(const GekkoAOT::GX::CompactSR::Model& model,std::span<const float> source,unsigned w,unsigned h){
 if(!executor)executor=std::make_unique<Executor>();return executor->Submit(model,source,w,h,34u);
}
void compact_gpu_shutdown(){executor.reset();}
}
