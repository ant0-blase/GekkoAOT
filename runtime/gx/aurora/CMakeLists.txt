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
# Pinned Aurora revision presents its live EFB. Retail GX display copies latch
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
    # Aurora 9c0bf66f still carries the default GX_TF_RGBA8 argument in the
    # recording.hpp declaration.  Keep that declaration byte-for-byte compatible
    # instead of requiring the later 3840bf9 spelling without the default.
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



# Native address-keyed zero-copy XFB support for the pinned Aurora revision.
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
extern "C" float gekkoaot_native_gx_widescreen_target_aspect() noexcept;

webgpu::Viewport gekkoaot_present_viewport(uint32_t surface_width, uint32_t surface_height,
                                           uint32_t content_width, uint32_t content_height) noexcept {
  const float target = gekkoaot_native_gx_widescreen_target_aspect();
  if (!(target > 0.0f)) {
    return webgpu::calculate_present_viewport(surface_width, surface_height, content_width, content_height);
  }
  if (surface_width == 0 || surface_height == 0) return {};

  uint32_t viewport_width = surface_width;
  uint32_t viewport_height = static_cast<uint32_t>(static_cast<float>(viewport_width) / target + 0.5f);
  if (viewport_height > surface_height) {
    viewport_height = surface_height;
    viewport_width = static_cast<uint32_t>(static_cast<float>(viewport_height) * target + 0.5f);
  }
  if (viewport_width == 0) viewport_width = 1;
  if (viewport_height == 0) viewport_height = 1;
  if (viewport_width > surface_width) viewport_width = surface_width;
  if (viewport_height > surface_height) viewport_height = surface_height;
  return {
      .left = static_cast<float>((surface_width - viewport_width) / 2),
      .top = static_cast<float>((surface_height - viewport_height) / 2),
      .width = static_cast<float>(viewport_width),
      .height = static_cast<float>(viewport_height),
      .znear = 0.0f,
      .zfar = 1.0f,
  };
}

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
  const auto viewport = gekkoaot_present_viewport(
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
  const auto viewport = gekkoaot_present_viewport(
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


# Clip the source's one-pixel colored guard edge from 4:3 scanout. Apply after
# the XFB hook so fresh and already-patched Aurora checkouts get the same fix.
if(EXISTS "${_gekkoaot_aurora_main}")
  file(READ "${_gekkoaot_aurora_main}" _gekkoaot_aurora_main_text)
  if(NOT _gekkoaot_aurora_main_text MATCHES "GEKKOAOT_PRESENT_EDGE_CLIP_V175")
    set(_gekkoaot_edge_anchor [=[  set_present_viewport(pass, viewport,
                       webgpu::g_graphicsConfig.surfaceConfiguration.width,
                       webgpu::g_graphicsConfig.surfaceConfiguration.height);
  pass.Draw(3);]=])
    set(_gekkoaot_edge_replacement [=[  set_present_viewport(pass, viewport,
                       webgpu::g_graphicsConfig.surfaceConfiguration.width,
                       webgpu::g_graphicsConfig.surfaceConfiguration.height);
  // GEKKOAOT_PRESENT_EDGE_CLIP_V175: retain the black clear at the left and
  // bottom of the 4:3 pillarbox; full-window aspect retains all its pixels.
  if (viewport.left > 0.f && viewport.width > 2.f && viewport.height > 1.f) {
    pass.SetScissorRect(static_cast<uint32_t>(viewport.left) + 1u,
                        static_cast<uint32_t>(viewport.top),
                        static_cast<uint32_t>(viewport.width) - 2u,
                        static_cast<uint32_t>(viewport.height) - 1u);
  }
  pass.Draw(3);]=])
    string(FIND "${_gekkoaot_aurora_main_text}" "${_gekkoaot_edge_anchor}" _gekkoaot_edge_pos)
    if(_gekkoaot_edge_pos EQUAL -1)
      message(FATAL_ERROR "Pinned Aurora aurora.cpp changed: VI scanout edge anchor missing")
    endif()
    string(REPLACE "${_gekkoaot_edge_anchor}" "${_gekkoaot_edge_replacement}"
      _gekkoaot_aurora_main_text "${_gekkoaot_aurora_main_text}")
    file(WRITE "${_gekkoaot_aurora_main}" "${_gekkoaot_aurora_main_text}")
  endif()
endif()

# GekkoAOT v158 selective orthographic widescreen correction.
# Perspective draws are always Hor+ corrected. Orthographic UI draws are also
# counter-scaled so final 4:3->window scanout does not flatten the HUD. The one
# class that must remain at retail width is screen-feedback/post-process work:
# Aurora already tracks textures produced by GXCopyTex in copyTextures, so use
# that provenance instead of treating every orthographic pass the same.
set(_gekkoaot_aurora_shader_info "${GEKKOAOT_AURORA_SOURCE}/lib/gx/shader_info.cpp")
if(EXISTS "${_gekkoaot_aurora_shader_info}")
  file(READ "${_gekkoaot_aurora_shader_info}" _gekkoaot_shader_info_text)
  if(NOT _gekkoaot_shader_info_text MATCHES "GEKKOAOT_WIDESCREEN_PROJECTION_V166")
    set(_gekkoaot_wide_decl_anchor [=[#include <tracy/Tracy.hpp>

namespace aurora::gx {]=])
    set(_gekkoaot_wide_decl_replacement [=[#include <tracy/Tracy.hpp>

// GEKKOAOT_WIDESCREEN_PROJECTION_V166
extern "C" float gekkoaot_native_gx_widescreen_x_scale() noexcept;
extern "C" float gekkoaot_native_gx_widescreen_projection_scale(float, float, float, float) noexcept;
extern "C" bool gekkoaot_native_gx_screen_copy(int, int, float, float) noexcept;
extern "C" void gekkoaot_native_gx_widescreen_diag(int, float, float, float, float,
    int, int, int, int, bool, bool, float) noexcept;

namespace aurora::gx {]=])
    set(_gekkoaot_wide_proj_anchor [=[  auto proj = g_gxState.proj;
]=])
    set(_gekkoaot_wide_proj_replacement [=[  auto proj = g_gxState.proj;
  const auto& gekkoaotView = g_gxState.logicalViewport;
  const bool gekkoaotOffscreen = gfx::is_offscreen();
  const float gekkoaotWideX = g_gxState.projType == GX_PERSPECTIVE
      ? gekkoaot_native_gx_widescreen_projection_scale(
            proj.m0[0], proj.m1[1], gekkoaotView.width, gekkoaotView.height)
      : gekkoaot_native_gx_widescreen_x_scale();

  // v158: keep normal 2D/UI at the same physical proportions as 4:3 while
  // final scanout fills the widescreen window. The exception is an
  // orthographic draw sampling a texture produced by GXCopyTex: that is
  // screen-feedback/post-process data and must keep the full retail EFB width,
  // otherwise the old 4:3 boundary becomes a black/stale vertical strip.
  bool gekkoaotOrthoSamplesEfbCopy = false;
  if (g_gxState.projType == GX_ORTHOGRAPHIC) {
    for (u32 i = 0; i < MaxTextures; ++i) {
      if (!info.sampledTextures.test(i))
        continue;
      const auto& obj = g_gxState.loadedTextures[i];
      const auto copy = g_gxState.copyTextures.find(obj.data);
      if (obj.data != nullptr && copy != g_gxState.copyTextures.end() &&
          gekkoaot_native_gx_screen_copy(copy->second.guestSource.width,
              copy->second.guestSource.height, gekkoaotView.width, gekkoaotView.height)) {
        gekkoaotOrthoSamplesEfbCopy = true;
        break;
      }
    }
  }

  const bool gekkoaotApplyWideX =
      (g_gxState.projType == GX_PERSPECTIVE && !gekkoaotOffscreen) ||
      (g_gxState.projType == GX_ORTHOGRAPHIC && !gekkoaotOrthoSamplesEfbCopy);
  if (gekkoaotWideX != 1.0f && gekkoaotApplyWideX) {
    proj.m0 = proj.m0 * Vec4{gekkoaotWideX, gekkoaotWideX, gekkoaotWideX, gekkoaotWideX};
  }
  const auto& gekkoaotScissor = g_gxState.logicalScissor;
  gekkoaot_native_gx_widescreen_diag(static_cast<int>(g_gxState.projType),
      gekkoaotView.left, gekkoaotView.top, gekkoaotView.width, gekkoaotView.height,
      gekkoaotScissor.x, gekkoaotScissor.y, gekkoaotScissor.width, gekkoaotScissor.height,
      gekkoaotOffscreen, gekkoaotOrthoSamplesEfbCopy,
      gekkoaotApplyWideX ? gekkoaotWideX : 1.f);
]=])
    string(FIND "${_gekkoaot_shader_info_text}" "${_gekkoaot_wide_decl_anchor}" _gekkoaot_wide_decl_pos)
    string(FIND "${_gekkoaot_shader_info_text}" "${_gekkoaot_wide_proj_anchor}" _gekkoaot_wide_proj_pos)
    if(_gekkoaot_wide_decl_pos EQUAL -1 OR _gekkoaot_wide_proj_pos EQUAL -1)
      message(FATAL_ERROR "Pinned Aurora shader_info.cpp changed: widescreen projection anchors missing")
    endif()
    string(REPLACE "${_gekkoaot_wide_decl_anchor}" "${_gekkoaot_wide_decl_replacement}"
      _gekkoaot_shader_info_text "${_gekkoaot_shader_info_text}")
    string(REPLACE "${_gekkoaot_wide_proj_anchor}" "${_gekkoaot_wide_proj_replacement}"
      _gekkoaot_shader_info_text "${_gekkoaot_shader_info_text}")
    file(WRITE "${_gekkoaot_aurora_shader_info}" "${_gekkoaot_shader_info_text}")
    message(STATUS "gekkoaot: Aurora v158 selective-ortho widescreen overlay installed (UI corrected, EFB feedback preserved)")
  endif()
endif()

# HPPOA uses GX_TG_TEX0 while TEX0 is absent from the current vertex format.
# GX supplies the default (0,0) coordinate in that case. Aurora's pinned
# shader generator instead aborts on the missing attribute.
set(_gekkoaot_aurora_shader "${GEKKOAOT_AURORA_SOURCE}/lib/gx/shader.cpp")
if(EXISTS "${_gekkoaot_aurora_shader}")
  file(READ "${_gekkoaot_aurora_shader}" _gekkoaot_shader_text)
  if(NOT _gekkoaot_shader_text MATCHES "GEKKOAOT_MISSING_TEXCOORD_V175")
    set(_gekkoaot_missing_tex_anchor [=[    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
      return "vec4f(0.0, 0.0, 0.0, 0.0)"s;
    }
    UNLIKELY FATAL("unmapped vtx attr {}", underlying(attr));]=])
    set(_gekkoaot_missing_tex_replacement [=[    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
      return "vec4f(0.0, 0.0, 0.0, 0.0)"s;
    }
    // GEKKOAOT_MISSING_TEXCOORD_V175: missing GX texture attributes supply
    // zero ST; the texgen code provides the default homogeneous coordinates.
    if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
      return "vec2f(0.0, 0.0)"s;
    }
    UNLIKELY FATAL("unmapped vtx attr {}", underlying(attr));]=])
    string(FIND "${_gekkoaot_shader_text}" "${_gekkoaot_missing_tex_anchor}"
      _gekkoaot_missing_tex_pos)
    if(_gekkoaot_missing_tex_pos EQUAL -1)
      message(FATAL_ERROR "Pinned Aurora shader.cpp changed: missing texcoord anchor missing")
    endif()
    string(REPLACE "${_gekkoaot_missing_tex_anchor}" "${_gekkoaot_missing_tex_replacement}"
      _gekkoaot_shader_text "${_gekkoaot_shader_text}")
    file(WRITE "${_gekkoaot_aurora_shader}" "${_gekkoaot_shader_text}")
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

# GEKKOAOT_BUILD_UNBLOCK_V159
# Aurora 9c0bf66f asks for an xxHash CMake package first.  Arch and several
# other distributions ship libxxhash + xxhash.h without xxHashConfig.cmake,
# which makes Aurora fall through to FetchContent.  That unnecessarily turns
# an otherwise local build into a network-dependent configure and can leave a
# half-populated _deps tree after an interrupted download.  Publish the normal
# xxHash::xxhash target ourselves when a system installation is present;
# Aurora will keep its existing FetchContent fallback when it is not.
if(NOT TARGET xxHash::xxhash)
  # v159e: keep executable search roots such as /usr/bin out of C/C++ include
  # paths.  Also clear stale results from the previous generic find_path().
  unset(GEKKOAOT_XXHASH_INCLUDE_DIR CACHE)
  unset(GEKKOAOT_XXHASH_LIBRARY CACHE)

  find_path(GEKKOAOT_XXHASH_INCLUDE_DIR
    NAMES xxhash.h
    PATHS /usr/include /usr/local/include
    NO_DEFAULT_PATH)
  find_library(GEKKOAOT_XXHASH_LIBRARY
    NAMES xxhash libxxhash
    PATHS /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64
    NO_DEFAULT_PATH)

  if(GEKKOAOT_XXHASH_INCLUDE_DIR AND GEKKOAOT_XXHASH_LIBRARY)
    add_library(gekkoaot_system_xxhash UNKNOWN IMPORTED GLOBAL)
    set_target_properties(gekkoaot_system_xxhash PROPERTIES
      IMPORTED_LOCATION "${GEKKOAOT_XXHASH_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${GEKKOAOT_XXHASH_INCLUDE_DIR}")
    add_library(xxHash::xxhash ALIAS gekkoaot_system_xxhash)
    message(STATUS "gekkoaot: using system xxhash: ${GEKKOAOT_XXHASH_LIBRARY} include=${GEKKOAOT_XXHASH_INCLUDE_DIR}")
  endif()
endif()

# v161f: compose safely with gekkoaot-storage-staging-v4. That patch replaces
# the upstream numeric StorageBufferSize literal with the
# GEKKOAOT_AURORA_STORAGE_BUFFER_SIZE macro (currently 64 MiB). Treat that as
# authoritative and never shrink it. Numeric upstream/older forms are still
# grown to at least 32 MiB. Unknown future forms are left untouched instead of
# making the entire Aurora configure fail.
set(_gekkoaot_aurora_resources_hpp "${GEKKOAOT_AURORA_SOURCE}/lib/gfx/resources.hpp")
if(EXISTS "${_gekkoaot_aurora_resources_hpp}")
  file(READ "${_gekkoaot_aurora_resources_hpp}" _gekkoaot_resources_text)

  if(_gekkoaot_resources_text MATCHES "GEKKOAOT_AURORA_STORAGE_BUFFER_SIZE")
    message(STATUS
      "GEKKOAOT_STORAGE_ARENA_V161F=1 storage-mib=64 action=keep-storage-staging-v4 policy=no-shrink")
  else()
    string(REGEX MATCH
      "inline constexpr uint64_t StorageBufferSize = ([0-9]+);"
      _gekkoaot_storage_match "${_gekkoaot_resources_text}")

    if(_gekkoaot_storage_match)
      set(_gekkoaot_storage_bytes "${CMAKE_MATCH_1}")
      if(_gekkoaot_storage_bytes LESS 33554432)
        string(REGEX REPLACE
          "inline constexpr uint64_t StorageBufferSize = [0-9]+;[^\n]*"
          "// GEKKOAOT_STORAGE_ARENA_V161F\ninline constexpr uint64_t StorageBufferSize = 33554432; // 32 MiB"
          _gekkoaot_resources_text "${_gekkoaot_resources_text}")
        file(WRITE "${_gekkoaot_aurora_resources_hpp}" "${_gekkoaot_resources_text}")
        message(STATUS
          "GEKKOAOT_STORAGE_ARENA_V161F=1 storage-mib=32 action=grow policy=reduce-dawn-map-rollovers")
      else()
        math(EXPR _gekkoaot_storage_mib "${_gekkoaot_storage_bytes} / 1048576")
        message(STATUS
          "GEKKOAOT_STORAGE_ARENA_V161F=1 storage-mib=${_gekkoaot_storage_mib} action=keep-existing policy=no-shrink")
      endif()
    else()
      message(WARNING
        "GEKKOAOT_STORAGE_ARENA_V161F=1 action=leave-unknown-form policy=non-blocking")
    endif()
  endif()
endif()

# v161c: install the multi-state GX pipeline cache after the controller has
# composed all Aurora source patches. Keeping this out of gekkoaotctl.cpp
# makes the optimization resilient to local controller edits/rebases.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_LIST_DIR}/aurora_pipeline_multistate_v161.py"
          "${GEKKOAOT_AURORA_SOURCE}"
  RESULT_VARIABLE GEKKOAOT_AURORA_V161_RESULT
  OUTPUT_VARIABLE GEKKOAOT_AURORA_V161_OUTPUT
  ERROR_VARIABLE GEKKOAOT_AURORA_V161_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT GEKKOAOT_AURORA_V161_RESULT EQUAL 0)
  message(FATAL_ERROR
    "GekkoAOT v161 Aurora multi-state compose failed: ${GEKKOAOT_AURORA_V161_ERROR}")
endif()
if(GEKKOAOT_AURORA_V161_OUTPUT)
  message(STATUS "${GEKKOAOT_AURORA_V161_OUTPUT}")
endif()

# v161g: replace per-overflow CreateBuffer(mappedAtCreation) churn from
# storage-staging-v4 with a bounded persistent MapAsync-remapped pool.
execute_process(
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_LIST_DIR}/aurora_storage_pool_v161g.py"
          "${GEKKOAOT_AURORA_SOURCE}"
  RESULT_VARIABLE GEKKOAOT_AURORA_STORAGE_POOL_RESULT
  OUTPUT_VARIABLE GEKKOAOT_AURORA_STORAGE_POOL_OUTPUT
  ERROR_VARIABLE GEKKOAOT_AURORA_STORAGE_POOL_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT GEKKOAOT_AURORA_STORAGE_POOL_RESULT EQUAL 0)
  message(FATAL_ERROR
    "GekkoAOT v161g Aurora storage pool compose failed: ${GEKKOAOT_AURORA_STORAGE_POOL_ERROR}")
endif()
if(GEKKOAOT_AURORA_STORAGE_POOL_OUTPUT)
  message(STATUS "${GEKKOAOT_AURORA_STORAGE_POOL_OUTPUT}")
endif()

# v161h: persistent indexed-array snapshots above the transient storage arena.
execute_process(
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_LIST_DIR}/aurora_persistent_array_cache_v161h.py"
          "${GEKKOAOT_AURORA_SOURCE}"
  RESULT_VARIABLE GEKKOAOT_AURORA_ARRAY_CACHE_RESULT
  OUTPUT_VARIABLE GEKKOAOT_AURORA_ARRAY_CACHE_OUTPUT
  ERROR_VARIABLE GEKKOAOT_AURORA_ARRAY_CACHE_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_STRIP_TRAILING_WHITESPACE)
if(NOT GEKKOAOT_AURORA_ARRAY_CACHE_RESULT EQUAL 0)
  message(FATAL_ERROR
    "GekkoAOT v161h Aurora persistent array cache compose failed: ${GEKKOAOT_AURORA_ARRAY_CACHE_ERROR}")
endif()
if(GEKKOAOT_AURORA_ARRAY_CACHE_OUTPUT)
  message(STATUS "${GEKKOAOT_AURORA_ARRAY_CACHE_OUTPUT}")
endif()

execute_process(
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_LIST_DIR}/aurora_present_counter_v190.py"
          "${GEKKOAOT_AURORA_SOURCE}"
  RESULT_VARIABLE GEKKOAOT_PRESENT_COUNTER_RESULT)
if(NOT GEKKOAOT_PRESENT_COUNTER_RESULT EQUAL 0)
  message(FATAL_ERROR "GekkoAOT Aurora presentation counter compose failed")
endif()

execute_process(
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_LIST_DIR}/aurora_lazy_staging_v236.py"
          "${GEKKOAOT_AURORA_SOURCE}"
  RESULT_VARIABLE GEKKOAOT_LAZY_STAGING_RESULT
  OUTPUT_VARIABLE GEKKOAOT_LAZY_STAGING_OUTPUT
  ERROR_VARIABLE GEKKOAOT_LAZY_STAGING_ERROR)
if(NOT GEKKOAOT_LAZY_STAGING_RESULT EQUAL 0)
  message(FATAL_ERROR "GekkoAOT lazy staging compose failed: ${GEKKOAOT_LAZY_STAGING_ERROR}")
endif()
message(STATUS "${GEKKOAOT_LAZY_STAGING_OUTPUT}")

add_subdirectory("${GEKKOAOT_AURORA_SOURCE}" aurora EXCLUDE_FROM_ALL)

# Aurora's standalone test executables do not link GekkoAOT's bridge. Supply
# the neutral 4:3 values expected by the projection/presentation overlays.
foreach(_gekkoaot_aurora_test IN ITEMS gx_fifo_tests gx_shader_tests gx_texture_cache_tests
                                      render_worker_tests gfx_recording_tests)
  if(TARGET ${_gekkoaot_aurora_test})
    target_sources(${_gekkoaot_aurora_test} PRIVATE aspect_test_stubs.cpp)
  endif()
endforeach()

add_library(gekkoaot-native-gx SHARED bridge.cpp bounding_box.cpp)
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

# v300: the GX worker and Aurora's render worker spend most of their time in
# Aurora's static libraries, which otherwise build with generic flags: every
# cross-file call goes through the PLT and nothing inlines across files. Give
# them the bridge's code-generation policy (LTO, hidden visibility, direct
# calls, native ISA). The plugin's exported surface stays the bridge's
# GEKKOAOT_GX_EXPORT entry points. Opt out: -DGEKKOAOT_GX_AURORA_FAST_CODEGEN=OFF.
option(GEKKOAOT_GX_AURORA_FAST_CODEGEN "Build Aurora with the native GX bridge code policy" ON)
if(GEKKOAOT_GX_AURORA_FAST_CODEGEN AND NOT MSVC)
  foreach(_gekkoaot_aurora_lib IN ITEMS aurora_gx aurora_core aurora_vi)
    if(TARGET ${_gekkoaot_aurora_lib})
      set_target_properties(${_gekkoaot_aurora_lib} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES)
      if(GEKKOAOT_GX_IPO_OK)
        set_property(TARGET ${_gekkoaot_aurora_lib} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
      endif()
      target_compile_options(${_gekkoaot_aurora_lib} PRIVATE
        -O3 -fno-semantic-interposition -fno-plt)
      if(NOT APPLE)
        target_compile_options(${_gekkoaot_aurora_lib} PRIVATE -march=native -mtune=native)
      endif()
    endif()
  endforeach()
  if(UNIX AND NOT APPLE)
    # Bind the plugin's own function references locally, including the
    # prebuilt Dawn archive (ErrorMonad::IsError@plt, RefCounted@plt, ...).
    # SDL stays interposable: the plugin carries SDL3-static, but at runtime it
    # must share the host's libSDL3 instance (input, events, video init).
    set(_gekkoaot_gx_dynamic_list "${CMAKE_CURRENT_BINARY_DIR}/gekkoaot-native-gx.dynamic-list")
    file(WRITE "${_gekkoaot_gx_dynamic_list}" "{\n  SDL_*;\n};\n")
    target_link_options(gekkoaot-native-gx PRIVATE
      -Wl,-Bsymbolic-functions "-Wl,--dynamic-list=${_gekkoaot_gx_dynamic_list}")
  endif()
  message(STATUS "gekkoaot: Aurora fast codegen v300 enabled (ipo=${GEKKOAOT_GX_IPO_OK})")
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

# Decode the raster offset shared by the GX viewport and scissor (BP59).
execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_LIST_DIR}/aurora_scissor_offset_v247.py" "${GEKKOAOT_AURORA_SOURCE}"
  RESULT_VARIABLE _gekkoaot_scissor_status)
if(NOT _gekkoaot_scissor_status EQUAL 0)
  message(FATAL_ERROR "Aurora BP59 offset overlay failed")
endif()

# Preserve projections of EFB regions proven to produce texture copies.
execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_LIST_DIR}/aurora_texture_viewport_v250.py" "${GEKKOAOT_AURORA_SOURCE}"
  RESULT_VARIABLE _gekkoaot_texture_viewport_result)
if(NOT _gekkoaot_texture_viewport_result EQUAL 0)
  message(FATAL_ERROR "Aurora texture viewport overlay failed")
endif()

# GX movie renderers commonly submit integer UVs with manual SU scale=1.
execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_LIST_DIR}/aurora_texcoord_scale_v251.py" "${GEKKOAOT_AURORA_SOURCE}"
  RESULT_VARIABLE _gekkoaot_texcoord_scale_result)
if(NOT _gekkoaot_texcoord_scale_result EQUAL 0)
  message(FATAL_ERROR "Aurora ordinary texture coordinate scale overlay failed")
endif()

execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_LIST_DIR}/aurora_screen_fill_v253.py" "${GEKKOAOT_AURORA_SOURCE}"
  RESULT_VARIABLE _gekkoaot_screen_fill_result)
if(NOT _gekkoaot_screen_fill_result EQUAL 0)
  message(FATAL_ERROR "Aurora screen fill overlay failed")
endif()

execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_LIST_DIR}/aurora_bounding_box_v257.py" "${GEKKOAOT_AURORA_SOURCE}"
  RESULT_VARIABLE _gekkoaot_bounding_box_result)
if(NOT _gekkoaot_bounding_box_result EQUAL 0)
  message(FATAL_ERROR "Aurora native GPU bounding box overlay failed")
endif()

execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_LIST_DIR}/aurora_ui_scissor_v257.py" "${GEKKOAOT_AURORA_SOURCE}"
  RESULT_VARIABLE _gekkoaot_ui_scissor_result)
if(NOT _gekkoaot_ui_scissor_result EQUAL 0)
  message(FATAL_ERROR "Aurora UI scissor overlay failed")
endif()

execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_LIST_DIR}/aurora_bind_group_cache_v297.py" "${GEKKOAOT_AURORA_SOURCE}"
  RESULT_VARIABLE _gekkoaot_bind_group_cache_result)
if(NOT _gekkoaot_bind_group_cache_result EQUAL 0)
  message(FATAL_ERROR "Aurora native bind group cache overlay failed")
endif()

execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_LIST_DIR}/aurora_gx_hotpath_v300.py" "${GEKKOAOT_AURORA_SOURCE}"
  RESULT_VARIABLE _gekkoaot_gx_hotpath_result)
if(NOT _gekkoaot_gx_hotpath_result EQUAL 0)
  message(FATAL_ERROR "Aurora GX hot path v300 overlay failed")
endif()

execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_LIST_DIR}/aurora_native_snapshot_v301.py" "${GEKKOAOT_AURORA_SOURCE}"
  RESULT_VARIABLE _gekkoaot_native_snapshot_result)
if(NOT _gekkoaot_native_snapshot_result EQUAL 0)
  message(FATAL_ERROR "Aurora immutable native snapshot v301 overlay failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/aurora_native_snapshot_v301.py"
  "${CMAKE_CURRENT_LIST_DIR}/aurora_resolved_batch_v302.py"
  "${CMAKE_CURRENT_LIST_DIR}/aurora_native_upload_v314.py"
  "${CMAKE_CURRENT_LIST_DIR}/aurora_native_cache_lifetime_v317.py"
  "${CMAKE_CURRENT_LIST_DIR}/../native_snapshot_atlas.h"
  "${CMAKE_CURRENT_LIST_DIR}/../native_resource_table.h"
  "${CMAKE_CURRENT_LIST_DIR}/../native_index_batch.h")

execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_LIST_DIR}/aurora_resolved_batch_v302.py" "${GEKKOAOT_AURORA_SOURCE}"
  RESULT_VARIABLE _gekkoaot_resolved_batch_result)
if(NOT _gekkoaot_resolved_batch_result EQUAL 0)
  message(FATAL_ERROR "Aurora resolved native draw batch v302 overlay failed")
endif()

execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_LIST_DIR}/aurora_native_upload_v314.py" "${GEKKOAOT_AURORA_SOURCE}"
  RESULT_VARIABLE _gekkoaot_native_upload_result)
if(NOT _gekkoaot_native_upload_result EQUAL 0)
  message(FATAL_ERROR "Aurora native upload arena v314 overlay failed")
endif()

execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_LIST_DIR}/aurora_native_cache_lifetime_v317.py" "${GEKKOAOT_AURORA_SOURCE}"
  RESULT_VARIABLE _gekkoaot_native_cache_lifetime_result)
if(NOT _gekkoaot_native_cache_lifetime_result EQUAL 0)
  message(FATAL_ERROR "Aurora native cache lifetime v317 overlay failed")
endif()
