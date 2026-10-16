// SPDX-License-Identifier: GPL-3.0-or-later
// v359 performance HUD: a small ImGui-drawn panel composited over every
// presented image. ImGui only supplies the font atlas and the draw-list
// tessellation; there is no ImGui context, window or input here.
#include "perf_hud.h"

#include "../graphics_settings.h"
#include "webgpu/gpu.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/resource.h>
#endif
#endif

namespace GekkoAOT::GX::Hud {
namespace {
using Clock = std::chrono::steady_clock;

std::atomic<bool> g_enabled{false};
std::atomic<std::uint32_t> g_items{HudDefaultItems};
std::mutex g_runtime_mutex;
std::array<float, RuntimeValueCount> g_runtime{};
Clock::time_point g_runtime_time{};
std::atomic<std::uint64_t> g_game_frames{0};

// ---- Host load sampling (render worker only) -------------------------------
struct CpuSampler {
  Clock::time_point last{};
  double last_seconds = -1.0;
  float percent = -1.f;

  static double ProcessSeconds() {
#ifdef _WIN32
    FILETIME create{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user)) return -1.0;
    const auto ticks = [](const FILETIME& t) {
      return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32u) | t.dwLowDateTime;
    };
    return double(ticks(kernel) + ticks(user)) * 1e-7;
#elif defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return -1.0;
    return double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
           double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1e-6;
#else
    std::ifstream stat("/proc/self/stat");
    std::string line;
    if (!std::getline(stat, line)) return -1.0;
    const auto close = line.rfind(')');
    if (close == std::string::npos) return -1.0;
    // Fields after the command name start at field 3 (state); utime and
    // stime are fields 14 and 15.
    unsigned long long utime = 0, stime = 0;
    const char* p = line.c_str() + close + 2;
    for (int field = 3; field < 14 && *p; ++field) {
      p = std::strchr(p, ' ');
      if (!p) return -1.0;
      ++p;
    }
    if (std::sscanf(p, "%llu %llu", &utime, &stime) != 2) return -1.0;
    const long ticks = sysconf(_SC_CLK_TCK);
    return ticks > 0 ? double(utime + stime) / double(ticks) : -1.0;
#endif
  }

  void Sample(Clock::time_point now) {
    const double seconds = ProcessSeconds();
    if (seconds < 0.0) return;
    if (last_seconds >= 0.0) {
      const double wall = std::chrono::duration<double>(now - last).count();
      const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
      if (wall > 0.0) percent = float(std::clamp(100.0 * (seconds - last_seconds) / (wall * cores), 0.0, 100.0));
    }
    last = now;
    last_seconds = seconds;
  }
};

struct GpuSampler {
  bool probed = false;
#ifdef __linux__
  // Per-engine counters from this process's DRM fdinfo: busy ns (amdgpu,
  // i915) or busy/total cycles (xe). Keyed by client id and engine.
  struct Counter { double busy = 0.0, total = 0.0; };
  std::map<std::string, Counter> last_counters;
  Clock::time_point last_time{};
  bool fdinfo = true;

  bool SampleFdinfo() {
    std::map<std::string, Counter> counters;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fdinfo", error)) {
      std::ifstream file(entry.path());
      std::string line, client;
      std::vector<std::pair<std::string, Counter>> engines;
      while (std::getline(file, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const auto key = line.substr(0, colon);
        const double value = std::strtod(line.c_str() + colon + 1, nullptr);
        if (key == "drm-client-id") client = line.substr(colon + 1);
        else if (key.rfind("drm-engine-", 0) == 0 && key.find("capacity") == std::string::npos)
          engines.push_back({key.substr(11), {value, -1.0}});
        else if (key.rfind("drm-cycles-", 0) == 0) engines.push_back({key.substr(11), {value, 0.0}});
        else if (key.rfind("drm-total-cycles-", 0) == 0) {
          for (auto& engine : engines)
            if (engine.first == key.substr(17)) engine.second.total = value;
        }
      }
      if (client.empty()) continue;
      for (const auto& [engine, counter] : engines) counters[client + "/" + engine] = counter;
    }
    if (counters.empty()) return false;
    const auto now = Clock::now();
    const double wall_ns = std::chrono::duration<double, std::nano>(now - last_time).count();
    float best = -1.f;
    if (last_time.time_since_epoch().count() != 0 && wall_ns > 0.0) {
      for (const auto& [key, counter] : counters) {
        const auto previous = last_counters.find(key);
        if (previous == last_counters.end()) continue;
        const double busy = counter.busy - previous->second.busy;
        const double total = counter.total >= 0.0 ? counter.total - previous->second.total : wall_ns;
        if (busy >= 0.0 && total > 0.0) best = std::max(best, float(std::min(100.0, 100.0 * busy / total)));
      }
    }
    last_counters = std::move(counters);
    last_time = now;
    if (best >= 0.f) percent = best;
    return true;
  }
#endif
  std::vector<std::string> sysfs;
  void* nvml = nullptr;
  void* device = nullptr;
  int (*get_utilization)(void*, unsigned*) = nullptr;
  float percent = -1.f;

  void Probe() {
    probed = true;
#ifndef _WIN32
    for (int card = 0; card < 8; ++card) {
      const std::string path = "/sys/class/drm/card" + std::to_string(card) + "/device/gpu_busy_percent";
      if (std::ifstream(path)) sysfs.push_back(path);
    }
    if (!sysfs.empty()) return;
    nvml = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    const auto symbol = [&](const char* name) { return nvml ? dlsym(nvml, name) : nullptr; };
#else
    nvml = LoadLibraryA("nvml.dll");
    const auto symbol = [&](const char* name) {
      return nvml ? reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(nvml), name)) : nullptr;
    };
#endif
    using Init = int (*)();
    using Handle = int (*)(unsigned, void**);
    const auto init = reinterpret_cast<Init>(symbol("nvmlInit_v2"));
    const auto handle = reinterpret_cast<Handle>(symbol("nvmlDeviceGetHandleByIndex_v2"));
    get_utilization = reinterpret_cast<int (*)(void*, unsigned*)>(symbol("nvmlDeviceGetUtilizationRates"));
    if (!init || !handle || !get_utilization || init() != 0 || handle(0, &device) != 0) get_utilization = nullptr;
  }

#ifdef _WIN32
  // Windows: the per-process "GPU Engine" counters Task Manager shows (all
  // vendors, Windows 10+). pdh.dll is loaded on demand: no link dependency.
  using OpenQuery = PDH_STATUS(WINAPI*)(LPCWSTR, DWORD_PTR, PDH_HQUERY*);
  using AddCounter = PDH_STATUS(WINAPI*)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
  using Collect = PDH_STATUS(WINAPI*)(PDH_HQUERY);
  using GetArray = PDH_STATUS(WINAPI*)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PPDH_FMT_COUNTERVALUE_ITEM_W);
  HMODULE pdh = nullptr;
  PDH_HQUERY query = nullptr;
  PDH_HCOUNTER counter = nullptr;
  Collect collect = nullptr;
  GetArray get_array = nullptr;
  bool pdh_failed = false;
  std::vector<unsigned char> pdh_items;

  bool SamplePdh() {
    if (pdh_failed) return false;
    if (!query) {
      pdh = LoadLibraryA("pdh.dll");
      const auto symbol = [&](const char* name) {
        return pdh ? reinterpret_cast<void*>(GetProcAddress(pdh, name)) : nullptr;
      };
      const auto open = reinterpret_cast<OpenQuery>(symbol("PdhOpenQueryW"));
      const auto add = reinterpret_cast<AddCounter>(symbol("PdhAddEnglishCounterW"));
      collect = reinterpret_cast<Collect>(symbol("PdhCollectQueryData"));
      get_array = reinterpret_cast<GetArray>(symbol("PdhGetFormattedCounterArrayW"));
      wchar_t path[128];
      std::swprintf(path, 128, L"\\GPU Engine(pid_%lu_*)\\Utilization Percentage", GetCurrentProcessId());
      if (!open || !add || !collect || !get_array || open(nullptr, 0, &query) != ERROR_SUCCESS ||
          add(query, path, 0, &counter) != ERROR_SUCCESS) {
        pdh_failed = true;
        return false;
      }
      collect(query);
      return true;
    }
    if (collect(query) != ERROR_SUCCESS) return true;
    DWORD bytes = 0, count = 0;
    if (get_array(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != static_cast<PDH_STATUS>(PDH_MORE_DATA) ||
        bytes == 0)
      return true;
    pdh_items.resize(bytes);
    auto* items = reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(pdh_items.data());
    if (get_array(counter, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return true;
    // Busiest engine type (3D, compute, copy...), summed over its instances.
    std::map<std::wstring, double> engines;
    for (DWORD i = 0; i < count; ++i) {
      if (items[i].FmtValue.CStatus != ERROR_SUCCESS || !items[i].szName) continue;
      const std::wstring name = items[i].szName;
      const auto type = name.find(L"engtype_");
      engines[type == std::wstring::npos ? name : name.substr(type)] += items[i].FmtValue.doubleValue;
    }
    double best = 0.0;
    for (const auto& [name, value] : engines) best = std::max(best, value);
    percent = float(std::clamp(best, 0.0, 100.0));
    return true;
  }
#endif

  void Sample() {
#ifdef __linux__
    if (fdinfo && SampleFdinfo()) return;
    fdinfo = false;
#endif
#ifdef _WIN32
    if (SamplePdh()) return;
#endif
    if (!probed) Probe();
    if (!sysfs.empty()) {
      int best = -1;
      for (const auto& path : sysfs) {
        int value = -1;
        if (std::ifstream(path) >> value) best = std::max(best, value);
      }
      percent = best >= 0 ? float(best) : -1.f;
    } else if (get_utilization) {
      unsigned rates[2]{}; // nvmlUtilization_t { gpu, memory }
      percent = get_utilization(device, rates) == 0 ? float(rates[0]) : -1.f;
    }
  }
};

// ---- Rendering -----------------------------------------------------------
struct Renderer {
  ImFontAtlas atlas;
  ImFont* font = nullptr;
  float font_px = 0.f;
  std::vector<unsigned char> font_file;
  std::unique_ptr<ImDrawListSharedData> shared;
  wgpu::Texture texture;
  wgpu::TextureView view;
  wgpu::Sampler sampler;
  wgpu::BindGroupLayout layout;
  wgpu::RenderPipeline pipeline;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  wgpu::Buffer uniforms, vertices, indices;
  std::uint64_t vertex_capacity = 0, index_capacity = 0;
  wgpu::BindGroup bind_group;

  // Present timing (display FPS/frame time) and samplers.
  std::array<double, 240> intervals{};
  std::size_t interval_count = 0, interval_next = 0;
  Clock::time_point last_present{};
  Clock::time_point last_sample{};
  std::uint64_t last_game_frames = 0;
  Clock::time_point last_game_time{};
  float display_fps = 0.f, frame_ms = 0.f, game_fps = 0.f;
  std::deque<Clock::time_point> presents;
  CpuSampler cpu;
  GpuSampler gpu;
};

Renderer& renderer() {
  static Renderer r;
  return r;
}

bool LoadFont(Renderer& r, float px) {
  r.atlas.Clear();
  ImFontConfig config;
  config.OversampleH = 2;
  config.OversampleV = 1;
  config.PixelSnapH = false;
  config.FontDataOwnedByAtlas = false;
  if (r.font_file.empty()) {
    std::vector<std::string> candidates;
    if (const char* custom = std::getenv("GEKKOAOT_HUD_FONT")) candidates.emplace_back(custom);
#ifdef _WIN32
    candidates.insert(candidates.end(), {"C:\\Windows\\Fonts\\segoeuib.ttf", "C:\\Windows\\Fonts\\arialbd.ttf"});
#elif defined(__APPLE__)
    candidates.insert(candidates.end(), {"/System/Library/Fonts/Supplemental/Arial Bold.ttf",
                                         "/Library/Fonts/Arial Bold.ttf"});
#else
    candidates.insert(candidates.end(),
                      {"/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
                       "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                       "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
                       "/usr/share/fonts/noto/NotoSans-Bold.ttf",
                       "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
                       "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
                       "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"});
#endif
    for (const auto& path : candidates) {
      std::ifstream file(path, std::ios::binary);
      if (!file) continue;
      r.font_file.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
      if (r.font_file.size() > 1024u) break;
      r.font_file.clear();
    }
  }
  r.font = r.font_file.empty()
               ? r.atlas.AddFontDefault(&config)
               : r.atlas.AddFontFromMemoryTTF(r.font_file.data(), int(r.font_file.size()), px, &config);
  if (!r.font || !r.atlas.Build()) return false;
  r.font_px = px;
  unsigned char* pixels = nullptr;
  int width = 0, height = 0;
  r.atlas.GetTexDataAsAlpha8(&pixels, &width, &height);
  if (!pixels || width <= 0 || height <= 0) return false;

  using aurora::webgpu::g_device;
  using aurora::webgpu::g_queue;
  const wgpu::TextureDescriptor descriptor{
      .label = "GekkoAOT HUD font",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .size = {std::uint32_t(width), std::uint32_t(height), 1},
      .format = wgpu::TextureFormat::R8Unorm,
  };
  r.texture = g_device.CreateTexture(&descriptor);
  r.view = r.texture.CreateView();
  const wgpu::TexelCopyTextureInfo destination{.texture = r.texture};
  const wgpu::TexelCopyBufferLayout source{.bytesPerRow = std::uint32_t(width), .rowsPerImage = std::uint32_t(height)};
  g_queue.WriteTexture(&destination, pixels, std::size_t(width) * height, &source, &descriptor.size);

  r.shared = std::make_unique<ImDrawListSharedData>();
  r.shared->Font = r.font;
  r.shared->FontSize = r.font->FontSize;
  r.shared->FontScale = 1.f;
  r.shared->TexUvWhitePixel = r.atlas.TexUvWhitePixel;
  r.shared->TexUvLines = r.atlas.TexUvLines;
  r.shared->CurveTessellationTol = 1.25f;
  r.shared->SetCircleTessellationMaxError(0.30f);
  r.shared->InitialFlags = ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;
  r.bind_group = {};
  return true;
}

bool EnsurePipeline(Renderer& r, wgpu::TextureFormat format) {
  using aurora::webgpu::g_device;
  if (r.pipeline && r.format == format) return true;
  r.format = format;
  wgpu::ShaderSourceWGSL source{};
  source.code = R"WGSL(
struct Screen { scale: vec4<f32> };
struct VertexIn { @location(0) pos: vec2<f32>, @location(1) uv: vec2<f32>, @location(2) color: vec4<f32> };
struct VertexOut { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32>, @location(1) color: vec4<f32> };
@group(0) @binding(0) var<uniform> screen: Screen;
@group(0) @binding(1) var font_sampler: sampler;
@group(0) @binding(2) var font: texture_2d<f32>;
@vertex fn vs_main(v: VertexIn) -> VertexOut {
  var out: VertexOut;
  out.pos = vec4<f32>(v.pos.x * screen.scale.x - 1.0, 1.0 - v.pos.y * screen.scale.y, 0.0, 1.0);
  out.uv = v.uv;
  out.color = v.color;
  return out;
}
@fragment fn fs_main(v: VertexOut) -> @location(0) vec4<f32> {
  let coverage = textureSample(font, font_sampler, v.uv).r;
  return vec4<f32>(v.color.rgb, v.color.a * coverage);
}
)WGSL";
  const wgpu::ShaderModuleDescriptor module_descriptor{.nextInChain = &source, .label = "GekkoAOT HUD shader"};
  const auto module = g_device.CreateShaderModule(&module_descriptor);
  const std::array entries{
      wgpu::BindGroupLayoutEntry{.binding = 0, .visibility = wgpu::ShaderStage::Vertex,
                                 .buffer = {.type = wgpu::BufferBindingType::Uniform}},
      wgpu::BindGroupLayoutEntry{.binding = 1, .visibility = wgpu::ShaderStage::Fragment,
                                 .sampler = {.type = wgpu::SamplerBindingType::Filtering}},
      wgpu::BindGroupLayoutEntry{.binding = 2, .visibility = wgpu::ShaderStage::Fragment,
                                 .texture = {.sampleType = wgpu::TextureSampleType::Float,
                                             .viewDimension = wgpu::TextureViewDimension::e2D}},
  };
  const wgpu::BindGroupLayoutDescriptor layout_descriptor{.entryCount = entries.size(), .entries = entries.data()};
  r.layout = g_device.CreateBindGroupLayout(&layout_descriptor);
  const wgpu::PipelineLayoutDescriptor pipeline_layout{.bindGroupLayoutCount = 1, .bindGroupLayouts = &r.layout};
  const std::array attributes{
      wgpu::VertexAttribute{.format = wgpu::VertexFormat::Float32x2, .offset = offsetof(ImDrawVert, pos),
                            .shaderLocation = 0},
      wgpu::VertexAttribute{.format = wgpu::VertexFormat::Float32x2, .offset = offsetof(ImDrawVert, uv),
                            .shaderLocation = 1},
      wgpu::VertexAttribute{.format = wgpu::VertexFormat::Unorm8x4, .offset = offsetof(ImDrawVert, col),
                            .shaderLocation = 2},
  };
  const wgpu::VertexBufferLayout vertex_layout{.arrayStride = sizeof(ImDrawVert),
                                               .attributeCount = attributes.size(),
                                               .attributes = attributes.data()};
  const wgpu::BlendState blend{
      .color = {.operation = wgpu::BlendOperation::Add, .srcFactor = wgpu::BlendFactor::SrcAlpha,
                .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha},
      .alpha = {.operation = wgpu::BlendOperation::Add, .srcFactor = wgpu::BlendFactor::One,
                .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha},
  };
  const wgpu::ColorTargetState target{.format = format, .blend = &blend};
  const wgpu::FragmentState fragment{.module = module, .entryPoint = "fs_main", .targetCount = 1, .targets = &target};
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = "GekkoAOT HUD pipeline",
      .layout = g_device.CreatePipelineLayout(&pipeline_layout),
      .vertex = {.module = module, .entryPoint = "vs_main", .bufferCount = 1, .buffers = &vertex_layout},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
      .multisample = {.count = 1, .mask = UINT32_MAX},
      .fragment = &fragment,
  };
  r.pipeline = g_device.CreateRenderPipeline(&descriptor);
  const wgpu::SamplerDescriptor sampler{.magFilter = wgpu::FilterMode::Linear, .minFilter = wgpu::FilterMode::Linear};
  r.sampler = g_device.CreateSampler(&sampler);
  const wgpu::BufferDescriptor uniforms{.label = "GekkoAOT HUD screen",
                                        .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                        .size = 16};
  r.uniforms = g_device.CreateBuffer(&uniforms);
  r.bind_group = {};
  return bool(r.pipeline);
}

void EnsureBuffer(wgpu::Buffer& buffer, std::uint64_t& capacity, std::uint64_t bytes, wgpu::BufferUsage usage,
                  const char* label) {
  if (buffer && capacity >= bytes) return;
  capacity = std::max<std::uint64_t>(bytes * 2u, 16384u);
  const wgpu::BufferDescriptor descriptor{.label = label, .usage = usage | wgpu::BufferUsage::CopyDst,
                                          .size = capacity};
  buffer = aurora::webgpu::g_device.CreateBuffer(&descriptor);
}

void UpdateStats(Renderer& r, Clock::time_point now) {
  if (r.last_present.time_since_epoch().count() != 0) {
    const double interval = std::chrono::duration<double, std::milli>(now - r.last_present).count();
    if (interval > 0.0 && interval < 1000.0) {
      r.intervals[r.interval_next] = interval;
      r.interval_next = (r.interval_next + 1u) % r.intervals.size();
      r.interval_count = std::min(r.interval_count + 1u, r.intervals.size());
    }
  }
  r.last_present = now;
  r.presents.push_back(now);
  while (!r.presents.empty() && now - r.presents.front() > std::chrono::seconds(1)) r.presents.pop_front();
  if (now - r.last_sample < std::chrono::milliseconds(500) && r.last_sample.time_since_epoch().count() != 0)
    return;
  const double window = r.presents.size() > 1u
                            ? std::chrono::duration<double>(r.presents.back() - r.presents.front()).count()
                            : 0.0;
  r.display_fps = window > 0.0 ? float(double(r.presents.size() - 1u) / window) : 0.f;
  r.frame_ms = r.display_fps > 0.f ? 1000.f / r.display_fps : 0.f;
  const auto frames = g_game_frames.load(std::memory_order_relaxed);
  if (r.last_game_time.time_since_epoch().count() != 0) {
    const double seconds = std::chrono::duration<double>(now - r.last_game_time).count();
    if (seconds > 0.0) r.game_fps = float(double(frames - r.last_game_frames) / seconds);
  }
  r.last_game_frames = frames;
  r.last_game_time = now;
  const auto items = g_items.load(std::memory_order_relaxed);
  if (items & HudCpu) r.cpu.Sample(now);
  if (items & HudGpu) r.gpu.Sample();
  r.last_sample = now;
}

struct Row {
  const char* label;
  ImU32 accent;
  char value[32];
  char extra[48];
};
} // namespace

void Configure(bool enabled, std::uint32_t items) {
  g_enabled.store(enabled, std::memory_order_relaxed);
  g_items.store(items, std::memory_order_relaxed);
}

void PublishRuntime(const float* values, std::uint32_t count) {
  if (!values) return;
  std::lock_guard lock{g_runtime_mutex};
  for (std::uint32_t i = 0; i < count && i < RuntimeValueCount; ++i) g_runtime[i] = values[i];
  g_runtime_time = Clock::now();
}

void NoteGameFrame() { g_game_frames.fetch_add(1, std::memory_order_relaxed); }

void Draw(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& target, wgpu::TextureFormat format,
          std::uint32_t width, std::uint32_t height) {
  auto& r = renderer();
  const auto now = Clock::now();
  if (!g_enabled.load(std::memory_order_relaxed) || !target || width < 64u || height < 64u) {
    r.last_present = {};
    return;
  }
  UpdateStats(r, now);
  const float px = std::clamp(std::round(float(height) / 46.f), 13.f, 44.f);
  if (r.font_px != px && !LoadFont(r, px)) return;
  if (!EnsurePipeline(r, format)) return;

  std::array<float, RuntimeValueCount> runtime{};
  bool runtime_live = false;
  {
    std::lock_guard lock{g_runtime_mutex};
    runtime = g_runtime;
    runtime_live = now - g_runtime_time < std::chrono::seconds(3);
  }
  const auto items = g_items.load(std::memory_order_relaxed);
  std::vector<Row> rows;
  const auto add = [&](const char* label, ImU32 accent) -> Row& {
    rows.push_back({label, accent, {}, {}});
    return rows.back();
  };
  if (items & HudFps) {
    auto& row = add("FPS", IM_COL32(110, 205, 255, 255));
    std::snprintf(row.value, sizeof(row.value), "%.0f", r.display_fps);
    if (items & HudFrameTime) std::snprintf(row.extra, sizeof(row.extra), "%.1f ms", r.frame_ms);
  } else if (items & HudFrameTime) {
    auto& row = add("FRAME", IM_COL32(110, 205, 255, 255));
    std::snprintf(row.value, sizeof(row.value), "%.1f", r.frame_ms);
    std::snprintf(row.extra, sizeof(row.extra), "ms");
  }
  if (items & HudGameFps) {
    auto& row = add("GAME", IM_COL32(150, 230, 140, 255));
    std::snprintf(row.value, sizeof(row.value), "%.0f", runtime_live ? runtime[GameFps] : r.game_fps);
    std::snprintf(row.extra, sizeof(row.extra), "fps");
  }
  if ((items & HudVps) && runtime_live) {
    auto& row = add("VPS", IM_COL32(200, 170, 255, 255));
    std::snprintf(row.value, sizeof(row.value), "%.0f", runtime[Vps]);
    std::snprintf(row.extra, sizeof(row.extra), "/ %.0f", runtime[NominalVps]);
  }
  if ((items & HudSpeed) && runtime_live) {
    auto& row = add("SPEED", IM_COL32(255, 205, 110, 255));
    std::snprintf(row.value, sizeof(row.value), "%.0f%%", runtime[Speed]);
  }
  if ((items & HudCpu) && r.cpu.percent >= 0.f) {
    auto& row = add("CPU", IM_COL32(95, 170, 255, 255));
    std::snprintf(row.value, sizeof(row.value), "%.0f%%", r.cpu.percent);
    if (runtime_live && runtime[MainCpu] >= 0.f)
      std::snprintf(row.extra, sizeof(row.extra), "main %.0f%%", runtime[MainCpu]);
  }
  if ((items & HudGpu) && r.gpu.percent >= 0.f) {
    auto& row = add("GPU", IM_COL32(120, 225, 160, 255));
    std::snprintf(row.value, sizeof(row.value), "%.0f%%", r.gpu.percent);
  }
  const bool graph = (items & HudGraph) && r.interval_count > 1u;
  if (rows.empty() && !graph) return;

  // Layout: label column, right-aligned value column, unit/extra column.
  const float pad = std::round(px * 0.8f), gap = std::round(px * 0.8f), line = std::round(px * 1.3f);
  const float header_h = std::round(px * 1.7f);
  float label_w = 0.f, value_w = r.font->CalcTextSizeA(px, FLT_MAX, 0.f, "100%").x, extra_w = 0.f;
  for (const auto& row : rows) {
    label_w = std::max(label_w, r.font->CalcTextSizeA(px, FLT_MAX, 0.f, row.label).x);
    value_w = std::max(value_w, r.font->CalcTextSizeA(px, FLT_MAX, 0.f, row.value).x);
    if (row.extra[0]) extra_w = std::max(extra_w, r.font->CalcTextSizeA(px, FLT_MAX, 0.f, row.extra).x);
  }
  float panel_w = pad * 2.f + label_w + gap + value_w + (extra_w > 0.f ? gap * 0.5f + extra_w : 0.f);
  const float graph_h = graph ? std::round(px * 1.6f) : 0.f;
  if (graph) panel_w = std::max(panel_w, std::round(px * 9.f));
  const float panel_h = header_h + pad * 2.f + line * float(rows.size()) + (graph ? graph_h + (rows.empty() ? 0.f : pad * 0.5f) : 0.f);
  const ImVec2 origin(std::round(px * 0.7f), std::round(px * 0.7f));

  ImDrawList list(r.shared.get());
  list._ResetForNewFrame();
  r.shared->ClipRectFullscreen = ImVec4(0.f, 0.f, float(width), float(height));
  list.PushClipRectFullScreen();
  list.PushTextureID(ImTextureID{});
  const float rounding = std::round(px * 0.75f);
  // Soft elevation, translucent neutral glass and a restrained top highlight.
  // This uses the existing HUD draw pass, with no readback or blur pass per frame.
  for (int i=5; i>0; --i) {
    const float spread=float(i)*px*.10f;
    list.AddRectFilled(ImVec2(origin.x-spread,origin.y+px*.25f-spread),
                       ImVec2(origin.x+panel_w+spread,origin.y+panel_h+px*.25f+spread),
                       IM_COL32(0,0,0,10),rounding+spread);
  }
  list.AddRectFilled(origin, ImVec2(origin.x + panel_w, origin.y + panel_h), IM_COL32(34,34,34,205), rounding);
  list.AddRect(origin, ImVec2(origin.x + panel_w, origin.y + panel_h), IM_COL32(255,255,255,42), rounding,0,1.f);
  const float dot=std::max(1.f,std::round(px/14.f));
  const char* mark="GEKKOAOT";
  const auto glyph=[](char c)->std::array<unsigned,7> {
    switch(c) {
      case 'G':return {14,17,16,23,17,17,14};
      case 'E':return {31,16,16,30,16,16,31};
      case 'K':return {17,18,20,24,20,18,17};
      case 'O':return {14,17,17,17,17,17,14};
      case 'A':return {14,17,17,31,17,17,17};
      case 'T':return {31,4,4,4,4,4,4};
      default:return {};
    }
  };
  for(int c=0;mark[c];++c) {
    const auto bits=glyph(mark[c]);
    for(int y=0;y<7;++y)for(int x=0;x<5;++x)if(bits[y]&(1u<<(4-x))) {
      const ImVec2 point(origin.x+pad+(c*6+x)*dot*1.5f,origin.y+pad+y*dot*1.5f);
      list.AddRectFilled(point,ImVec2(point.x+dot,point.y+dot),IM_COL32(226,226,226,235),dot*.2f);
    }
  }
  list.AddLine(ImVec2(origin.x+pad,origin.y+header_h),ImVec2(origin.x+panel_w-pad,origin.y+header_h),IM_COL32(255,255,255,24));
  float y = origin.y + header_h + pad*.6f;
  const float label_x = origin.x + pad, value_right = label_x + label_w + gap + value_w;
  for (const auto& row : rows) {
    const float value_x = value_right - r.font->CalcTextSizeA(px, FLT_MAX, 0.f, row.value).x;
    list.AddText(r.font, px, ImVec2(label_x + 1.f, y + 1.f), IM_COL32(0, 0, 0, 150), row.label);
    list.AddText(r.font, px, ImVec2(label_x, y), IM_COL32(183,183,183,255), row.label);
    list.AddText(r.font, px, ImVec2(value_x + 1.f, y + 1.f), IM_COL32(0, 0, 0, 150), row.value);
    list.AddText(r.font, px, ImVec2(value_x, y), IM_COL32(255, 255, 255, 255), row.value);
    if (row.extra[0])
      list.AddText(r.font, px * 0.82f, ImVec2(value_right + gap * 0.5f, y + px * 0.14f),
                   IM_COL32(175,175,175,255), row.extra);
    y += line;
  }
  if (graph) {
    if (!rows.empty()) y += pad * 0.5f;
    const float x0 = label_x, x1 = origin.x + panel_w - pad, h = graph_h;
    list.AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + h), IM_COL32(255, 255, 255, 18), std::round(px * 0.2f));
    // Scale to the slowest recent frame, at least 50 ms (20 fps) tall.
    double peak = 0.0;
    for (std::size_t i = 0; i < r.interval_count; ++i) peak = std::max(peak, r.intervals[i]);
    const double scale = std::max(peak * 1.15, 50.0);
    const std::size_t count = r.interval_count;
    std::vector<ImVec2> points;
    points.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t index = (r.interval_next + r.intervals.size() - count + i) % r.intervals.size();
      const float fx = x0 + (x1 - x0) * float(i) / float(std::max<std::size_t>(count - 1u, 1u));
      const float fy = y + h - float(std::min(r.intervals[index] / scale, 1.0)) * (h - 2.f) - 1.f;
      points.emplace_back(fx, fy);
    }
    list.AddPolyline(points.data(), int(points.size()), IM_COL32(225,225,225,235), 0,
                     std::max(1.f, std::round(px * 0.08f)));
  }
  list.PopTextureID();
  list.PopClipRect();
  if (list.VtxBuffer.empty() || list.IdxBuffer.empty()) return;

  using aurora::webgpu::g_device;
  using aurora::webgpu::g_queue;
  const std::uint64_t vertex_bytes = std::uint64_t(list.VtxBuffer.size()) * sizeof(ImDrawVert);
  const std::uint64_t index_bytes = (std::uint64_t(list.IdxBuffer.size()) * sizeof(ImDrawIdx) + 3u) & ~3ull;
  EnsureBuffer(r.vertices, r.vertex_capacity, vertex_bytes, wgpu::BufferUsage::Vertex, "GekkoAOT HUD vertices");
  EnsureBuffer(r.indices, r.index_capacity, index_bytes, wgpu::BufferUsage::Index, "GekkoAOT HUD indices");
  g_queue.WriteBuffer(r.vertices, 0, list.VtxBuffer.Data, vertex_bytes);
  if (list.IdxBuffer.size() % 2 != 0 && sizeof(ImDrawIdx) == 2) list.IdxBuffer.push_back(0);
  g_queue.WriteBuffer(r.indices, 0, list.IdxBuffer.Data, index_bytes);
  const std::array<float, 4> scale{2.f / float(width), 2.f / float(height), 0.f, 0.f};
  g_queue.WriteBuffer(r.uniforms, 0, scale.data(), sizeof(scale));
  if (!r.bind_group) {
    const std::array entries{
        wgpu::BindGroupEntry{.binding = 0, .buffer = r.uniforms, .size = 16},
        wgpu::BindGroupEntry{.binding = 1, .sampler = r.sampler},
        wgpu::BindGroupEntry{.binding = 2, .textureView = r.view},
    };
    const wgpu::BindGroupDescriptor descriptor{.layout = r.layout, .entryCount = entries.size(),
                                               .entries = entries.data()};
    r.bind_group = g_device.CreateBindGroup(&descriptor);
  }
  const wgpu::RenderPassColorAttachment attachment{.view = target, .loadOp = wgpu::LoadOp::Load,
                                                   .storeOp = wgpu::StoreOp::Store};
  const wgpu::RenderPassDescriptor pass_descriptor{.label = "GekkoAOT HUD", .colorAttachmentCount = 1,
                                                   .colorAttachments = &attachment};
  const auto pass = encoder.BeginRenderPass(&pass_descriptor);
  pass.SetPipeline(r.pipeline);
  pass.SetBindGroup(0, r.bind_group);
  pass.SetVertexBuffer(0, r.vertices, 0, vertex_bytes);
  pass.SetIndexBuffer(r.indices, sizeof(ImDrawIdx) == 2 ? wgpu::IndexFormat::Uint16 : wgpu::IndexFormat::Uint32, 0,
                      index_bytes);
  pass.SetViewport(0.f, 0.f, float(width), float(height), 0.f, 1.f);
  for (const auto& command : list.CmdBuffer) {
    if (command.ElemCount == 0u || command.UserCallback) continue;
    const float cx0 = std::clamp(command.ClipRect.x, 0.f, float(width));
    const float cy0 = std::clamp(command.ClipRect.y, 0.f, float(height));
    const float cx1 = std::clamp(command.ClipRect.z, cx0, float(width));
    const float cy1 = std::clamp(command.ClipRect.w, cy0, float(height));
    if (cx1 - cx0 < 1.f || cy1 - cy0 < 1.f) continue;
    pass.SetScissorRect(std::uint32_t(cx0), std::uint32_t(cy0), std::uint32_t(cx1 - cx0), std::uint32_t(cy1 - cy0));
    pass.DrawIndexed(command.ElemCount, 1, command.IdxOffset, std::int32_t(command.VtxOffset));
  }
  pass.End();
}
} // namespace GekkoAOT::GX::Hud
