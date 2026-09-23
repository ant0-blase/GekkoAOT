cmake_minimum_required(VERSION 3.25)
project(GekkoAOTAuroraBridge LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(GEKKOAOT_AURORA_SOURCE "" CACHE PATH "Pinned Aurora source checkout")
if(NOT EXISTS "${GEKKOAOT_AURORA_SOURCE}/CMakeLists.txt")
  message(FATAL_ERROR "GEKKOAOT_AURORA_SOURCE must point to encounter/aurora")
endif()

# GekkoAOT v42-compatible single-window adaptation.
#
# SDL/Vulkan must wrap the Qt GameView XID when the window is CREATED. Reparenting
# an already-created Vulkan top-level is too late and is exactly what caused the
# bridge to spawn a second game window. Keep this adaptation tiny,
# pinned to Aurora's known create_window() source, and idempotent.
set(_gekkoaot_aurora_window "${GEKKOAOT_AURORA_SOURCE}/lib/window.cpp")
if(UNIX AND NOT APPLE AND EXISTS "${_gekkoaot_aurora_window}")
  file(READ "${_gekkoaot_aurora_window}" _gekkoaot_window_text)
  if(NOT _gekkoaot_window_text MATCHES "GEKKOAOT_EXTERNAL_X11_V001")
    string(REPLACE
      "#include <atomic>"
      "#include <atomic>\n#include <cstdlib>"
      _gekkoaot_window_text "${_gekkoaot_window_text}")
    set(_gekkoaot_create_anchor "  g_window = SDL_CreateWindowWithProperties(props);")
    set(_gekkoaot_create_inject [=[  // GEKKOAOT_EXTERNAL_X11_V001
#if defined(SDL_PLATFORM_LINUX)
  if (const char* xidText = std::getenv("GEKKOAOT_NATIVE_GX_X11_WINDOW");
      xidText != nullptr && xidText[0] != '\0') {
    char* end = nullptr;
    const unsigned long long xid = std::strtoull(xidText, &end, 10);
    if (end != xidText && *end == '\0' && xid != 0) {
      TRY(SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X11_WINDOW_NUMBER,
                                static_cast<Sint64>(xid)),
          "Failed to wrap GekkoAOT GameView X11 window: {}", SDL_GetError());
    }
  }
#endif
  g_window = SDL_CreateWindowWithProperties(props);]=])
    string(FIND "${_gekkoaot_window_text}" "${_gekkoaot_create_anchor}" _gekkoaot_create_pos)
    if(_gekkoaot_create_pos EQUAL -1)
      message(FATAL_ERROR "Pinned Aurora window.cpp changed: SDL window creation anchor missing")
    endif()
    string(REPLACE "${_gekkoaot_create_anchor}" "${_gekkoaot_create_inject}"
      _gekkoaot_window_text "${_gekkoaot_window_text}")
    file(WRITE "${_gekkoaot_aurora_window}" "${_gekkoaot_window_text}")
    message(STATUS "GekkoAOT: Aurora single-window X11 adapter installed")
  endif()
endif()

# GekkoAOT native XFB presentation override.
#
# Aurora 7f2801cd always presents its live EFB. Retail GX display copies latch
# an earlier EFB image into XFB and VI scans that image later, so GekkoAOT needs
# a presentation-source override without forcing an early swapchain present.
# Keep this adaptation pinned to the known present_source() body and fail the
# configure if the pinned Aurora source changes underneath us.
set(_gekkoaot_aurora_gpu "${GEKKOAOT_AURORA_SOURCE}/lib/webgpu/gpu.cpp")
if(EXISTS "${_gekkoaot_aurora_gpu}")
  file(READ "${_gekkoaot_aurora_gpu}" _gekkoaot_gpu_text)
  if(NOT _gekkoaot_gpu_text MATCHES "GEKKOAOT_PRESENT_OVERRIDE_V9")
    set(_gekkoaot_gpu_decl_anchor "static TextureWithSampler g_resampledFrameBuffer;")
    set(_gekkoaot_gpu_present_anchor "const TextureWithSampler& present_source() noexcept {\n  return g_graphicsConfig.msaaSamples > 1 ? g_frameBufferResolved : g_frameBuffer;\n}")
    string(FIND "${_gekkoaot_gpu_text}" "${_gekkoaot_gpu_decl_anchor}" _gekkoaot_gpu_decl_pos)
    string(FIND "${_gekkoaot_gpu_text}" "${_gekkoaot_gpu_present_anchor}" _gekkoaot_gpu_present_pos)
    if(_gekkoaot_gpu_decl_pos EQUAL -1 OR _gekkoaot_gpu_present_pos EQUAL -1)
      message(FATAL_ERROR "Pinned Aurora gpu.cpp changed: XFB present-source anchors missing")
    endif()
    string(REPLACE
      "${_gekkoaot_gpu_decl_anchor}"
      "static TextureWithSampler g_resampledFrameBuffer;\n// GEKKOAOT_PRESENT_OVERRIDE_V9\nstatic const TextureWithSampler* g_gekkoaotPresentOverride = nullptr;"
      _gekkoaot_gpu_text "${_gekkoaot_gpu_text}")
    string(REPLACE
      "${_gekkoaot_gpu_present_anchor}"
      "void set_present_source_override(const TextureWithSampler* source) noexcept {\n  g_gekkoaotPresentOverride = source;\n}\n\nconst TextureWithSampler& present_source() noexcept {\n  if (g_gekkoaotPresentOverride != nullptr) {\n    return *g_gekkoaotPresentOverride;\n  }\n  return g_graphicsConfig.msaaSamples > 1 ? g_frameBufferResolved : g_frameBuffer;\n}"
      _gekkoaot_gpu_text "${_gekkoaot_gpu_text}")
    file(WRITE "${_gekkoaot_aurora_gpu}" "${_gekkoaot_gpu_text}")
    message(STATUS "gekkoaot: Aurora native XFB present-source override installed")
  endif()
endif()

# GekkoAOT V10 zero-copy display latch support.
#
# Aurora's normal resolve_pass_into() performs a fullscreen resolve/copy. For a
# retail display copy that fully clears the EFB afterwards, that copy is
# unnecessary: rotate the single-sample EFB backing texture into the XFB latch
# and continue rendering into the old latch texture. Partial/no-clear copies
# keep the V9 resolve path for exact semantics.
set(_gekkoaot_aurora_recording_hpp "${GEKKOAOT_AURORA_SOURCE}/lib/gfx/recording.hpp")
set(_gekkoaot_aurora_recording_cpp "${GEKKOAOT_AURORA_SOURCE}/lib/gfx/recording.cpp")
if(EXISTS "${_gekkoaot_aurora_recording_hpp}" AND EXISTS "${_gekkoaot_aurora_recording_cpp}")
  file(READ "${_gekkoaot_aurora_recording_hpp}" _gekkoaot_recording_hpp_text)
  file(READ "${_gekkoaot_aurora_recording_cpp}" _gekkoaot_recording_cpp_text)
  if(NOT _gekkoaot_recording_cpp_text MATCHES "GEKKOAOT_XFB_ZERO_COPY_V10")
    set(_gekkoaot_xfb_decl_anchor [=[void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat = GX_TF_RGBA8);]=])
    set(_gekkoaot_xfb_decl_replacement [=[void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat = GX_TF_RGBA8);
bool resolve_display_copy_zero_copy(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha,
                                    bool clearDepth, Vec4<float> clearColorValue, float clearDepthValue);]=])
    string(FIND "${_gekkoaot_recording_hpp_text}" "${_gekkoaot_xfb_decl_anchor}" _gekkoaot_xfb_decl_pos)
    if(_gekkoaot_xfb_decl_pos EQUAL -1)
      message(FATAL_ERROR "Pinned Aurora recording.hpp changed: XFB zero-copy declaration anchor missing")
    endif()
    string(REPLACE "${_gekkoaot_xfb_decl_anchor}" "${_gekkoaot_xfb_decl_replacement}"
      _gekkoaot_recording_hpp_text "${_gekkoaot_recording_hpp_text}")

    set(_gekkoaot_xfb_impl_anchor [=[void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat) {
  // Resolve current render pass]=])
    set(_gekkoaot_xfb_impl_replacement [=[// GEKKOAOT_XFB_ZERO_COPY_V10
bool resolve_display_copy_zero_copy(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha,
                                    bool clearDepth, Vec4<float> clearColorValue, float clearDepthValue) {
  // Full-target + full-color-clear display copies can be latched without moving
  // a single pixel: the old EFB becomes the XFB and the previous XFB texture
  // becomes the next live EFB. The sealed pass keeps its old WebGPU handles.
  // Partial/no-clear copies still need the old EFB contents and fall back to
  // resolve_pass_into(), preserving V9 correctness.
  if (!texture || !g_recorder.active() || g_recorder.currentRenderPass == UINT32_MAX || g_recorder.inOffscreen) {
    return false;
  }

  auto& frame = current_frame_packet();
  auto& prevPass = current_render_passes()[g_recorder.currentRenderPass];
  if (prevPass.colorAttachmentCount == 0) {
    return false;
  }
  const auto& scene = prevPass.colorAttachments[SceneColorAttachmentIndex];
  const bool fullRect = rect.x == 0 && rect.y == 0 && rect.width == static_cast<int32_t>(scene.size.width) &&
                        rect.height == static_cast<int32_t>(scene.size.height);
  const bool fullColorClear = clearColor && clearAlpha;
  auto& liveColor = webgpu::g_graphicsConfig.msaaSamples > 1 ? webgpu::g_frameBufferResolved
                                                              : webgpu::g_frameBuffer;
  const bool compatible = liveColor.texture && liveColor.view && texture->texture && texture->attachmentTextureView &&
                          texture->size.width == liveColor.size.width &&
                          texture->size.height == liveColor.size.height && texture->format == liveColor.format;
  if (!fullRect || !fullColorClear || !compatible) {
    return false;
  }

  // Preserve ordering, but do not attach a resolve target: this is the part
  // that removes the repeated native-resolution fullscreen resolve.
  prevPass.discardable = false;
  enqueue_pass(frame, g_recorder.currentRenderPass);

  wgpu::Texture spareTexture = std::move(texture->texture);
  wgpu::TextureView spareView = std::move(texture->attachmentTextureView);
  texture->sampleTextureView = {};

  texture->texture = std::move(liveColor.texture);
  wgpu::TextureView latchedView = std::move(liveColor.view);
  texture->sampleTextureView = latchedView;
  texture->attachmentTextureView = std::move(latchedView);
  liveColor.texture = std::move(spareTexture);
  liveColor.view = std::move(spareView);

  RenderPass newPass{
      .label = pass_label("EFB"),
      .clearDepthValue = clearDepthValue,
      .clearDepth = clearDepth,
      .hasDepth = prevPass.hasDepth,
      .hasStencil = prevPass.hasStencil,
  };
  set_efb_targets(newPass);
  for (uint32_t i = 0; i < newPass.colorAttachmentCount; ++i) {
    newPass.colorAttachments[i].loadOp = wgpu::LoadOp::Undefined;
    newPass.colorAttachments[i].clear = false;
  }
  auto& newScene = newPass.colorAttachments[SceneColorAttachmentIndex];
  newScene.clear = true;
  newScene.clearValue = clearColorValue;
  newPass.commands.reserve(2048);
  current_render_passes().emplace_back(std::move(newPass));
  ++g_recorder.currentRenderPass;
  push_command(CommandType::SetViewport, Command::Data{.setViewport = g_recorder.cachedViewport});
  push_command(CommandType::SetScissor, Command::Data{.setScissor = g_recorder.cachedScissor});
  return true;
}

void resolve_pass_into(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                       Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat) {
  // Resolve current render pass]=])
    string(FIND "${_gekkoaot_recording_cpp_text}" "${_gekkoaot_xfb_impl_anchor}" _gekkoaot_xfb_impl_pos)
    if(_gekkoaot_xfb_impl_pos EQUAL -1)
      message(FATAL_ERROR "Pinned Aurora recording.cpp changed: XFB zero-copy implementation anchor missing")
    endif()
    string(REPLACE "${_gekkoaot_xfb_impl_anchor}" "${_gekkoaot_xfb_impl_replacement}"
      _gekkoaot_recording_cpp_text "${_gekkoaot_recording_cpp_text}")
    file(WRITE "${_gekkoaot_aurora_recording_hpp}" "${_gekkoaot_recording_hpp_text}")
    file(WRITE "${_gekkoaot_aurora_recording_cpp}" "${_gekkoaot_recording_cpp_text}")
    message(STATUS "gekkoaot: Aurora XFB zero-copy V10 render-target rotation installed")
  endif()
endif()



# Native address-keyed zero-copy XFB support for Aurora 7f2801cd.
#
# GXCopyDisp(clear=1) transfers ownership of the current EFB render target into
# an address-keyed XFB slot.  The frame is submitted without WSI presentation,
# then rendering continues on a rotated target.  At VI retrace, scanout presents
# the retained texture selected by VI without ending/clearing the EFB currently
# being rendered.  There is no EFB->XFB texture copy and no CPU readback.
set(_gekkoaot_aurora_main "${GEKKOAOT_AURORA_SOURCE}/lib/aurora.cpp")
if(EXISTS "${_gekkoaot_aurora_main}")
  file(READ "${_gekkoaot_aurora_main}" _gekkoaot_aurora_main_text)
  if(NOT _gekkoaot_aurora_main_text MATCHES "GEKKOAOT_XFB_ZERO_COPY_V10B")
    foreach(_anchor IN ITEMS
        "void end_frame() noexcept {"
        "const bool canPresent = currentTexture && currentView;"
        "void aurora_end_frame() { aurora::end_frame(); }"
        "} // namespace\n} // namespace aurora\n\n// C API bindings")
      string(FIND "${_gekkoaot_aurora_main_text}" "${_anchor}" _anchor_pos)
      if(_anchor_pos EQUAL -1)
        message(FATAL_ERROR "Pinned Aurora aurora.cpp changed: zero-copy XFB anchor missing: ${_anchor}")
      endif()
    endforeach()

    # end_frame(false) submits GX/GFX work but never acquires or presents a WSI
    # image.  This is the producer side of the zero-copy EFB->XFB hand-off.
    string(REPLACE
      "void end_frame() noexcept {"
      "void end_frame(bool present) noexcept { // GEKKOAOT_XFB_ZERO_COPY_V10B"
      _gekkoaot_aurora_main_text "${_gekkoaot_aurora_main_text}")
    string(REPLACE
      "gfx::end_frame([rmlBindGroup = std::move(rmlBindGroup), rmlOverlay, viewport,"
      "gfx::end_frame([present, rmlBindGroup = std::move(rmlBindGroup), rmlOverlay, viewport,"
      _gekkoaot_aurora_main_text "${_gekkoaot_aurora_main_text}")
    string(REPLACE
      "if (window::is_presentable() && g_surface) {"
      "if (present && window::is_presentable() && g_surface) {"
      _gekkoaot_aurora_main_text "${_gekkoaot_aurora_main_text}")
    string(REPLACE
      "const bool canPresent = currentTexture && currentView;"
      "const bool canPresent = present && currentTexture && currentView;"
      _gekkoaot_aurora_main_text "${_gekkoaot_aurora_main_text}")
    string(REPLACE
      "} else {\n      Log.info(\"Skipping present; window not presentable\");"
      "} else if (present) {\n      Log.info(\"Skipping present; window not presentable\");"
      _gekkoaot_aurora_main_text "${_gekkoaot_aurora_main_text}")
    string(REPLACE
      "} else if (g_surface) {\n      switch (surfaceStatus) {"
      "} else if (present && g_surface) {\n      switch (surfaceStatus) {"
      _gekkoaot_aurora_main_text "${_gekkoaot_aurora_main_text}")

    # VI scanout must not call end_frame() on the EFB currently under
    # construction.  Add a small presentation-only path that samples an
    # immutable retained XFB texture into the host swapchain.  Queue ordering
    # guarantees producer submissions complete before this scanout submission;
    # the current active EFB recorder remains untouched.
    set(_gekkoaot_present_helper [=[
void present_texture(const webgpu::TextureWithSampler& source) noexcept {
  ZoneScopedN("GekkoAOT VI XFB scanout");
#ifdef AURORA_ENABLE_GX
  if (!source.texture || !source.view || source.size.width == 0 || source.size.height == 0) {
    return;
  }

  // VI scanout must execute on Aurora's render worker, just like the normal
  // end_frame() present path. Queue this call behind the producer frame when
  // invoked from GekkoAOT's runtime thread; WebGPU handle copies retain the XFB.
  if (!gfx::render_worker::is_worker_thread()) {
    auto retained = source;
    gfx::render_worker::enqueue_work([retained = std::move(retained)]() mutable {
      present_texture(retained);
    });
    gfx::render_worker::synchronize();
    return;
  }

  if (!window::is_presentable()) {
    return;
  }
  if (!g_surface && !webgpu::refresh_surface(true)) {
    return;
  }
  if (!g_surface) {
    return;
  }

  wgpu::SurfaceTexture surfaceTexture;
  {
    window::SurfaceLock surfaceLock;
    if (!window::is_presentable() || !g_surface) {
      return;
    }
    g_surface.GetCurrentTexture(&surfaceTexture);
  }
  if (surfaceTexture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal || !surfaceTexture.texture) {
    webgpu::release_surface();
    return;
  }

  auto currentTexture = std::move(surfaceTexture.texture);
  auto currentView = currentTexture.CreateView();
  if (!currentView) {
    return;
  }

  constexpr wgpu::CommandEncoderDescriptor encoderDescriptor{.label = "GekkoAOT VI scanout encoder"};
  auto encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
  const auto viewport = webgpu::calculate_present_viewport(
      webgpu::g_graphicsConfig.surfaceConfiguration.width,
      webgpu::g_graphicsConfig.surfaceConfiguration.height,
      source.size.width, source.size.height);
  const auto bindGroup = webgpu::create_copy_bind_group(source);
  const std::array attachments{
      wgpu::RenderPassColorAttachment{
          .view = currentView,
          .loadOp = wgpu::LoadOp::Clear,
          .storeOp = wgpu::StoreOp::Store,
      },
  };
  const wgpu::RenderPassDescriptor renderPassDescriptor{
      .label = "GekkoAOT VI XFB scanout",
      .colorAttachmentCount = attachments.size(),
      .colorAttachments = attachments.data(),
  };
  const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
  pass.SetPipeline(webgpu::g_CopyPipeline);
  pass.SetBindGroup(0, bindGroup, 0, nullptr);
  set_present_viewport(pass, viewport,
                       webgpu::g_graphicsConfig.surfaceConfiguration.width,
                       webgpu::g_graphicsConfig.surfaceConfiguration.height);
  pass.Draw(3);
  pass.End();

  constexpr wgpu::CommandBufferDescriptor cmdBufDescriptor{.label = "GekkoAOT VI scanout command buffer"};
  const auto buffer = encoder.Finish(&cmdBufDescriptor);
  g_queue.Submit(1, &buffer);

  bool present_ok = false;
  {
    window::SurfaceLock surfaceLock;
    if (window::is_presentable() && g_surface) {
      const auto present_status = g_surface.Present();
      present_ok = static_cast<bool>(present_status);
    }
  }
  if (present_ok) {
    gfx::after_present();
  } else {
    webgpu::release_surface();
  }
#else
  (void)source;
#endif
}

// GekkoAOT v48 universal temporal interpolation. This is renderer-only: it
// blends two immutable completed GX snapshots and never advances guest time.
struct GekkoAotBlendResources {
  wgpu::BindGroupLayout layout;
  wgpu::RenderPipeline pipeline;
  wgpu::Buffer params;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
};

GekkoAotBlendResources& gekkoaot_blend_resources() {
  static GekkoAotBlendResources resources;
  return resources;
}

bool ensure_gekkoaot_blend_pipeline() {
#ifdef AURORA_ENABLE_GX
  auto& resources = gekkoaot_blend_resources();
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  if (resources.pipeline && resources.layout && resources.params && resources.format == format)
    return true;

  resources = {};
  resources.format = format;
  wgpu::ShaderSourceWGSL source{};
  source.code = R"WGSL(
struct Params { value: vec4<f32> };
struct VertexOutput { @builtin(position) position: vec4<f32>, @location(0) uv: vec2<f32> };
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var tex_sampler: sampler;
@group(0) @binding(2) var previous_tex: texture_2d<f32>;
@group(0) @binding(3) var current_tex: texture_2d<f32>;
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
  return vec4<f32>(mix(a.rgb, b.rgb, clamp(params.value.x, 0.0, 1.0)), 1.0);
}
)WGSL";
  const wgpu::ShaderModuleDescriptor module_desc{.nextInChain = &source,
                                                  .label = "GekkoAOT v48 interpolation shader"};
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
  };
  const wgpu::BindGroupLayoutDescriptor bgl_desc{.entryCount = entries.size(), .entries = entries.data()};
  resources.layout = g_device.CreateBindGroupLayout(&bgl_desc);
  const wgpu::PipelineLayoutDescriptor layout_desc{.bindGroupLayoutCount = 1,
                                                    .bindGroupLayouts = &resources.layout};
  const auto pipeline_layout = g_device.CreatePipelineLayout(&layout_desc);
  const std::array targets{wgpu::ColorTargetState{.format = format, .writeMask = wgpu::ColorWriteMask::All}};
  const wgpu::FragmentState fragment{.module = module, .entryPoint = "fs_main",
                                     .targetCount = targets.size(), .targets = targets.data()};
  const wgpu::RenderPipelineDescriptor pipeline_desc{
      .label = "GekkoAOT v48 interpolation pipeline",
      .layout = pipeline_layout,
      .vertex = wgpu::VertexState{.module = module, .entryPoint = "vs_main"},
      .primitive = wgpu::PrimitiveState{.topology = wgpu::PrimitiveTopology::TriangleList},
      .multisample = wgpu::MultisampleState{.count = 1, .mask = UINT32_MAX},
      .fragment = &fragment,
  };
  resources.pipeline = g_device.CreateRenderPipeline(&pipeline_desc);
  const wgpu::BufferDescriptor params_desc{.label = "GekkoAOT v48 interpolation params",
                                            .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::Uniform,
                                            .size = 16};
  resources.params = g_device.CreateBuffer(&params_desc);
  return resources.pipeline && resources.layout && resources.params;
#else
  return false;
#endif
}

void present_texture_blend(const webgpu::TextureWithSampler& previous,
                           const webgpu::TextureWithSampler& current,
                           float alpha) noexcept {
  ZoneScopedN("GekkoAOT v48 temporal interpolation");
#ifdef AURORA_ENABLE_GX
  if (!previous.texture || !previous.view || !current.texture || !current.view ||
      previous.size.width == 0 || previous.size.height == 0 || current.size.width == 0 ||
      current.size.height == 0)
    return;

  alpha = std::clamp(alpha, 0.0f, 1.0f);
  if (!gfx::render_worker::is_worker_thread()) {
    auto retained_previous = previous;
    auto retained_current = current;
    gfx::render_worker::enqueue_work(
        [retained_previous = std::move(retained_previous), retained_current = std::move(retained_current), alpha]() mutable {
          present_texture_blend(retained_previous, retained_current, alpha);
        });
    // Bounded queue: no stale interpolation backlog if the GPU misses target.
    gfx::render_worker::synchronize();
    return;
  }
  if (!ensure_gekkoaot_blend_pipeline() || !window::is_presentable()) return;
  if (!g_surface && !webgpu::refresh_surface(true)) return;
  if (!g_surface) return;

  wgpu::SurfaceTexture surface_texture;
  {
    window::SurfaceLock surface_lock;
    if (!window::is_presentable() || !g_surface) return;
    g_surface.GetCurrentTexture(&surface_texture);
  }
  if (surface_texture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal ||
      !surface_texture.texture) {
    webgpu::release_surface();
    return;
  }
  auto swap_texture = std::move(surface_texture.texture);
  auto swap_view = swap_texture.CreateView();
  if (!swap_view) return;

  auto& resources = gekkoaot_blend_resources();
  const std::array<float, 4> params{alpha, 0.0f, 0.0f, 0.0f};
  g_queue.WriteBuffer(resources.params, 0, params.data(), sizeof(params));
  const std::array bind_entries{
      wgpu::BindGroupEntry{.binding = 0, .buffer = resources.params, .size = 16},
      wgpu::BindGroupEntry{.binding = 1, .sampler = current.sampler},
      wgpu::BindGroupEntry{.binding = 2, .textureView = previous.view},
      wgpu::BindGroupEntry{.binding = 3, .textureView = current.view},
  };
  const wgpu::BindGroupDescriptor bind_desc{.layout = resources.layout,
                                             .entryCount = bind_entries.size(),
                                             .entries = bind_entries.data()};
  const auto bind_group = g_device.CreateBindGroup(&bind_desc);
  constexpr wgpu::CommandEncoderDescriptor encoder_desc{.label = "GekkoAOT v48 interpolation encoder"};
  auto encoder = g_device.CreateCommandEncoder(&encoder_desc);
  const auto viewport = webgpu::calculate_present_viewport(
      webgpu::g_graphicsConfig.surfaceConfiguration.width,
      webgpu::g_graphicsConfig.surfaceConfiguration.height,
      current.size.width, current.size.height);
  const std::array attachments{wgpu::RenderPassColorAttachment{
      .view = swap_view, .loadOp = wgpu::LoadOp::Clear, .storeOp = wgpu::StoreOp::Store}};
  const wgpu::RenderPassDescriptor pass_desc{.label = "GekkoAOT v48 interpolation pass",
                                              .colorAttachmentCount = attachments.size(),
                                              .colorAttachments = attachments.data()};
  const auto pass = encoder.BeginRenderPass(&pass_desc);
  pass.SetPipeline(resources.pipeline);
  pass.SetBindGroup(0, bind_group, 0, nullptr);
  set_present_viewport(pass, viewport, webgpu::g_graphicsConfig.surfaceConfiguration.width,
                       webgpu::g_graphicsConfig.surfaceConfiguration.height);
  pass.Draw(3);
  pass.End();
  constexpr wgpu::CommandBufferDescriptor command_desc{.label = "GekkoAOT v48 interpolation command buffer"};
  const auto command = encoder.Finish(&command_desc);
  g_queue.Submit(1, &command);

  bool present_ok = false;
  {
    window::SurfaceLock surface_lock;
    if (window::is_presentable() && g_surface) {
      const auto present_status = g_surface.Present();
      present_ok = static_cast<bool>(present_status);
    }
  }
  if (present_ok) gfx::after_present();
  else webgpu::release_surface();
#else
  (void)previous; (void)current; (void)alpha;
#endif
}
]=])
    string(REPLACE
      "} // namespace\n} // namespace aurora\n\n// C API bindings"
      "${_gekkoaot_present_helper}\n} // namespace\n} // namespace aurora\n\n// C API bindings"
      _gekkoaot_aurora_main_text "${_gekkoaot_aurora_main_text}")

    string(REPLACE
      "void aurora_end_frame() { aurora::end_frame(); }"
      "void aurora_end_frame() { aurora::end_frame(true); }\nextern \"C\" void aurora_end_frame_no_present() { aurora::end_frame(false); }\nextern \"C\" void aurora_present_texture(const void* texture) {\n#ifdef AURORA_ENABLE_GX\n  if (texture != nullptr) {\n    aurora::present_texture(*static_cast<const aurora::webgpu::TextureWithSampler*>(texture));\n  }\n#else\n  (void)texture;\n#endif\n}\nextern \"C\" void aurora_present_texture_blend(const void* previous, const void* current, float alpha) {\n#ifdef AURORA_ENABLE_GX\n  if (previous != nullptr && current != nullptr) {\n    aurora::present_texture_blend(*static_cast<const aurora::webgpu::TextureWithSampler*>(previous), *static_cast<const aurora::webgpu::TextureWithSampler*>(current), alpha);\n  }\n#else\n  (void)previous; (void)current; (void)alpha;\n#endif\n}"
      _gekkoaot_aurora_main_text "${_gekkoaot_aurora_main_text}")

    file(WRITE "${_gekkoaot_aurora_main}" "${_gekkoaot_aurora_main_text}")
    message(STATUS "gekkoaot: Aurora address-keyed zero-copy XFB hooks installed")
  endif()
endif()

# GekkoAOT owns DVD/nod and memory. Aurora owns GX/WebGPU + SDL presentation.
set(AURORA_ENABLE_GX ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_THP OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_RMLUI OFF CACHE BOOL "" FORCE)
set(AURORA_DAWN_PROVIDER package CACHE STRING "" FORCE)
set(AURORA_SDL3_PROVIDER auto CACHE STRING "" FORCE)
add_subdirectory("${GEKKOAOT_AURORA_SOURCE}" aurora EXCLUDE_FROM_ALL)

add_library(gekkoaot-native-gx SHARED bridge.cpp)
# aurora::gx and aurora::core reference the VI helper implemented in
# Aurora's dedicated aurora_vi target (configured_fb_size/configure).  Because
# gekkoaot-native-gx is a shared object, leaving aurora::vi out can produce a
# .so with an unresolved aurora::vi::configured_fb_size() that only fails at
# dlopen() time. Link it explicitly so both LLVM and Portable C modules reach
# the same self-contained native GX bridge.
target_link_libraries(gekkoaot-native-gx PRIVATE
  aurora::gx
  aurora::core
  aurora::vi
  absl::flat_hash_map)
target_include_directories(gekkoaot-native-gx PRIVATE
  "${GEKKOAOT_AURORA_SOURCE}/lib"
  "${GEKKOAOT_AURORA_SOURCE}/include")
set_target_properties(gekkoaot-native-gx PROPERTIES
  OUTPUT_NAME "gekkoaot-native-gx"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)

# The direct retail FIFO frontend is now a measurable hot path. Keep its build
# policy aligned with the native AOT runtime instead of relying only on generic
# CMake Release flags. IPO is optional and self-disables on unsupported hosts.
include(CheckIPOSupported)
check_ipo_supported(RESULT GEKKOAOT_GX_IPO_OK OUTPUT GEKKOAOT_GX_IPO_ERROR)
if(GEKKOAOT_GX_IPO_OK)
  set_property(TARGET gekkoaot-native-gx PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  message(STATUS "gekkoaot: native GX bridge IPO/LTO enabled")
endif()

find_package(X11 QUIET)
if(X11_FOUND)
  target_compile_definitions(gekkoaot-native-gx PRIVATE GEKKOAOT_HAVE_X11=1)
  target_link_libraries(gekkoaot-native-gx PRIVATE X11::X11)
endif()

if(MSVC)
  target_compile_options(gekkoaot-native-gx PRIVATE /W4 /O2)
else()
  target_compile_options(gekkoaot-native-gx PRIVATE
    -Wall -Wextra -Wpedantic -O3 -fomit-frame-pointer
    -fno-semantic-interposition -fno-plt)
  if(NOT APPLE)
    target_compile_options(gekkoaot-native-gx PRIVATE -march=native -mtune=native)
  endif()

  # Aurora's public Dolphin SDK headers intentionally contain declarations such
  # as `u8 u8;`, `u16 u16;`, etc. GCC 14+ diagnoses those declarations as
  # -Wchanges-meaning errors unless permissive mode is enabled. Keep this
  # compatibility flag local to the GekkoAOT/Aurora bridge instead of weakening
  # warning/error handling for Aurora or the rest of GekkoAOT.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(gekkoaot-native-gx PRIVATE -fpermissive)
  endif()
endif()
