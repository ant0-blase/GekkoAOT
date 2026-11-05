// SPDX-License-Identifier: GPL-3.0-or-later
// Universal post-processing, final presentation and interpolation blending.
// Included by bridge.cpp, whose translation unit holds the bridge state; this
// file is not compiled on its own.

struct PostFxResources {
  wgpu::BindGroupLayout layout;
  wgpu::RenderPipeline pipeline;
  wgpu::Buffer params;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
};

PostFxResources& UniversalPostFxResources() {
  static PostFxResources resources;
  return resources;
}

bool EnsureUniversalPostFxPipeline() {
  using namespace aurora::webgpu;
  auto& resources = UniversalPostFxResources();
  const auto format = g_graphicsConfig.surfaceConfiguration.format;
  if (resources.pipeline && resources.layout && resources.params && resources.format == format)
    return true;

  resources = {};
  resources.format = format;
  wgpu::ShaderSourceWGSL source{};
  std::string postShader=aurora::gfx::hd::DynamicTextureShader;
  postShader+=R"WGSL(
struct Params { primary: vec4<f32>, secondary: vec4<f32>, grading: vec4<f32>, light: vec4<f32>, flags: vec4<f32>, };
struct VertexOutput { @builtin(position) position: vec4<f32>, @location(0) uv: vec2<f32>, };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var tex_sampler: sampler;
@group(0) @binding(2) var source_tex: texture_2d<f32>;
var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
  vec2(-1.0, 1.0), vec2(-1.0, -3.0), vec2(3.0, 1.0));
var<private> uvs: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
  vec2(0.0, 0.0), vec2(0.0, 2.0), vec2(2.0, 0.0));
@vertex fn vs_main(@builtin(vertex_index) idx: u32) -> VertexOutput {
  var out: VertexOutput;
  out.position = vec4<f32>(pos[idx], 0.0, 1.0);
  out.uv = uvs[idx];
  return out;
}
fn sample_source(uv: vec2<f32>) -> vec4<f32> {
  let raw=textureSampleLevel(source_tex, tex_sampler, uv, 0.0);
  if(params.flags.y>.5){return hd_edge_reconstruct(source_tex,tex_sampler,uv,raw);}
  return raw;
}
fn aces(x: vec3<f32>) -> vec3<f32> {
  let a = 2.51; let b = 0.03; let c = 2.43; let d = 0.59; let e = 0.14;
  return clamp((x * (a * x + vec3<f32>(b))) /
               (x * (c * x + vec3<f32>(d)) + vec3<f32>(e)),
               vec3<f32>(0.0), vec3<f32>(1.0));
}
fn hash12(p: vec2<f32>) -> f32 {
  let h = dot(p, vec2<f32>(127.1, 311.7));
  return fract(sin(h) * 43758.5453);
}
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
  let dims = vec2<f32>(textureDimensions(source_tex));
  let px = vec2<f32>(1.0) / max(dims, vec2<f32>(1.0));
  let uv = clamp(in.uv, vec2<f32>(0.0), vec2<f32>(1.0));
  let center = sample_source(uv).rgb;
  let n = sample_source(uv + vec2<f32>(0.0, -px.y)).rgb;
  let s = sample_source(uv + vec2<f32>(0.0, px.y)).rgb;
  let e = sample_source(uv + vec2<f32>(px.x, 0.0)).rgb;
  let w = sample_source(uv + vec2<f32>(-px.x, 0.0)).rgb;
  let local_blur = (center * 4.0 + n + s + e + w) / 8.0;

  var filtered = center;
  if (params.flags.x > 0.5) {
    // Directional FXAA, preserving flat areas and high-contrast detail.
    let nw = sample_source(uv - px).rgb;
    let ne = sample_source(uv + vec2<f32>(px.x,-px.y)).rgb;
    let sw = sample_source(uv + vec2<f32>(-px.x,px.y)).rgb;
    let se = sample_source(uv + px).rgb;
    let luma = vec3<f32>(0.299,0.587,0.114);
    let a=dot(nw,luma); let b=dot(ne,luma); let c=dot(sw,luma); let d=dot(se,luma);
    let m=dot(center,luma);
    let lo=min(m,min(min(a,b),min(c,d))); let hi=max(m,max(max(a,b),max(c,d)));
    if (hi-lo > max(0.0312,hi*0.125)) {
      var dir=vec2<f32>(-((a+b)-(c+d)),(a+c)-(b+d));
      let reduce=max((a+b+c+d)*0.03125,0.0078125);
      dir=clamp(dir/(min(abs(dir.x),abs(dir.y))+reduce),vec2<f32>(-8.0),vec2<f32>(8.0))*px;
      let rgbA=0.5*(sample_source(uv+dir*(-1.0/6.0)).rgb+
                    sample_source(uv+dir*(1.0/6.0)).rgb);
      let rgbB=rgbA*0.5+0.25*(sample_source(uv-dir*0.5).rgb+
                               sample_source(uv+dir*0.5).rgb);
      let lumB=dot(rgbB,luma);
      filtered=select(rgbB,rgbA,lumB<lo || lumB>hi);
    }
  }
  var color = filtered + (center - local_blur) * params.primary.x;
  color += (center - local_blur) * params.secondary.y;

  let radius = px * 4.0;
  let b0 = sample_source(uv + vec2<f32>( radius.x, 0.0)).rgb;
  let b1 = sample_source(uv + vec2<f32>(-radius.x, 0.0)).rgb;
  let b2 = sample_source(uv + vec2<f32>(0.0,  radius.y)).rgb;
  let b3 = sample_source(uv + vec2<f32>(0.0, -radius.y)).rgb;
  let bloom_source = max((b0 + b1 + b2 + b3) * 0.25 - vec3<f32>(0.58), vec3<f32>(0.0));
  color += bloom_source * params.primary.y * 0.75;

  let filmic = aces(max(color, vec3<f32>(0.0)));
  color = mix(color, filmic, params.secondary.x);

  if (params.secondary.z > 0.0001) {
    let radial = clamp(length((uv - vec2<f32>(0.5)) * vec2<f32>(1.0, 0.82)) * 1.6, 0.0, 1.0);
    let soft = (local_blur + center) * 0.5;
    color = mix(color, soft, params.secondary.z * smoothstep(0.35, 1.0, radial) * 0.45);
  }

  let d = length((uv - vec2<f32>(0.5)) * vec2<f32>(1.0, 0.82));
  let vignette = 1.0 - params.primary.z * smoothstep(0.30, 0.72, d);
  color *= vignette;

  let noise = hash12(uv * dims + vec2<f32>(params.secondary.w, params.secondary.w * 0.618)) - 0.5;
  color += vec3<f32>(noise * params.primary.w);
  color *= exp2(params.light.x) * params.grading.x;
  color *= vec3<f32>(1.0+params.light.y*0.15,1.0+params.light.w*0.15,1.0-params.light.y*0.15);
  color = (color - vec3<f32>(0.5)) * params.grading.y + vec3<f32>(0.5);
  let luma = dot(color,vec3<f32>(0.2126,0.7152,0.0722));
  let spread = max(color.r,max(color.g,color.b))-min(color.r,min(color.g,color.b));
  color = mix(vec3<f32>(luma),color,params.grading.z*(1.0+params.light.z*(1.0-clamp(spread,0.0,1.0))));
  color = pow(max(color,vec3<f32>(0.0)),vec3<f32>(1.0/params.grading.w));
  return vec4<f32>(clamp(color, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
}
)WGSL";
  source.code=wgpu::StringView{postShader.data(),postShader.size()};
  const wgpu::ShaderModuleDescriptor module_desc{.nextInChain = &source,
      .label = "GekkoAOT v196 universal post-processing shader"};
  const auto module = g_device.CreateShaderModule(&module_desc);
  const std::array entries{
      wgpu::BindGroupLayoutEntry{.binding = 0, .visibility = wgpu::ShaderStage::Fragment,
          .buffer = wgpu::BufferBindingLayout{.type = wgpu::BufferBindingType::Uniform}},
      wgpu::BindGroupLayoutEntry{.binding = 1, .visibility = wgpu::ShaderStage::Fragment,
          .sampler = wgpu::SamplerBindingLayout{.type = wgpu::SamplerBindingType::Filtering}},
      wgpu::BindGroupLayoutEntry{.binding = 2, .visibility = wgpu::ShaderStage::Fragment,
          .texture = wgpu::TextureBindingLayout{.sampleType = wgpu::TextureSampleType::Float,
                                                .viewDimension = wgpu::TextureViewDimension::e2D}},
  };
  const wgpu::BindGroupLayoutDescriptor bgl_desc{.entryCount = entries.size(), .entries = entries.data()};
  resources.layout = g_device.CreateBindGroupLayout(&bgl_desc);
  const wgpu::PipelineLayoutDescriptor layout_desc{.bindGroupLayoutCount = 1,
                                                    .bindGroupLayouts = &resources.layout};
  const auto pipeline_layout = g_device.CreatePipelineLayout(&layout_desc);
  const std::array targets{wgpu::ColorTargetState{.format = format,
                                                   .writeMask = wgpu::ColorWriteMask::All}};
  const wgpu::FragmentState fragment{.module = module, .entryPoint = "fs_main",
                                     .targetCount = targets.size(), .targets = targets.data()};
  const wgpu::RenderPipelineDescriptor pipeline_desc{
      .label = "GekkoAOT v196 universal post-processing pipeline", .layout = pipeline_layout,
      .vertex = wgpu::VertexState{.module = module, .entryPoint = "vs_main"},
      .primitive = wgpu::PrimitiveState{.topology = wgpu::PrimitiveTopology::TriangleList},
      .multisample = wgpu::MultisampleState{.count = 1, .mask = UINT32_MAX}, .fragment = &fragment};
  resources.pipeline = g_device.CreateRenderPipeline(&pipeline_desc);
  const wgpu::BufferDescriptor params_desc{.label = "GekkoAOT v196 post-processing params",
      .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::Uniform, .size = 80};
  resources.params = g_device.CreateBuffer(&params_desc);
  return resources.pipeline && resources.layout && resources.params;
}

bool PresentUniversalPostFx(const aurora::webgpu::TextureWithSampler& source) {
  using namespace aurora;
  using namespace aurora::webgpu;
  const bool reconstructVideo=g_video.texture_scale>1 && source.texture && source.texture.Get()==g_ram_xfb_present.texture.Get();
  if ((!g_post_processing_enabled && !g_video.fxaa && !reconstructVideo) || !source.texture || !source.view ||
      source.size.width == 0 || source.size.height == 0)
    return false;
  if (!gfx::render_worker::is_worker_thread()) {
    auto retained = source;
    bool result = false;
    gfx::render_worker::enqueue_work([retained = std::move(retained), &result]() mutable {
      result = PresentUniversalPostFx(retained);
    });
    gfx::render_worker::synchronize();
    return result;
  }
  if (!EnsureUniversalPostFxPipeline() || !window::is_presentable()) return false;
  if (!g_surface && !refresh_surface(true)) return false;
  if (!g_surface) return false;

  wgpu::SurfaceTexture surface_texture;
  {
    window::SurfaceLock surface_lock;
    if (!window::is_presentable() || !g_surface) return false;
    g_surface.GetCurrentTexture(&surface_texture);
  }
  if (surface_texture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
      !surface_texture.texture) {
    release_surface();
    return false;
  }
  auto swap_texture = std::move(surface_texture.texture);
  auto swap_view = swap_texture.CreateView();
  if (!swap_view) return false;

  auto& resources = UniversalPostFxResources();
  const float enabled = g_post_processing_enabled ? 1.f : 0.f;
  const std::array<float, 20> params{
      g_post_sharpen*enabled, g_post_bloom*enabled, g_post_vignette*enabled, g_post_grain*enabled,
      g_post_tone*enabled, g_post_local_contrast*enabled, g_post_cinematic*enabled,
      static_cast<float>((g_post_frame++) & 0xffffu),
      enabled ? g_video.brightness : 1.f, enabled ? g_video.contrast : 1.f,
      enabled ? g_video.saturation : 1.f, enabled ? g_video.gamma : 1.f,
      g_video.exposure*enabled, g_video.temperature*enabled, g_video.vibrance*enabled,
      g_video.tint*enabled, float(g_video.fxaa),float(reconstructVideo),0,0};
  g_queue.WriteBuffer(resources.params, 0, params.data(), sizeof(params));
  const std::array bind_entries{
      wgpu::BindGroupEntry{.binding = 0, .buffer = resources.params, .size = 80},
      wgpu::BindGroupEntry{.binding = 1, .sampler = source.sampler},
      wgpu::BindGroupEntry{.binding = 2, .textureView = source.view},
  };
  const wgpu::BindGroupDescriptor bind_desc{.layout = resources.layout,
      .entryCount = bind_entries.size(), .entries = bind_entries.data()};
  const auto bind_group = g_device.CreateBindGroup(&bind_desc);
  constexpr wgpu::CommandEncoderDescriptor encoder_desc{.label = "GekkoAOT v196 post-processing encoder"};
  auto encoder = g_device.CreateCommandEncoder(&encoder_desc);
  const auto viewport = PresentViewportForAspect(g_graphicsConfig.surfaceConfiguration.width,
      g_graphicsConfig.surfaceConfiguration.height, source.size.width, source.size.height);
  const std::array attachments{wgpu::RenderPassColorAttachment{.view = swap_view,
      .loadOp = wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store}};
  const wgpu::RenderPassDescriptor pass_desc{.label = "GekkoAOT v196 post-processing pass",
      .colorAttachmentCount = attachments.size(), .colorAttachments = attachments.data()};
  const auto pass = encoder.BeginRenderPass(&pass_desc);
  pass.SetPipeline(resources.pipeline);
  pass.SetBindGroup(0, bind_group, 0, nullptr);
  pass.SetViewport(viewport.left, viewport.top, viewport.width, viewport.height,
                   viewport.znear, viewport.zfar);
  pass.Draw(3);
  pass.End();
  GekkoAOT::GX::Hud::Draw(encoder, swap_view, g_graphicsConfig.surfaceConfiguration.format,
                          g_graphicsConfig.surfaceConfiguration.width, g_graphicsConfig.surfaceConfiguration.height);
  const auto command = encoder.Finish();
  g_queue.Submit(1, &command);

  bool present_ok = false;
  {
    window::SurfaceLock surface_lock;
    if (window::is_presentable() && g_surface)
      present_ok = static_cast<bool>(g_surface.Present());
  }
  if (present_ok) {
    gfx::after_present();
    return true;
  }
  release_surface();
  return false;
}

void PresentFinalTexture(const aurora::webgpu::TextureWithSampler& original) {
  const auto& source=aurora::gfx::enhanced::present_source(original);
  const auto enhancedGraph=aurora::gfx::enhanced::output_graph(source);
  const auto before=aurora::gfx::presentation_count();
  const auto present_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  const auto previous_ns=g_last_present_ns.exchange(present_ns,std::memory_order_relaxed);
  if (previous_ns!=0) {
    const double ms=double(present_ns-previous_ns)*1e-6;
    if (ms>0.0 && ms<250.0) {
      auto& pacing=g_present_pacing;
      pacing.sum+=ms; pacing.sum_sq+=ms*ms; pacing.worst=std::max(pacing.worst,ms); ++pacing.count;
    }
  }
  const bool reconstructVideo=g_video.texture_scale>1 && original.texture && original.texture.Get()==g_ram_xfb_present.texture.Get();
  if ((g_post_processing_enabled || g_video.fxaa || reconstructVideo) && PresentUniversalPostFx(source)) {
    static unsigned videoLogs=0;if(reconstructVideo && videoLogs++<3)std::fprintf(stderr,"GEKKOAOT_VIDEO_RECONSTRUCTION method=edge-directed-gpu neural=0 source=%ux%u cache=frame-only\n",source.size.width,source.size.height);
    aurora::gfx::enhanced::record_presentation(enhancedGraph,aurora::gfx::presentation_count()>before);
    return;
  }
  aurora_present_texture(&source);
  aurora::gfx::enhanced::record_presentation(enhancedGraph,aurora::gfx::presentation_count()>before);
}

// The display refresh rate (frames above it are never seen; presenting
// several per refresh only adds judder and tearing). 60 Hz when unknown.
double DisplayRefreshHz() {
  static double cached=60.0;
  static auto checked=std::chrono::steady_clock::time_point{};
  const auto now=std::chrono::steady_clock::now();
  if (now-checked<std::chrono::seconds(2)) return cached;
  checked=now;
  if (g_info.window) {
    const SDL_DisplayID display=SDL_GetDisplayForWindow(g_info.window);
    if (const SDL_DisplayMode* mode=display ? SDL_GetCurrentDisplayMode(display) : nullptr;
        mode && mode->refresh_rate>=23.f && mode->refresh_rate<=1000.f)
      cached=mode->refresh_rate;
  }
  return cached;
}

struct InterpolationBlendResources {
  aurora::webgpu::TextureWithSampler post_target;
  wgpu::BindGroupLayout layout;
  wgpu::RenderPipeline pipeline;
  wgpu::Buffer params;
  wgpu::BindGroupLayout motion_layout;
  wgpu::ComputePipeline motion_pipeline;
  wgpu::Texture flow;
  wgpu::TextureView flow_view;
  std::uint32_t flow_width = 0, flow_height = 0;
  std::uint64_t flow_generation = 0;
  wgpu::Texture history_identity;
  unsigned mode = 1;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
};

InterpolationBlendResources& BlendResources() {
  static InterpolationBlendResources resources;
  return resources;
}

bool EnsureInterpolationBlendPipeline() {
  using namespace aurora::webgpu;
  auto& resources = BlendResources();
  const auto format = g_graphicsConfig.surfaceConfiguration.format;
  if (resources.pipeline && resources.layout && resources.params && resources.format == format)
    return true;

  resources = {};
  resources.format = format;
  if (const char* mode = std::getenv("GEKKOAOT_INTERPOLATION_MODE")) {
    if (EqualNoCase(mode, "blend")) resources.mode = 0;
    else if (EqualNoCase(mode, "repeat")) resources.mode = 2;
    else if (!EqualNoCase(mode, "motion"))
      std::fprintf(stderr, "GEKKOAOT_INTERPOLATION_MODE invalid=%s using=motion\n", mode);
  }
  wgpu::ShaderSourceWGSL source{};
  source.code = R"WGSL(
struct Params { value: vec4<f32>, };
struct VertexOutput { @builtin(position) position: vec4<f32>, @location(0) uv: vec2<f32>, };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var tex_sampler: sampler;
@group(0) @binding(2) var previous_tex: texture_2d<f32>;
@group(0) @binding(3) var current_tex: texture_2d<f32>;
@group(0) @binding(4) var motion_tex: texture_2d<f32>;
var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
  vec2(-1.0, 1.0), vec2(-1.0, -3.0), vec2(3.0, 1.0));
var<private> uvs: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
  vec2(0.0, 0.0), vec2(0.0, 2.0), vec2(2.0, 0.0));
@vertex fn vs_main(@builtin(vertex_index) idx: u32) -> VertexOutput {
  var out: VertexOutput;
  out.position = vec4<f32>(pos[idx], 0.0, 1.0);
  out.uv = uvs[idx];
  return out;
}
@fragment fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
  let a = textureSample(previous_tex, tex_sampler, in.uv);
  let b = textureSample(current_tex, tex_sampler, in.uv);
  let alpha = clamp(params.value.x, 0.0, 1.0);
  if (params.value.y < 0.5) { return vec4<f32>(mix(a.rgb, b.rgb, alpha), 1.0); }
  if (params.value.y > 1.5) { return vec4<f32>(b.rgb, 1.0); }
  let flow = textureSample(motion_tex, tex_sampler, in.uv);
  let delta = flow.xy / vec2<f32>(textureDimensions(current_tex));
  let old_uv = in.uv + delta * alpha;
  let new_uv = in.uv - delta * (1.0 - alpha);
  let old = textureSample(previous_tex, tex_sampler, old_uv).rgb;
  let newer = textureSample(current_tex, tex_sampler, new_uv).rgb;
  let disagreement = dot(abs(old - newer), vec3<f32>(0.299, 0.587, 0.114));
  // Cuts, occlusions and failed matches use the current real frame. Blending
  // those areas would produce doubled silhouettes, text and HUD elements.
  let outside = any(old_uv < vec2<f32>(0.0)) || any(old_uv > vec2<f32>(1.0)) ||
                any(new_uv < vec2<f32>(0.0)) || any(new_uv > vec2<f32>(1.0));
  if (flow.z > 0.07 || disagreement > 0.12 || outside) {
    return vec4<f32>(b.rgb, 1.0);
  }
  return vec4<f32>(mix(old, newer, alpha), 1.0);
}
)WGSL";
  const wgpu::ShaderModuleDescriptor module_desc{.nextInChain = &source, .label = "GekkoAOT v49 interpolation shader"};
  const auto module = g_device.CreateShaderModule(&module_desc);
  const std::array entries{
      wgpu::BindGroupLayoutEntry{.binding = 0, .visibility = wgpu::ShaderStage::Fragment,
                                 .buffer = wgpu::BufferBindingLayout{.type = wgpu::BufferBindingType::Uniform}},
      wgpu::BindGroupLayoutEntry{.binding = 1, .visibility = wgpu::ShaderStage::Fragment,
                                 .sampler = wgpu::SamplerBindingLayout{.type = wgpu::SamplerBindingType::Filtering}},
      wgpu::BindGroupLayoutEntry{.binding = 2, .visibility = wgpu::ShaderStage::Fragment,
                                 .texture = wgpu::TextureBindingLayout{.sampleType = wgpu::TextureSampleType::Float,
                                                                      .viewDimension = wgpu::TextureViewDimension::e2D}},
      wgpu::BindGroupLayoutEntry{.binding = 3, .visibility = wgpu::ShaderStage::Fragment,
                                 .texture = wgpu::TextureBindingLayout{.sampleType = wgpu::TextureSampleType::Float,
                                                                      .viewDimension = wgpu::TextureViewDimension::e2D}},
      wgpu::BindGroupLayoutEntry{.binding = 4, .visibility = wgpu::ShaderStage::Fragment,
                                 .texture = wgpu::TextureBindingLayout{.sampleType = wgpu::TextureSampleType::Float,
                                                                      .viewDimension = wgpu::TextureViewDimension::e2D}},
  };
  const wgpu::BindGroupLayoutDescriptor bgl_desc{.entryCount = entries.size(), .entries = entries.data()};
  resources.layout = g_device.CreateBindGroupLayout(&bgl_desc);
  const wgpu::PipelineLayoutDescriptor layout_desc{.bindGroupLayoutCount = 1, .bindGroupLayouts = &resources.layout};
  const auto pipeline_layout = g_device.CreatePipelineLayout(&layout_desc);
  const std::array targets{wgpu::ColorTargetState{.format = format, .writeMask = wgpu::ColorWriteMask::All}};
  const wgpu::FragmentState fragment{.module = module, .entryPoint = "fs_main",
                                     .targetCount = targets.size(), .targets = targets.data()};
  const wgpu::RenderPipelineDescriptor pipeline_desc{
      .label = "GekkoAOT v49 interpolation pipeline", .layout = pipeline_layout,
      .vertex = wgpu::VertexState{.module = module, .entryPoint = "vs_main"},
      .primitive = wgpu::PrimitiveState{.topology = wgpu::PrimitiveTopology::TriangleList},
      .multisample = wgpu::MultisampleState{.count = 1, .mask = UINT32_MAX}, .fragment = &fragment};
  resources.pipeline = g_device.CreateRenderPipeline(&pipeline_desc);
  const wgpu::BufferDescriptor params_desc{.label = "GekkoAOT v49 interpolation params",
                                            .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::Uniform,
                                            .size = 16};
  resources.params = g_device.CreateBuffer(&params_desc);
  return resources.pipeline && resources.layout && resources.params;
}

bool PrepareMotionField(const aurora::webgpu::TextureWithSampler& previous,
                        const aurora::webgpu::TextureWithSampler& current) {
  using namespace aurora::webgpu;
  auto& r = BlendResources();
  if (previous.size.width != current.size.width ||
      previous.size.height != current.size.height) return false;
  const auto width = (current.size.width + 7u) / 8u;
  const auto height = (current.size.height + 7u) / 8u;
  if (!r.flow || r.flow_width != width || r.flow_height != height) {
    const wgpu::TextureDescriptor desc{
        .label = "GekkoAOT motion field",
        .usage = wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::TextureBinding,
        .size = {width, height, 1}, .format = wgpu::TextureFormat::RGBA16Float};
    r.flow = g_device.CreateTexture(&desc);
    if (!r.flow) return false;
    r.flow_view = r.flow.CreateView();
    r.flow_width = width; r.flow_height = height;
    r.flow_generation = 0; r.history_identity = {};
  }
  if (r.mode != 1) return bool(r.flow_view);
  if (r.flow_generation == g_interp_snapshot_count && r.history_identity.Get() == current.texture.Get())
    return true;
  if (!r.motion_pipeline) {
    wgpu::ShaderSourceWGSL source{};
    source.code = GekkoAOT::GX::MotionShader;
    const wgpu::ShaderModuleDescriptor module_desc{
        .nextInChain = &source, .label = "GekkoAOT block motion shader"};
    const auto module = g_device.CreateShaderModule(&module_desc);
    const std::array entries{
        wgpu::BindGroupLayoutEntry{.binding = 0, .visibility = wgpu::ShaderStage::Compute,
            .texture = wgpu::TextureBindingLayout{.sampleType = wgpu::TextureSampleType::Float}},
        wgpu::BindGroupLayoutEntry{.binding = 1, .visibility = wgpu::ShaderStage::Compute,
            .texture = wgpu::TextureBindingLayout{.sampleType = wgpu::TextureSampleType::Float}},
        wgpu::BindGroupLayoutEntry{.binding = 2, .visibility = wgpu::ShaderStage::Compute,
            .storageTexture = wgpu::StorageTextureBindingLayout{
                .access = wgpu::StorageTextureAccess::WriteOnly,
                .format = wgpu::TextureFormat::RGBA16Float,
                .viewDimension = wgpu::TextureViewDimension::e2D}},
    };
    const wgpu::BindGroupLayoutDescriptor layout_desc{
        .entryCount = entries.size(), .entries = entries.data()};
    r.motion_layout = g_device.CreateBindGroupLayout(&layout_desc);
    const wgpu::PipelineLayoutDescriptor pipeline_layout_desc{
        .bindGroupLayoutCount = 1, .bindGroupLayouts = &r.motion_layout};
    const auto layout = g_device.CreatePipelineLayout(&pipeline_layout_desc);
    const wgpu::ComputePipelineDescriptor pipeline_desc{
        .label = "GekkoAOT bounded block motion", .layout = layout,
        .compute = {.module = module, .entryPoint = "motion"}};
    r.motion_pipeline = g_device.CreateComputePipeline(&pipeline_desc);
    std::fprintf(stderr,
        "GEKKOAOT_INTERPOLATION_MOTION=1 grid=8 search=16+3 cache=per-produced-frame unreliable=current-frame\n");
  }
  if (!r.motion_pipeline || !r.motion_layout || !r.flow_view) return false;
  const std::array entries{
      wgpu::BindGroupEntry{.binding = 0, .textureView = previous.view},
      wgpu::BindGroupEntry{.binding = 1, .textureView = current.view},
      wgpu::BindGroupEntry{.binding = 2, .textureView = r.flow_view},
  };
  const wgpu::BindGroupDescriptor group_desc{
      .layout = r.motion_layout, .entryCount = entries.size(), .entries = entries.data()};
  const auto group = g_device.CreateBindGroup(&group_desc);
  auto encoder = g_device.CreateCommandEncoder();
  auto pass = encoder.BeginComputePass();
  pass.SetPipeline(r.motion_pipeline); pass.SetBindGroup(0, group);
  pass.DispatchWorkgroups((width + 7u) / 8u, (height + 7u) / 8u);
  pass.End();
  const auto command = encoder.Finish();
  g_queue.Submit(1, &command);
  r.flow_generation = g_interp_snapshot_count;
  r.history_identity = current.texture;
  return true;
}

bool PresentInterpolationBlend(const aurora::webgpu::TextureWithSampler& previous,
                               const aurora::webgpu::TextureWithSampler& current,
                               float alpha) {
  using namespace aurora;
  using namespace aurora::webgpu;
  if (!previous.texture || !previous.view || !current.texture || !current.view ||
      previous.size.width == 0 || previous.size.height == 0 ||
      current.size.width == 0 || current.size.height == 0)
    return false;

  alpha = std::clamp(alpha, 0.0f, 1.0f);
  if (!gfx::render_worker::is_worker_thread()) {
    auto retained_previous = previous;
    auto retained_current = current;
    bool result = false;
    gfx::render_worker::enqueue_work(
        [retained_previous = std::move(retained_previous), retained_current = std::move(retained_current),
         alpha, &result]() mutable {
          result = PresentInterpolationBlend(retained_previous, retained_current, alpha);
        });
    gfx::render_worker::synchronize();
    return result;
  }

  if (!EnsureInterpolationBlendPipeline() || !window::is_presentable()) return false;
  if (!PrepareMotionField(previous, current)) return false;
  if (!g_surface && !refresh_surface(true)) return false;
  if (!g_surface) return false;

  const bool post = g_post_processing_enabled || g_video.fxaa;
  auto& resources = BlendResources();
  aurora::webgpu::TextureWithSampler intermediate;
  if (post) {
    if (!resources.post_target.texture || resources.post_target.size.width != current.size.width ||
        resources.post_target.size.height != current.size.height)
      resources.post_target = create_render_texture(current.size.width,current.size.height,false);
    intermediate = resources.post_target;
    if(!intermediate.texture || !intermediate.view) return false;
  }
  wgpu::SurfaceTexture surface_texture;
  if (!post) {
    window::SurfaceLock surface_lock;
    if (!window::is_presentable() || !g_surface) return false;
    g_surface.GetCurrentTexture(&surface_texture);
  }
  if (!post && (surface_texture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal || !surface_texture.texture)) {
    release_surface();
    return false;
  }
  auto swap_texture = std::move(surface_texture.texture);
  auto swap_view = post ? intermediate.view : swap_texture.CreateView();
  if (!swap_view) return false;

  const std::array<float, 4> params{alpha, float(resources.mode), 0.0f, 0.0f};
  g_queue.WriteBuffer(resources.params, 0, params.data(), sizeof(params));
  const std::array bind_entries{
      wgpu::BindGroupEntry{.binding = 0, .buffer = resources.params, .size = 16},
      wgpu::BindGroupEntry{.binding = 1, .sampler = current.sampler},
      wgpu::BindGroupEntry{.binding = 2, .textureView = previous.view},
      wgpu::BindGroupEntry{.binding = 3, .textureView = current.view},
      wgpu::BindGroupEntry{.binding = 4, .textureView = resources.flow_view},
  };
  const wgpu::BindGroupDescriptor bind_desc{.layout = resources.layout, .entryCount = bind_entries.size(),
                                             .entries = bind_entries.data()};
  const auto bind_group = g_device.CreateBindGroup(&bind_desc);
  constexpr wgpu::CommandEncoderDescriptor encoder_desc{.label = "GekkoAOT v49 interpolation encoder"};
  auto encoder = g_device.CreateCommandEncoder(&encoder_desc);
  auto viewport = PresentViewportForAspect(g_graphicsConfig.surfaceConfiguration.width,
                                                 g_graphicsConfig.surfaceConfiguration.height,
                                                 current.size.width, current.size.height);
  if(post) viewport={0.f,0.f,float(current.size.width),float(current.size.height),0.f,1.f};
  static unsigned aspect_logs = 0u;
  if (aspect_logs++ < 8u) {
    std::fprintf(stderr,
                 "GEKKOAOT_FRAME_INTERPOLATION_ASPECT_V137=1 mode=%s surface=%ux%u source=%ux%u viewport=%.0fx%.0f+%.0f+%.0f\n",
                 g_aspect_mode.load(std::memory_order_relaxed) == 1u ? "window" : "4:3",
                 g_graphicsConfig.surfaceConfiguration.width,
                 g_graphicsConfig.surfaceConfiguration.height,
                 current.size.width, current.size.height,
                 viewport.width, viewport.height, viewport.left, viewport.top);
  }
  const std::array attachments{wgpu::RenderPassColorAttachment{.view = swap_view, .loadOp = wgpu::LoadOp::Clear,
                                                                .storeOp = wgpu::StoreOp::Store}};
  const wgpu::RenderPassDescriptor pass_desc{.label = "GekkoAOT v49 interpolation pass",
                                              .colorAttachmentCount = attachments.size(),
                                              .colorAttachments = attachments.data()};
  const auto pass = encoder.BeginRenderPass(&pass_desc);
  pass.SetPipeline(resources.pipeline);
  pass.SetBindGroup(0, bind_group, 0, nullptr);
  pass.SetViewport(viewport.left, viewport.top, viewport.width, viewport.height, viewport.znear, viewport.zfar);
  pass.Draw(3);
  pass.End();
  constexpr wgpu::CommandBufferDescriptor command_desc{.label = "GekkoAOT v49 interpolation command buffer"};
  const auto command = encoder.Finish(&command_desc);
  g_queue.Submit(1, &command);

  if(post) return PresentUniversalPostFx(intermediate);

  bool present_ok = false;
  {
    window::SurfaceLock surface_lock;
    if (window::is_presentable() && g_surface) {
      const auto present_status = g_surface.Present();
      present_ok = static_cast<bool>(present_status);
    }
  }
  if (present_ok) {
    gfx::after_present();
    return true;
  }
  release_surface();
  return false;
}

bool SnapshotInterpolationFrame(const aurora::webgpu::TextureWithSampler& source) {
  using namespace aurora::webgpu;
  if (!g_frame_interpolation_enabled || !source.texture || !source.view ||
      source.size.width == 0 || source.size.height == 0) {
    return false;
  }

  // end_frame_no_present() may have sealed work to Aurora's render worker. Put
  // the history copy behind that work so the snapshot can never race the GX
  // render submission that produced it.
  if (!aurora::gfx::render_worker::is_worker_thread()) {
    auto retained = source;
    bool result = false;
    aurora::gfx::render_worker::enqueue_work([retained = std::move(retained), &result]() mutable {
      result = SnapshotInterpolationFrame(retained);
    });
    aurora::gfx::render_worker::synchronize();
    return result;
  }

  // Rotate two immutable history textures. The retired previous texture becomes
  // the destination for the next copy, so steady state allocates nothing.
  auto recycle = std::move(g_interp_previous);
  g_interp_previous = std::move(g_interp_current);
  g_interp_previous_valid = g_interp_current_valid;
  g_interp_current = std::move(recycle);

  // Keep interpolation history in the host presentation format and copy
  // the XFB through Aurora's proven sampling blit.  GX display-copy textures
  // may be RGBA8Unorm (notably RGB565 conversion targets) while the Vulkan
  // surface/history is BGRA8Unorm.  The old raw CopyTextureToTexture path
  // reinterpreted those bytes, swapping R/B, and also required CopySrc usage
  // that copy-cache render targets do not guarantee.  Sampling performs the
  // logical RGBA -> BGRA conversion exactly like the direct VI present path.
  const auto history_format = g_graphicsConfig.surfaceConfiguration.format;
  if (!RenderTextureReusable(g_interp_current, source.size, history_format)) {
    g_interp_current = create_render_texture(source.size.width, source.size.height, false);
  }
  if (!g_interp_current.texture || !g_interp_current.view || !g_CopyPipeline) {
    g_interp_current_valid = false;
    return false;
  }

  const auto bind_group = create_copy_bind_group(source);
  if (!bind_group) {
    g_interp_current_valid = false;
    return false;
  }

  static unsigned color_logs = 0u;
  if (color_logs++ < 8u) {
    std::fprintf(stderr,
                 "GEKKOAOT_FRAME_INTERPOLATION_COLOR_V134=1 src_format=%u history_format=%u mode=aurora-sample-blit\n",
                 static_cast<unsigned>(source.format),
                 static_cast<unsigned>(g_interp_current.format));
  }

  constexpr wgpu::CommandEncoderDescriptor encoder_desc{.label = "GekkoAOT v134 interpolation snapshot"};
  auto encoder = g_device.CreateCommandEncoder(&encoder_desc);
  const std::array attachments{wgpu::RenderPassColorAttachment{
      .view = g_interp_current.view,
      .loadOp = wgpu::LoadOp::Clear,
      .storeOp = wgpu::StoreOp::Store,
      .clearValue = {0.0, 0.0, 0.0, 1.0},
  }};
  const wgpu::RenderPassDescriptor pass_desc{
      .label = "GekkoAOT v134 interpolation snapshot blit",
      .colorAttachmentCount = attachments.size(),
      .colorAttachments = attachments.data(),
  };
  const auto pass = encoder.BeginRenderPass(&pass_desc);
  pass.SetPipeline(g_CopyPipeline);
  pass.SetBindGroup(0, bind_group, 0, nullptr);
  pass.SetViewport(0.0f, 0.0f, static_cast<float>(source.size.width),
                   static_cast<float>(source.size.height), 0.0f, 1.0f);
  pass.Draw(3);
  pass.End();

  constexpr wgpu::CommandBufferDescriptor command_desc{.label = "GekkoAOT v134 interpolation snapshot buffer"};
  const auto command = encoder.Finish(&command_desc);
  g_queue.Submit(1, &command);

  g_interp_current_valid = true;
  ++g_interp_snapshot_count;
  return g_interp_previous_valid;
}

void ObservePerspectiveCamera(unsigned vertices) {
  using namespace aurora::gx;
  if (g_gxState.projType != GX_PERSPECTIVE || aurora::gfx::is_offscreen()) return;
  const auto& proj = g_gxState.proj;
  const auto& view = g_gxState.logicalViewport;
  if (!(view.width > 0.f) || !(view.height > 0.f)) return;
  if(g_texture_viewport_roles.IsProducer(view.left,view.top,view.width,view.height,
       g_camera_copy_epoch.load(std::memory_order_relaxed)) ||
     g_screen_fill_draw || g_draw_perspective_ui ||
     !g_gxState.depthCompare || !g_gxState.depthUpdate) return;
  const std::array<float,10> candidate{proj.m0[0],proj.m0[2],proj.m1[1],proj.m1[2],
    proj.m2[2],proj.m2[3],view.left,view.top,view.width,view.height};
  const float display_width=DisplayViewWidth(view.width);
  const std::array<float,4> derived_key{proj.m0[0],proj.m1[1],display_width,view.height};
  if(!g_camera_memo.derived || g_camera_memo.key!=derived_key){
    g_camera_memo.key=derived_key;g_camera_memo.derived=true;
    g_camera_memo.fov=114.591559f*std::atan(1.f/std::abs(proj.m1[1]));
    g_camera_memo.aspect=GekkoAOT::GX::ProjectionAspect(proj.m0[0],proj.m1[1],display_width,view.height);
  }
  const float candidate_fov=g_camera_memo.fov,candidate_aspect=g_camera_memo.aspect;
  if(!std::isfinite(candidate_fov)||candidate_fov<19.999f||candidate_fov>150.001f)return;
  const std::array<float,8> lens{proj.m0[0],proj.m0[2],proj.m1[1],proj.m1[2],proj.m2[2],proj.m2[3],candidate_fov,candidate_aspect};
  const auto epoch=g_camera_vi_epoch.load(std::memory_order_relaxed);
  if(!g_camera_memo.published || g_camera_memo.lens!=lens || g_camera_memo.epoch!=epoch){
    auto found=std::find_if(g_camera_candidates_producer.begin(),g_camera_candidates_producer.end(),[&](const auto& c){return c.valid&&c.lens==lens;});
    if(found==g_camera_candidates_producer.end())found=std::min_element(g_camera_candidates_producer.begin(),g_camera_candidates_producer.end(),[](const auto& a,const auto& b){if(a.valid!=b.valid)return !a.valid;return a.epoch<b.epoch;});
    if(!found->valid || found->lens!=lens || found->epoch!=epoch){
      const auto index=std::size_t(found-g_camera_candidates_producer.begin());
      *found={lens,epoch,true};std::lock_guard lock(g_camera_projection_mutex);g_camera_candidates[index]=*found;
    }
    g_camera_memo.lens=lens;g_camera_memo.epoch=epoch;g_camera_memo.published=true;
  }
  if(!g_scene_camera_selector.Observe(epoch,candidate,view.zfar-view.znear,vertices))return;
  const std::array coefficients{proj.m0[0], proj.m1[1], view.width, view.height};
  if (coefficients != g_camera_coefficients || g_camera_cached_signature == 0u) {
    const float vertical = std::fabs(proj.m1[1]);
    if (!std::isfinite(vertical) || vertical < 0.00001f) return;
    const float fov = 2.f * std::atan(1.f / vertical) * 57.29577951308232f;
    const float aspect = GekkoAOT::GX::ProjectionAspect(
        proj.m0[0], proj.m1[1], DisplayViewWidth(view.width), view.height);
    if (!std::isfinite(fov) || fov < 20.f || fov > 150.f ||
        !std::isfinite(aspect) || aspect < .5f || aspect > 5.f) return;
    std::uint32_t fov_bits=0, aspect_bits=0;
    std::memcpy(&fov_bits,&fov,sizeof(fov));
    std::memcpy(&aspect_bits,&aspect,sizeof(aspect));
    g_camera_cached_signature = (std::uint64_t(fov_bits)<<32u)|aspect_bits;
    g_camera_coefficients = coefficients;
  }
  if (g_camera_published_signature != g_camera_cached_signature ||
      g_camera_published_copy != g_display_copy_count) {
    g_camera_signature.store(g_camera_cached_signature,std::memory_order_relaxed);
    g_camera_observed_copy.store(g_display_copy_count,std::memory_order_release);
    g_camera_published_signature = g_camera_cached_signature;
    g_camera_published_copy = g_display_copy_count;
  }
  const std::array<float,6> raw{proj.m0[0],proj.m0[2],proj.m1[1],proj.m1[2],proj.m2[2],proj.m2[3]};
  if(raw!=g_camera_producer_projection) {
    std::lock_guard lock(g_camera_projection_mutex);
    g_camera_projection=raw;g_camera_producer_projection=raw;
  }
}

// v300 GEKKOAOT_RUNTIME_PROFILE: averaged Aurora work per display copy, read
// from the stats of the last frame the render worker submitted.
void ReportAuroraFrameStats() {
  {
    // Frames that really reached the swapchain (what MangoHud counts).
    static auto last=std::chrono::steady_clock::now();
    static std::uint64_t last_presents=aurora::gfx::presentation_count();
    static std::uint64_t last_guest=0, last_replays=0, last_unique=0;
    const auto now=std::chrono::steady_clock::now();
    const double seconds=std::chrono::duration<double>(now-last).count();
    if (seconds>=5.0) {
      const auto presents=aurora::gfx::presentation_count();
      {
        // Guest frames (display copies), replays (interpolated presents),
        // unique poses (real frames shown directly + replays with a new pose)
        // and swapchain presents, per second.
        const auto guest=g_display_copy_count;
        const auto replays=g_metric_replays.load(std::memory_order_relaxed);
        const auto unique=g_metric_unique_replays.load(std::memory_order_relaxed);
        const auto direct=(presents-last_presents)>(replays-last_replays) ? (presents-last_presents)-(replays-last_replays) : 0u;
        double delay_ms=0, stalls=0;
        {
          std::lock_guard lock{g_replay_mutex};
          delay_ms=g_replay_interpolate && !g_replay_direct.load(std::memory_order_relaxed) ? 1000.0*g_replay_clock.Delay() : 0.0;
          stalls=double(g_replay_clock.Stalls());
        }
        std::fprintf(stderr,"GEKKOAOT_FPS_METRICS_V368 guest_fps=%.1f interpolated_fps=%.1f unique_pose_fps=%.1f presented_fps=%.1f added_latency_ms=%.1f clock_stalls=%.0f\n",
                     double(guest-last_guest)/seconds,double(replays-last_replays)/seconds,
                     double(unique-last_unique+direct)/seconds,double(presents-last_presents)/seconds,delay_ms,stalls);
        last_guest=guest; last_replays=replays; last_unique=unique;
      }
      const auto indexCache=g_native_index_ranges.Statistics();
      std::fprintf(stderr,"GEKKOAOT_GX_INDEX_CACHE enabled=%u hits=%llu misses=%llu admitted=%llu rejected=%llu\n",unsigned(g_native_index_range_cache),
        (unsigned long long)indexCache.hits,(unsigned long long)indexCache.misses,(unsigned long long)indexCache.admitted,(unsigned long long)indexCache.rejected);
      const auto pacing=g_present_pacing;
      g_present_pacing={};
      const double mean=pacing.count ? pacing.sum/double(pacing.count) : 0.0;
      const double jitter=pacing.count ? std::sqrt(std::max(0.0,pacing.sum_sq/double(pacing.count)-mean*mean)) : 0.0;
      std::fprintf(stderr,"GEKKOAOT_DISPLAY_FPS_V359 display_fps=%.1f interval_ms=%.2f jitter_ms=%.2f worst_ms=%.1f replay_captures=%llu eligible=%llu requests=%llu early=%llu period=%llu busy=%llu queued=%llu deadline=%llu direct=%llu cost_us=%lld frame_cost_us=%lld\n",
                   double(presents-last_presents)/seconds,mean,jitter,pacing.worst,
                   static_cast<unsigned long long>(g_replay_captures.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(g_replay_eligible.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(g_replay_requests.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(g_replay_declined_early.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(g_replay_declined_period.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(g_replay_declined_busy.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(g_replay_queued.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(g_replay_declined_deadline.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(g_replay_direct_ticks.load(std::memory_order_relaxed)),
                   static_cast<long long>(g_replay_cost_ns.load(std::memory_order_relaxed)/1000),
                   static_cast<long long>(g_replay_frame_cost_ns.load(std::memory_order_relaxed)/1000));
      if(g_native_ir_profile){const auto& ir=g_native_ir_stats;const auto& plans=g_native_draw_lists.stats;
        std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_PROGRESS sdk_ops=%llu fifo_ops=%llu decoder_units=%llu compatibility_calls=%llu primitive_plan_hits=%llu primitive_plan_misses=%llu primitive_plan_declined=%llu native_draws=%llu\n",
          (unsigned long long)ir.sdk,(unsigned long long)ir.fifo,(unsigned long long)ir.decoder_units,
          (unsigned long long)ir.compatibility,(unsigned long long)plans.hits,(unsigned long long)plans.misses,
          (unsigned long long)plans.declined,(unsigned long long)g_native_draw_list_executed);
      }
      last=now; last_presents=presents;
    }
  }
  static const bool enabled = EnvBool("GEKKOAOT_RUNTIME_PROFILE", false);
  if (!enabled) return;
  static std::uint64_t frames = 0u, draws = 0u, merged = 0u, verts = 0u, uniforms = 0u,
                       indices = 0u, storage = 0u, uploads = 0u;
  const AuroraStats* stats = aurora_get_stats();
  if (!stats) return;
  ++frames;
  draws += stats->drawCallCount;
  merged += stats->mergedDrawCallCount;
  verts += stats->lastVertSize;
  uniforms += stats->lastUniformSize;
  indices += stats->lastIndexSize;
  storage += stats->lastStorageSize;
  uploads += stats->lastTextureUploadSize;
  if (frames < 120u) return;
  ReportDrawCensus();
  ReportGxPhases();
  std::fprintf(stderr, "GEKKOAOT_XFB_DEAD_COPIES_V320=1 copies=%llu unpresented_overwrites=%llu\n",
               static_cast<unsigned long long>(frames),
               static_cast<unsigned long long>(std::exchange(g_xfb_unpresented_overwrites, 0u)));
  if(g_native_vertex_program)
    std::fprintf(stderr,"GEKKOAOT_NATIVE_VERTEX_V315_STATS draws=%llu vertices=%llu\n",
      static_cast<unsigned long long>(g_native_vertex_draws),
      static_cast<unsigned long long>(g_native_vertex_vertices));
  if(g_typed_arrays)
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_TYPED_COMMANDS_V313_STATS arrays=%llu fallback=%llu declined=%llu\n",
      static_cast<unsigned long long>(g_native_commands.NativeCalls()-g_native_commands.MatrixCalls()),
      static_cast<unsigned long long>(g_native_commands.FallbackCalls()),
      static_cast<unsigned long long>(g_native_commands.DeclinedHints()));
  if(g_direct_matrices)
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_DIRECT_MATRICES_V317_STATS matrices=%llu fallback=%llu\n",
      static_cast<unsigned long long>(g_native_commands.MatrixCalls()),
      static_cast<unsigned long long>(g_native_commands.FallbackCalls()));
  std::fprintf(stderr,
               "GEKKOAOT_AURORA_FRAME_V300=1 copies=%llu draws=%llu merged=%llu vert_kb=%.1f "
               "uniform_kb=%.1f index_kb=%.1f storage_kb=%.1f texture_kb=%.1f (per copy)\n",
               static_cast<unsigned long long>(frames),
               static_cast<unsigned long long>(draws / frames),
               static_cast<unsigned long long>(merged / frames), verts / 1024.0 / frames,
               uniforms / 1024.0 / frames, indices / 1024.0 / frames,
               storage / 1024.0 / frames, uploads / 1024.0 / frames);
  frames = draws = merged = verts = uniforms = indices = storage = uploads = 0u;
}

bool LatchDisplayCopyCached(std::uint32_t trigger_word, bool clear) {
  PhaseScope phase_scope(kPhPresent);
  using namespace aurora;
  using namespace aurora::gx;
  using namespace aurora::webgpu;

  // phys=0 is a valid MEM1-backed GXCopyDisp destination. Keep the Aurora
  // snapshot: VI may later select BOTTOM=0x500, i.e. row 1 of this zero-based
  // XFB. Do not early-return here.
  if (g_copy_dest_valid && g_gxState.texCopyDest &&
      g_copy_dest_physical == 0u && g_frame_open) {
    static unsigned zero_base_logs = 0u;
    if (zero_base_logs++ < 16u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_XFB_ZERO_BASE_V202=1 action=cache-and-present-via-vi-offset\n");
  }

  if (!g_copy_src_valid || !g_copy_dest_valid || !g_gxState.texCopyDest ||
      !g_frame_open) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_XFB_CACHE_V12=0 src=%u dest=%u phys=%08x frame=%u action=fail-closed\n",
                 g_copy_src_valid ? 1u : 0u, g_copy_dest_valid ? 1u : 0u,
                 g_copy_dest_physical, g_frame_open ? 1u : 0u);
    g_fifo_fault = true;
    g_quit = true;
    return false;
  }

  const std::uint32_t copy_width =
      static_cast<std::uint32_t>(std::max(g_gxState.texCopySrc.width, 1));
  const std::uint32_t source_height =
      static_cast<std::uint32_t>(std::max(g_gxState.texCopySrc.height, 1));
  const bool scale_invert = ((trigger_word >> 10u) & 1u) != 0u;
  float y_scale = 1.0f;
  if (g_copy_yscale_raw != 0u) {
    y_scale = scale_invert ? (256.0f / static_cast<float>(g_copy_yscale_raw))
                           : (static_cast<float>(g_copy_yscale_raw) / 256.0f);
  }
  const float xfb_lines_f =
      1.0f + static_cast<float>(source_height - 1u) * y_scale;
  const std::uint32_t copy_height =
      std::max<std::uint32_t>(static_cast<std::uint32_t>(std::max(xfb_lines_f, 1.0f)), 1u);

  // Reuse Aurora's normal EFB-copy recorder.  Unlike V10's render-target
  // rotation, resolve_pass_into() gives the XFB its own persistent texture and
  // starts a fresh EFB pass with the requested clear semantics.  Nothing is
  // presented here: VI remains the sole display owner.
  g_gxState.texCopyDstWidth = copy_width;
  g_gxState.texCopyDstHeight = copy_height;
  g_gxState.texCopyFmt =
      (g_gxState.pixelFmt == GX_PF_RGB8_Z24 || g_gxState.pixelFmt == GX_PF_RGB565_Z16)
          ? GX_TF_RGB565
          : GX_TF_RGBA8;
  // CPU EFB peeks observe the finished frame, not the fresh pass the
  // display copy starts with the guest's clear.
  aurora::gfx::gekkoaot_capture_peek_snapshot();
  copy_tex(g_gxState.texCopyDest, clear ? GX_TRUE : GX_FALSE);

  const auto copied = g_gxState.copyTextures.find(g_gxState.texCopyDest);
  if (copied == g_gxState.copyTextures.end() || !copied->second.handle) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_XFB_CACHE_V12=0 xfb=%08x reason=aurora-copy-cache-miss action=fail-closed\n",
                 g_copy_dest_physical);
    g_fifo_fault = true;
    g_quit = true;
    return false;
  }

  auto* slot = AcquireXfbSlot(g_copy_dest_physical);
  slot->snapshot = copied->second.handle;
  slot->frame_buffer = TextureWithSampler{
      .texture = slot->snapshot->texture,
      .view = slot->snapshot->sampleTextureView,
      .size = slot->snapshot->size,
      .format = slot->snapshot->format,
      .sampler = g_frameBuffer.sampler,
  };
  slot->resolved = {};
  if (slot->valid && slot->presented_generation != slot->generation) ++g_xfb_unpresented_overwrites;
  {
    static std::uint64_t g_trace_last_draws = 0;
    // Diagnostic: GEKKOAOT_XFB_COPY_TRACE=<skip>:<count> logs display copies
    // (address, height, whether the slot's previous frame was presented).
    static const auto trace = [] {
      const char* v = std::getenv("GEKKOAOT_XFB_COPY_TRACE");
      std::pair<std::uint64_t, std::uint64_t> r{0u, 0u};
      if (v) std::sscanf(v, "%llu:%llu", reinterpret_cast<unsigned long long*>(&r.first),
                         reinterpret_cast<unsigned long long*>(&r.second));
      return r;
    }();
    static std::uint64_t seen = 0u;
    if (trace.second && seen++ >= trace.first && seen <= trace.first + trace.second)
      std::fprintf(stderr, "GEKKOAOT_XFB_COPY_TRACE_V389 copy=%llu draws=%llu t_us=%lld xfb=%08x h=%u slot_valid=%u prev_presented=%u\n",
                   static_cast<unsigned long long>(g_display_copy_count),
                   static_cast<unsigned long long>(g_trace_native_draws - std::exchange(g_trace_last_draws, g_trace_native_draws)),
                   static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()).count()),
                   g_copy_dest_physical, copy_height, slot->valid ? 1u : 0u,
                   slot->presented_generation == slot->generation ? 1u : 0u);
  }
  slot->address = g_copy_dest_physical;
  slot->stride_bytes = g_copy_stride_bytes != 0u ? g_copy_stride_bytes : copy_width * 2u;
  slot->native_width = copy_width;
  g_display_copy_width.store(copy_width, std::memory_order_relaxed);
  slot->native_height = copy_height;
  slot->span_bytes = static_cast<std::uint64_t>(slot->stride_bytes) * copy_height;
  slot->generation = ++g_xfb_generation;
  slot->capture_frame = GekkoAOT::Capture::frame_id;
  slot->valid = true;
  slot->force_43 = g_recorded_force_43;

  // A GXCopyDisp(clear=1) can clear only selected PE attachments/channels.
  // Preserve only what the guest explicitly left untouched.  In particular:
  //   - MOH Frontline depth-only clear: LOAD color, CLEAR depth.
  //   - T3 masked clear: LOAD whichever attachment must survive the boundary.
  //   - full clear: no LOAD is needed.
  //
  // This keeps GXCopyDisp as a hard host-frame boundary, preventing the v176
  // runaway-frame/index-cap failure seen on HPCOS.
  const bool efb_has_alpha = g_gxState.pixelFmt == GX_PF_RGBA6_Z24;
  const bool guest_clears_color = clear && g_gxState.colorUpdate;
  const bool guest_clears_alpha = clear && g_gxState.alphaUpdate && efb_has_alpha;
  const bool guest_clears_depth = clear && g_gxState.depthUpdate;

  const bool preserve_color =
      clear && (!guest_clears_color || (efb_has_alpha && !guest_clears_alpha));
  const bool preserve_depth = clear && !guest_clears_depth;
  const bool masked_clear = clear && (preserve_color || preserve_depth);

  // Keep the old v176 behavior only as an explicit A/B switch. It is no longer
  // safe as the default because it can let a logical Aurora frame grow without
  // bound when a title emits many partial GXCopyDisp clears.
  const bool keep_recording_open =
      g_partial_clear_copy_continuation && masked_clear;

  if (g_display_copy_state_logs++ < 128u) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_XFB_COPY_STATE_SELECTIVE_V1 copy=%llu xfb=%08x src=%d,%d %dx%d lines=%u stride=%u clear=%u color_update=%u alpha_update=%u depth_update=%u masked=%u preserve_color=%u preserve_depth=%u keep_open=%u global_load=%u\n",
                 static_cast<unsigned long long>(g_display_copy_count),
                 g_copy_dest_physical, g_gxState.texCopySrc.x, g_gxState.texCopySrc.y,
                 g_gxState.texCopySrc.width, g_gxState.texCopySrc.height, copy_height,
                 g_copy_stride_bytes, clear ? 1u : 0u,
                 g_gxState.colorUpdate ? 1u : 0u,
                 g_gxState.alphaUpdate ? 1u : 0u,
                 g_gxState.depthUpdate ? 1u : 0u,
                 masked_clear ? 1u : 0u,
                 preserve_color ? 1u : 0u,
                 preserve_depth ? 1u : 0u,
                 keep_recording_open ? 1u : 0u,
                 g_efb_boundary_continuity ? 1u : 0u);
  }

  if (keep_recording_open) {
    if (g_partial_clear_copy_logs++ < 64u) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_XFB_PARTIAL_CLEAR_V176_LEGACY event=masked-clear copy=%llu xfb=%08x action=keep-aurora-frame-open\n",
                   static_cast<unsigned long long>(g_display_copy_count),
                   g_copy_dest_physical);
    }
    return true;
  }

  // A copy without clear keeps both EFB attachments alive, even across our
  // bounded host-frame boundary. Retail splash/loading renderers draw once
  // and issue more display copies or overlay draws afterwards.
  if (g_masked_clear_efb_continuity && (masked_clear || !clear)) {
    aurora::gfx::gekkoaot_preserve_efb_on_next_begin(!clear || preserve_color, !clear || preserve_depth);
  } else if (g_efb_boundary_continuity) {
    // Diagnostic compatibility mode matching the old v177 all-attachment LOAD.
    aurora::gfx::gekkoaot_preserve_efb_on_next_begin(true, true);
  }

  // v110 + selective EFB continuity: every GXCopyDisp remains a bounded
  // producer boundary. The next recording only LOADs attachments which must
  // survive a masked guest clear.
  // VI/host scanout remains read-only.
  if (g_vsync_applied!=g_vsync_default) { // live VSync setting
    aurora_enable_vsync(g_vsync_default);
    g_vsync_applied=g_vsync_default;
  }
  std::shared_ptr<ReplayCapture> finished;
  // Capturing a frame costs GPU time (EFB start copies) and GX time even if no
  // replay ever runs: frames presented directly are not captured at all.
  const bool unlock=g_frame_interpolation_enabled && g_replay_mode;
  const bool direct_frame=unlock && g_replay_direct.load(std::memory_order_relaxed);
  if (const bool replay=unlock && !direct_frame;
      replay!=g_replay_capture_on.load(std::memory_order_relaxed)) {
    // Switch at a frame boundary; the first captured frame starts next.
    g_replay_capture_on.store(replay,std::memory_order_relaxed);
    aurora::gfx::detail::gekkoaot_replay_set_enabled(replay);
    g_replay_building={};
    g_replay_occurrences.clear();
    g_replay_array_last.clear();
    g_replay_array_seen.clear();
    std::lock_guard lock{g_replay_mutex};
    g_replay_current.reset();
    g_replay_previous.reset();
    g_replay_clock.Reset();
  } else if (replay) {
    finished=FinishReplayCapture(*slot);
  }
  aurora_end_frame_no_present();
  if (finished)
    aurora::gfx::render_worker::enqueue_work([finished=std::move(finished)]() mutable {
      PublishReplayCapture(std::move(finished));
    });
  else if (direct_frame)
    aurora::gfx::render_worker::enqueue_work([] {
      NoteGuestFrame();
      TimeSubmittedWork(ReplayClockNs(),false);
    });
  GekkoAOT::GX::Hud::NoteGameFrame();
  ReportAuroraFrameStats();
  CaptureFrameBoundary();
  g_frame_open = false;
  if (g_array_frame_refresh) RefreshIndexedArraySnapshots();

  // v110 moved the producer boundary to GXCopyDisp, but the v48
  // interpolation history was never reconnected to that new boundary.  The
  // snapshot helper consequently became dead code and FrameInterpolationReady()
  // could never become true.  Capture the immutable resolved XFB after the
  // producer frame has been submitted; render-worker queue ordering guarantees
  // that the history copy observes the completed GXCopyDisp result.
  const bool interpolation_ready =
      g_frame_interpolation_enabled && !g_replay_mode && SnapshotInterpolationFrame(slot->frame_buffer);
  if (g_frame_interpolation_enabled) {
    static unsigned interpolation_logs = 0u;
    if (interpolation_logs++ < 64u)
      std::fprintf(stderr,
                   "GEKKOAOT_FRAME_INTERPOLATION_V127I=1 phase=snapshot xfb=%08x snapshot=%llu previous=%u current=%u ready=%u source=gx-copy\n",
                   g_copy_dest_physical, static_cast<unsigned long long>(g_interp_snapshot_count),
                   g_interp_previous_valid ? 1u : 0u, g_interp_current_valid ? 1u : 0u,
                   interpolation_ready ? 1u : 0u);
  }
  if (!g_quit && aurora_begin_frame()) g_frame_open = true;

  GekkoAOT::Capture::Mark(GekkoAOT::Capture::Kind::XfbPublish, slot->capture_frame, g_copy_dest_physical);
  g_frame_ready_xfb.store(g_copy_dest_physical, std::memory_order_release);

  const unsigned frame_to_field = static_cast<unsigned>((trigger_word >> 12u) & 3u);
  static unsigned logs = 0;
  if (logs++ < 64u) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_XFB_CACHE_V12=1 copy=%llu xfb=%08x clear=%u src=%d,%d %dx%d native=%ux%u stride=%u span=%llu yscale_raw=%u yscale=%.5f frame_to_field=%u generation=%llu mode=aurora-copy-cache-copy-boundary-submitted\n",
                 static_cast<unsigned long long>(g_display_copy_count),
                 g_copy_dest_physical, clear ? 1u : 0u,
                 g_gxState.texCopySrc.x, g_gxState.texCopySrc.y,
                 g_gxState.texCopySrc.width, g_gxState.texCopySrc.height,
                 copy_width, copy_height, slot->stride_bytes,
                 static_cast<unsigned long long>(slot->span_bytes),
                 g_copy_yscale_raw, static_cast<double>(y_scale), frame_to_field,
                 static_cast<unsigned long long>(slot->generation));
  }
  return true;
}
