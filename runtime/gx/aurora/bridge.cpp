// SPDX-License-Identifier: GPL-3.0-or-later
// GekkoAOT direct retail GX -> Aurora bridge. No emulator runtime.
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/gx/GXCommandList.h>
#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXAurora.h>
#include "gx/command_processor.hpp"
#include "gx/gx.hpp"
#include "gx/texture.hpp"
#include "gfx/render_worker.hpp"
#include "gfx/color_peek.hpp"
#include "gfx/depth_peek.hpp"
#include "gfx/frame.hpp"
#include "gfx/texture.hpp"
#include "webgpu/gpu.hpp"
#include <SDL3/SDL.h>
#include "window.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#ifndef GEKKOAOT_VERSION
#define GEKKOAOT_VERSION "dev"
#endif
#include <string>
#include <vector>
#include <utility>

#ifdef GEKKOAOT_HAVE_X11
#include <X11/Xlib.h>
#endif

#if defined(_WIN32)
#define GEKKOAOT_GX_EXPORT extern "C" __declspec(dllexport)
#else
#define GEKKOAOT_GX_EXPORT extern "C" __attribute__((visibility("default")))
#endif

using HostResolveFn = bool (*)(void* user, std::uint32_t address, std::uint32_t size,
                               std::uint32_t, std::uint32_t,
                               const void** data, std::uint32_t* available);
using HostPeEventFn = void (*)(void* user, std::uint32_t reg, std::uint32_t value);

// Pinned Aurora hooks installed by runtime/gx/aurora/CMakeLists.txt.
// They are private to the GekkoAOT NativeGX DSO.
extern "C" void aurora_end_frame_no_present();
extern "C" void aurora_present_texture(const void* texture);

namespace {
HostResolveFn g_resolve = nullptr;
void* g_resolve_user = nullptr;
HostPeEventFn g_pe = nullptr;
void* g_pe_user = nullptr;
bool g_ready = false;
bool g_quit = false;
bool g_frame_open = false;
// v128b renderer-level aspect mode. Keep only the two user-facing policies:
// exact 4:3, or a live window-aspect mode with projection compensation.
std::atomic<std::uint32_t> g_aspect_mode{0u};
bool g_alt_enter_latched = false;
AuroraInfo g_info{};
std::vector<std::uint8_t> g_fifo;
std::size_t g_fifo_read = 0;
std::size_t g_fifo_need = 1;
std::array<std::uint32_t, 8> g_vertex_size_cache{};
std::uint8_t g_vertex_size_valid = 0;

struct IndexScanField {
  std::uint8_t attr = 0;
  std::uint8_t index_width = 0;
  std::uint8_t index_count = 0;
  std::uint8_t reserved = 0;
  std::uint16_t offset = 0;
  std::uint16_t element_size = 0;
};
struct IndexScanLayout {
  std::uint16_t stride = 0;
  std::uint8_t field_count = 0;
  std::uint8_t valid = 0;
  std::array<IndexScanField, 12> fields{};
};
std::array<IndexScanLayout, 8> g_index_scan_layout{};
std::uint8_t g_index_scan_valid = 0;
std::array<std::uint32_t, 8> g_texture_image3_word{};
std::array<std::uint32_t, GX_VA_MAX_ATTR> g_array_available{};
// v141: retain the authoritative guest base separately from Aurora's host
// pointer/visible byte window. CPU D-cache maintenance publishes dynamic
// vertex data to GX; use that boundary to invalidate only overlapping GPU
// snapshots instead of hashing vertex arrays on every draw.
std::array<std::uint32_t, GX_VA_MAX_ATTR> g_array_guest_base{};
std::uint64_t g_array_cache_control_events = 0;
std::uint64_t g_array_cache_invalidations = 0;
unsigned g_array_cache_control_logs = 0;
std::uint8_t g_texture_image3_valid = 0;
std::uint64_t g_texture_generation = 1;
std::uint64_t g_display_copy_count = 0;
// Opt-in binary trace of retail draw commands. Records have four u64 fields:
// kind (1=draw, 2=GXCopyDisp), frame, command hash/monotonic ns, opcode/length.
// This lets a captured glitch be aligned with the command stream without
// logging per-draw text or changing the default hot path.
std::FILE* DrawCaptureFile() {
  static std::FILE* output = [] {
    const char* path = std::getenv("GEKKOAOT_GX_DIAG_DRAW_CAPTURE");
    if (!path || !*path) return static_cast<std::FILE*>(nullptr);
    auto* file = std::fopen(path, "wb");
    if (file) std::setvbuf(file, nullptr, _IOFBF, 1u << 20u);
    return file;
  }();
  return output;
}

void CaptureDraw(std::uint8_t opcode, const std::uint8_t* bytes, std::size_t length) {
  auto* file = DrawCaptureFile();
  if (!file) return;
  std::uint64_t hash = 14695981039346656037ull;
  for (std::size_t i = 0; i < length; ++i)
    hash = (hash ^ bytes[i]) * 1099511628211ull;
  const std::uint64_t record[4] = {1u, g_display_copy_count, hash,
                                   (static_cast<std::uint64_t>(opcode) << 32u) | length};
  (void)std::fwrite(record, sizeof(record), 1u, file);
  if (hash == 0xff3edebc607b9e0bull || hash == 0x6e7ebe27102771dfull) {
    static bool seen_missing = false, seen_present = false;
    bool& seen = hash == 0xff3edebc607b9e0bull ? seen_missing : seen_present;
    if (!seen) {
      seen = true;
      std::fprintf(stderr, "GEKKOAOT_GX_DIAG_TARGET_QUAD frame=%llu hash=%016llx bytes=",
                   static_cast<unsigned long long>(g_display_copy_count),
                   static_cast<unsigned long long>(hash));
      for (std::size_t i = 0; i < length; ++i) std::fprintf(stderr, "%02x", bytes[i]);
      std::fprintf(stderr, "\n");
    }
  }
}

void CaptureFrameBoundary() {
  auto* file = DrawCaptureFile();
  if (!file) return;
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
  const std::uint64_t record[4] = {2u, g_display_copy_count,
                                   static_cast<std::uint64_t>(ns), 0u};
  (void)std::fwrite(record, sizeof(record), 1u, file);
  (void)std::fflush(file);
}
// Host-presentation latch. GXCopyDisp still performs the exact same zero-copy
// XFB rotation; this only exposes that completed frame as an optional host
// presentation boundary. Latest frame wins if several copies occur in one AOT
// slice, avoiding a stale-present burst.
bool g_frame_ready_pending = false;
std::uint32_t g_frame_ready_xfb = 0;
// v48 universal presentation interpolation. These are dedicated single-sample
// GPU snapshots, so the previous frame remains immutable even when a game
// reuses the same XFB address every frame. One GPU copy is paid per guest frame;
// intermediate host presents never touch guest memory or advance VI/DSP/AI.
bool g_frame_interpolation_enabled = true;
bool g_interp_previous_valid = false;
bool g_interp_current_valid = false;
aurora::webgpu::TextureWithSampler g_interp_previous;
aurora::webgpu::TextureWithSampler g_interp_current;
std::uint64_t g_interp_snapshot_count = 0;

// Native zero-copy XFB cache. Each slot owns the Aurora EFB render targets
// that were active when GXCopyDisp targeted the guest physical XFB address.
struct XfbZeroCopySlot {
  std::uint32_t address = 0;
  std::uint32_t stride_bytes = 0;
  std::uint32_t native_width = 0;
  std::uint32_t native_height = 0;
  std::uint64_t span_bytes = 0;
  std::uint64_t generation = 0;
  bool valid = false;
  // Own the resolved XFB independently from the live EFB.  This is the key
  // difference from the old render-target rotation path: later EFB clears or
  // unrelated display copies cannot mutate the VI-visible image.
  aurora::gfx::TextureHandle snapshot;
  aurora::webgpu::TextureWithSampler frame_buffer;
  aurora::webgpu::TextureWithSampler resolved;
};
constexpr std::size_t kXfbZeroCopySlots = 8u;
std::array<XfbZeroCopySlot, kXfbZeroCopySlots> g_xfb_slots{};
std::uint64_t g_xfb_generation = 0;

// v57: some retail titles (notably FMV/video paths) write YUVY XFBs directly
// into MEM1 and point VI at them without ever issuing GXCopyDisp. Keep the
// existing zero-copy cache as the fast path; this texture is only the RAM-XFB
// compatibility path used after an address-keyed cache miss.
aurora::gfx::TextureHandle g_ram_xfb_texture;
aurora::webgpu::TextureWithSampler g_ram_xfb_present;
std::vector<std::uint8_t> g_ram_xfb_rgba;
std::uint32_t g_ram_xfb_width = 0u;
std::uint32_t g_ram_xfb_height = 0u;
std::uint64_t g_ram_xfb_generation = 0u;
struct RamXfbCandidate {
  std::uint32_t top = 0u;
  std::uint32_t bottom = 0u;
  std::uint32_t width = 0u;
  std::uint32_t stride = 0u;
  std::uint32_t field_height = 0u;
  std::uint64_t last_hash = 0u;
  std::uint64_t changes = 0u;
  std::uint64_t last_seen = 0u;
  std::uint32_t observations = 0u;
  bool armed = false;
  bool valid = false;
};
constexpr std::size_t kRamXfbCandidateSlots = 16u;
std::array<RamXfbCandidate, kRamXfbCandidateSlots> g_ram_xfb_candidates{};
std::uint64_t g_ram_xfb_candidate_epoch = 0u;
std::uint32_t g_copy_dest_physical = 0;
std::uint32_t g_copy_stride_bytes = 0;
std::uint32_t g_copy_yscale_raw = 0x100u;
bool g_copy_src_valid = false;
bool g_copy_dest_valid = false;
bool g_fifo_fault = false;
bool g_fifo_fault_reported = false;
bool g_texture_bind_cache = true;
bool g_array_frame_refresh = false;
bool g_array_base_trace = false;
bool g_z16r_copy_trace = false;
bool g_ztexture_trace = false;
unsigned g_ztexture_logs = 0;
std::uint32_t g_z16r_copy_dest = 0;
std::uint32_t g_z16r_copy_span = 0;
std::uint8_t g_z16r_bound_maps = 0;
unsigned g_z16r_bp_logs = 0;
unsigned g_z16r_copy_logs = 0;
unsigned g_z16r_bind_logs = 0;
unsigned g_z16r_draw_logs = 0;
std::uint64_t g_array_base_writes = 0;
std::uint64_t g_array_base_same_binding = 0;
std::uint64_t g_array_base_same_cached = 0;
std::uint64_t g_array_base_same_cached_bytes = 0;
std::uint64_t g_array_base_trace_draws = 0;
std::uint64_t g_array_base_trace_uploads = 0;
std::uint64_t g_array_base_trace_upload_bytes = 0;
std::uint64_t g_array_base_trace_invalidate = 0;
std::uint64_t g_array_base_trace_window_growth = 0;
std::uint64_t g_array_base_trace_window_cached = 0;
std::uint64_t g_array_base_trace_window_bytes = 0;
std::uint64_t g_array_base_trace_eager_growth = 0;
// v114 correctness barrier for guest-owned indexed vertex arrays. Aurora's
// cachedRange lives in host GPU staging and cannot observe CPU writes to MEM1.
// Refreshing only at GXCopyDisp is too late for engines that recycle a scratch
// vertex/normal/texcoord buffer several times inside one frame. Keep the narrow
// per-draw barrier enabled by default; the broad frame refresh remains off.
bool g_indexed_draw_coherency = false;

struct FifoWriteTrace {
  std::uint64_t value = 0;
  std::uint64_t stream_offset = 0;
  std::uint32_t guest_pc = 0;
  std::uint8_t size = 0;
};
struct FifoCommandTrace {
  std::uint64_t stream_offset = 0;
  std::size_t len = 0;
  std::uint8_t cmd = 0;
  std::uint8_t fmt = 0xffu;
  std::uint16_t count = 0;
  std::uint32_t stride = 0;
};
std::array<FifoWriteTrace, 32> g_fifo_write_trace{};
std::array<FifoCommandTrace, 32> g_fifo_command_trace{};
std::size_t g_fifo_command_trace_head = 0;
std::size_t g_fifo_command_trace_count = 0;
std::uint64_t g_fifo_stream_base = 0;
std::size_t g_fifo_write_trace_head = 0;
std::size_t g_fifo_write_trace_count = 0;
std::uint8_t g_fifo_last_cmd = 0;
std::size_t g_fifo_last_cmd_len = 0;
std::uint8_t g_fifo_last_draw_fmt = 0xffu;
std::uint16_t g_fifo_last_draw_count = 0;
std::uint32_t g_fifo_last_draw_stride = 0;

// GX_LOAD_BP_REG is 0x61 while GX opcode dispatch masks the low VAT bits
// with GX_OPCODE_MASK (0xF8), yielding 0x60. Always compare masked
// opcodes against the masked BP opcode or a retail BP command is mistaken
// for a one-byte command and Aurora receives a truncated FIFO packet.
constexpr std::uint8_t kLoadBpOpcode = GX_LOAD_BP_REG & GX_OPCODE_MASK;

bool EnvBool(const char* name, bool fallback) {
  const char* v=std::getenv(name); if(!v||!*v) return fallback;
  return std::strcmp(v,"0")!=0 && std::strcmp(v,"false")!=0 && std::strcmp(v,"off")!=0;
}
unsigned EnvUnsigned(const char* name, unsigned fallback) {
  const char* v=std::getenv(name); if(!v||!*v) return fallback;
  char* end=nullptr; const unsigned long x=std::strtoul(v,&end,0);
  return end!=v && *end=='\0' && x<=std::numeric_limits<unsigned>::max() ? static_cast<unsigned>(x) : fallback;
}
bool EqualNoCase(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    if (std::tolower(static_cast<unsigned char>(*a)) !=
        std::tolower(static_cast<unsigned char>(*b))) return false;
    ++a; ++b;
  }
  return *a == '\0' && *b == '\0';
}

std::uint32_t AspectModeFromText(const char* text) {
  // Backward compatibility: every old fixed-wide value now means the single
  // live "Stretched to Window" mode. 4:3/original stays exact 4:3.
  if (text && (EqualNoCase(text, "stretch") || EqualNoCase(text, "16:9") ||
               EqualNoCase(text, "16:10") || EqualNoCase(text, "21:9") ||
               EqualNoCase(text, "32:9")))
    return 1u;
  return 0u;
}

float HostWindowAspect() noexcept {
  const auto size = aurora::window::get_window_size();
  const std::uint32_t width = size.native_fb_width != 0u ? size.native_fb_width : size.fb_width;
  const std::uint32_t height = size.native_fb_height != 0u ? size.native_fb_height : size.fb_height;
  if (width == 0u || height == 0u) return 4.0f / 3.0f;
  return static_cast<float>(width) / static_cast<float>(height);
}

float TargetAspectForMode(std::uint32_t mode) noexcept {
  return mode == 1u ? HostWindowAspect() : 4.0f / 3.0f;
}

float ProjectionXScaleForMode(std::uint32_t mode) noexcept {
  if (mode != 1u) return 1.0f;
  const float target = HostWindowAspect();
  return target > 0.0f ? (4.0f / 3.0f) / target : 1.0f;
}

aurora::webgpu::Viewport PresentViewportForAspect(std::uint32_t surface_width,
                                                   std::uint32_t surface_height,
                                                   std::uint32_t content_width,
                                                   std::uint32_t content_height) noexcept {
  // v137: the direct VI scanout already uses the live GekkoAOT aspect, but the
  // interpolation path accidentally fell back to Aurora's content-aspect
  // viewport.  That reintroduced a ~4:3 viewport whenever interpolation was
  // enabled.  Derive the target from the actual swapchain dimensions in
  // stretch mode so fullscreen/resizes immediately fill the surface; retain an
  // exact 4:3 viewport in original mode.
  if (surface_width == 0u || surface_height == 0u) return {};
  const auto mode = g_aspect_mode.load(std::memory_order_acquire);
  const float target = mode == 1u
                           ? static_cast<float>(surface_width) / static_cast<float>(surface_height)
                           : 4.0f / 3.0f;
  if (!(target > 0.0f))
    return aurora::webgpu::calculate_present_viewport(surface_width, surface_height,
                                                       content_width, content_height);

  std::uint32_t viewport_width = surface_width;
  std::uint32_t viewport_height = static_cast<std::uint32_t>(
      static_cast<float>(viewport_width) / target + 0.5f);
  if (viewport_height > surface_height) {
    viewport_height = surface_height;
    viewport_width = static_cast<std::uint32_t>(
        static_cast<float>(viewport_height) * target + 0.5f);
  }
  viewport_width = std::clamp<std::uint32_t>(viewport_width, 1u, surface_width);
  viewport_height = std::clamp<std::uint32_t>(viewport_height, 1u, surface_height);
  return {
      .left = static_cast<float>((surface_width - viewport_width) / 2u),
      .top = static_cast<float>((surface_height - viewport_height) / 2u),
      .width = static_cast<float>(viewport_width),
      .height = static_cast<float>(viewport_height),
      .znear = 0.0f,
      .zfar = 1.0f,
  };
}

bool FrameInterpolationForAspect(std::uint32_t mode) {
  const bool requested = EnvBool("GEKKOAOT_FRAME_INTERPOLATION", true);
  if (!requested) return false;

  // v158: the persistent MKDD double-image/seam survived with interpolation
  // completely disabled, so it was not caused by the temporal blend. Restore
  // interpolation in widescreen by default. It can still be disabled without
  // touching guest timing with GEKKOAOT_WIDESCREEN_INTERPOLATION=0.
  if (mode == 1u && !EnvBool("GEKKOAOT_WIDESCREEN_INTERPOLATION", true))
    return false;
  return true;
}

void ApplyAspectMode(std::uint32_t mode, bool log_change) {
  mode = mode == 1u ? 1u : 0u;
  const std::uint32_t previous = g_aspect_mode.exchange(mode, std::memory_order_acq_rel);
  const bool stretch_to_window = mode == 1u;

  // v156c: keep producer-side GX/EFB geometry in the GameCube coordinate
  // space. MKDD switches viewport/scissor for HUD, split-screen, shadows and
  // post effects; stretching Aurora's producer viewport exposes the original
  // 4:3 edge as a large black seam. Hor+ stays in the projection, while the
  // already-existing PresentViewportForAspect() widens only final scanout.
  aurora::gx::set_viewport_policy(AURORA_VIEWPORT_FIT);
  aurora::gx::update();

  // The live host aspect still changes ProjectionXScaleForMode(), so refresh
  // uniforms on resize even though the producer viewport itself stays FIT.
  if (previous != mode || stretch_to_window)
    aurora::gx::g_gxState.dirty |= aurora::gx::DirtyUniform;

  const bool interpolation_before = g_frame_interpolation_enabled;
  g_frame_interpolation_enabled = FrameInterpolationForAspect(mode);
  if (previous != mode) {
    // Never blend snapshots produced under two different aspect policies.
    g_interp_previous = {};
    g_interp_current = {};
    g_interp_previous_valid = false;
    g_interp_current_valid = false;
    g_interp_snapshot_count = 0;
  }

  if (log_change && (previous != mode || !g_ready)) {
    std::fprintf(stderr,
                 "GEKKOAOT_WIDESCREEN_V156C=1 mode=%s target=%.6f xscale=%.6f "
                 "producer=gc-fit scanout=window-wide interpolation=%u previous-interpolation=%u\n",
                 stretch_to_window ? "stretch" : "4:3", TargetAspectForMode(mode),
                 ProjectionXScaleForMode(mode),
                 g_frame_interpolation_enabled ? 1u : 0u,
                 interpolation_before ? 1u : 0u);
  }
}
AuroraBackend BackendFromEnv() {
  const char* v=std::getenv("GEKKOAOT_GRAPHICS_BACKEND");
  if(!v) return BACKEND_AUTO;
  if(EqualNoCase(v,"vulkan")) return BACKEND_VULKAN;
  if(EqualNoCase(v,"opengl") || EqualNoCase(v,"ogl")) return BACKEND_OPENGL;
#ifdef _WIN32
  if(EqualNoCase(v,"d3d12")) return BACKEND_D3D12;
  if(EqualNoCase(v,"d3d11")) return BACKEND_D3D11;
#endif
  return BACKEND_AUTO;
}
std::uint32_t Be32(const std::uint8_t* p) {
  return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];
}
std::uint16_t Be16(const std::uint8_t* p) { return std::uint16_t((p[0]<<8)|p[1]); }

bool Resolve(std::uint32_t address, std::uint32_t size, const void** data, std::uint32_t* available) {
  return g_resolve && g_resolve(g_resolve_user,address,size,0,0,data,available);
}

std::uint32_t Bits(std::uint32_t value, unsigned offset, unsigned count) {
  return (value >> offset) & ((1u << count) - 1u);
}

std::uint32_t ComponentSize(std::uint32_t format) {
  return format < 2u ? 1u : format < 4u ? 2u : 4u;
}

std::uint32_t AttrSize(std::uint32_t descriptor, std::uint32_t direct_size) {
  switch(descriptor) {
  case 0u: return 0u;
  case 1u: return direct_size;
  case 2u: return 1u;
  case 3u: return 2u;
  default: return 0u;
  }
}

std::uint32_t ColorSize(std::uint32_t format) {
  static constexpr std::array<std::uint32_t,8> sizes{2u,3u,4u,2u,3u,4u,0u,0u};
  return sizes[format & 7u];
}

void TexFormat(std::uint32_t g0, std::uint32_t g1, std::uint32_t g2,
               unsigned index, std::uint32_t* format, std::uint32_t* elements) {
  std::uint32_t f=0u,e=0u;
  switch(index) {
  case 0u: f=Bits(g0,22u,3u); e=Bits(g0,21u,1u); break;
  case 1u: f=Bits(g1,1u,3u);  e=Bits(g1,0u,1u); break;
  case 2u: f=Bits(g1,10u,3u); e=Bits(g1,9u,1u); break;
  case 3u: f=Bits(g1,19u,3u); e=Bits(g1,18u,1u); break;
  case 4u: f=Bits(g1,28u,3u); e=Bits(g1,27u,1u); break;
  case 5u: f=Bits(g2,6u,3u);  e=Bits(g2,5u,1u); break;
  case 6u: f=Bits(g2,15u,3u); e=Bits(g2,14u,1u); break;
  case 7u: f=Bits(g2,24u,3u); e=Bits(g2,23u,1u); break;
  default: break;
  }
  *format=f; *elements=e;
}

// v42's boundary oracle derived draw size from the raw CP VCD/VAT image. Keep
// that rule here so the standalone FIFO framing does not depend on Aurora's
// derived/cached vertex layout being perfectly synchronized at every write.
std::uint32_t RawVertexSize(GXVtxFmt fmt) {
  using namespace aurora::gx;
  const std::uint32_t f=static_cast<std::uint32_t>(fmt)&7u;
  if(!g_gxState.cpRegValid.test(0x50u) || !g_gxState.cpRegValid.test(0x60u) ||
     !g_gxState.cpRegValid.test(0x70u+f) || !g_gxState.cpRegValid.test(0x80u+f) ||
     !g_gxState.cpRegValid.test(0x90u+f)) return 0u;

  const std::uint32_t lo=g_gxState.cpRegCache[0x50u];
  const std::uint32_t hi=g_gxState.cpRegCache[0x60u];
  const std::uint32_t g0=g_gxState.cpRegCache[0x70u+f];
  const std::uint32_t g1=g_gxState.cpRegCache[0x80u+f];
  const std::uint32_t g2=g_gxState.cpRegCache[0x90u+f];
  std::uint32_t size=0u;

  for(std::uint32_t bits=lo&0x1ffu; bits!=0u; bits>>=1u) size+=bits&1u;

  const std::uint32_t pos=Bits(lo,9u,2u);
  size+=AttrSize(pos,ComponentSize(Bits(g0,1u,3u))*(Bits(g0,0u,1u)+2u));

  const std::uint32_t nrm=Bits(lo,11u,2u);
  const bool nbt=Bits(g0,9u,1u)!=0u;
  const bool idx3=Bits(g0,31u,1u)!=0u;
  if(nrm==1u) size+=ComponentSize(Bits(g0,10u,3u))*(nbt?9u:3u);
  else if(nrm==2u || nrm==3u) size+=(nrm-1u)*(nbt&&idx3?3u:1u);

  for(unsigned color=0;color<2u;++color) {
    const std::uint32_t desc=Bits(lo,13u+color*2u,2u);
    const std::uint32_t format=Bits(g0,14u+color*4u,3u);
    size+=AttrSize(desc,ColorSize(format));
  }

  for(unsigned tex=0;tex<8u;++tex) {
    const std::uint32_t desc=Bits(hi,tex*2u,2u);
    std::uint32_t format=0u,elements=0u;
    TexFormat(g0,g1,g2,tex,&format,&elements);
    size+=AttrSize(desc,ComponentSize(format)*(elements+1u));
  }
  return size;
}

std::uint32_t VertexSize(GXVtxFmt fmt) {
  using namespace aurora::gx;
  const unsigned index=static_cast<unsigned>(fmt)&7u;
  const std::uint8_t bit=static_cast<std::uint8_t>(1u<<index);
  if((g_vertex_size_valid&bit)!=0u) return g_vertex_size_cache[index];

  std::uint32_t typed=0;
  const auto& vf=g_gxState.vtxFmts[fmt];
  for(int i=GX_VA_PNMTXIDX;i<=GX_VA_TEX7;++i) {
    const auto& af=vf.attrs[i];
    switch(g_gxState.vtxDesc[i]) {
      case GX_NONE: break;
      case GX_DIRECT: typed += comp_type_size(static_cast<GXAttr>(i),af.type)*comp_cnt_count(static_cast<GXAttr>(i),af.cnt); break;
      case GX_INDEX8: typed += (i==GX_VA_NRM && af.cnt==GX_NRM_NBT3)?3u:1u; break;
      case GX_INDEX16: typed += (i==GX_VA_NRM && af.cnt==GX_NRM_NBT3)?6u:2u; break;
    }
  }
  const std::uint32_t raw=RawVertexSize(fmt);
  if(raw!=0u && raw!=typed) {
    static unsigned logs=0;
    if(logs++<16u)
      std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_VTX_BOUNDARY_V43 fmt=%u typed=%u raw=%u action=raw-cp\n",
                   index,typed,raw);
  }
  const std::uint32_t size=raw!=0u ? raw : typed;
  if(size!=0u) {
    g_vertex_size_cache[index]=size;
    g_vertex_size_valid=static_cast<std::uint8_t>(g_vertex_size_valid|bit);
  }
  return size;
}

void InvalidateVertexSize(std::uint8_t reg) {
  if(reg==0x50u || reg==0x60u) {
    g_vertex_size_valid=0;
    g_index_scan_valid=0;
    return;
  }
  if((reg>=0x70u&&reg<=0x77u)||(reg>=0x80u&&reg<=0x87u)||(reg>=0x90u&&reg<=0x97u)) {
    const auto bit=static_cast<std::uint8_t>(1u<<(reg&7u));
    g_vertex_size_valid=static_cast<std::uint8_t>(g_vertex_size_valid&~bit);
    g_index_scan_valid=static_cast<std::uint8_t>(g_index_scan_valid&~bit);
  }
}

void MapArrayBase(std::uint8_t reg, std::uint32_t value) {
  using namespace aurora::gx;
  const std::uint32_t attr=std::uint32_t(reg-0xA0u)+GX_VA_POS;
  if(attr>=GX_VA_MAX_ATTR) return;
  g_array_guest_base[attr]=value;
  auto& array=g_gxState.arrays[attr];
  const void* ptr=nullptr; std::uint32_t available=0;
  const bool mapped=Resolve(value,1,&ptr,&available);
  if(g_array_base_trace) {
    ++g_array_base_writes;
    const bool same=mapped && g_gxState.cpRegValid.test(reg) &&
                    g_gxState.cpRegCache[reg]==value && array.data==ptr &&
                    g_array_available[attr]==available;
    if(same) {
      ++g_array_base_same_binding;
      if(array.cachedRange.size!=0u) {
        ++g_array_base_same_cached;
        g_array_base_same_cached_bytes+=array.cachedRange.size;
      }
    }
    if((g_array_base_writes&0x3fffu)==0u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_ARRAY_BASE_TRACE_V1 writes=%llu same_binding=%llu same_cached=%llu discarded_cached_bytes=%llu\n",
                   static_cast<unsigned long long>(g_array_base_writes),
                   static_cast<unsigned long long>(g_array_base_same_binding),
                   static_cast<unsigned long long>(g_array_base_same_cached),
                   static_cast<unsigned long long>(g_array_base_same_cached_bytes));
  }

  // The CP register write is authoritative even when the pointed address is
  // not currently mappable. Never retain the previous host pointer: doing so
  // makes a failed base update render stale vertices/matrices from an older
  // frame, which looks exactly like random GPU/VRAM corruption. Actual use of
  // an unmapped array will fail closed in the per-command window checks.
  if(!mapped) {
    array.data=nullptr; array.size=0; array.cachedRange={};
    g_array_available[attr]=0u;
    g_gxState.cpRegValid.set(reg); g_gxState.cpRegCache[reg]=value;
    g_gxState.dirty |= DirtyImmediates;
    static unsigned invalid_logs=0u;
    if(invalid_logs++<64u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_ARRAY_BASE_V86=0 reg=0x%02x attr=%u guest=%08x action=invalidate-stale-mapping\n",
                   unsigned(reg),attr,value);
    return;
  }

  // GX only exposes an array base and stride; it does not expose a byte size.
  // Do NOT hand Aurora the remainder of MEM1 here. Aurora uploads array.size to
  // its per-frame storage staging buffer, so using `available` (often tens of
  // MiB) overflows Aurora's fixed 8 MiB storage buffer as soon as indexed 3D
  // geometry is drawn. Keep the host mapping limit separately and grow the
  // visible Aurora window from the indices actually referenced by each draw.
  array.data=ptr; array.size=0; array.le=false; array.cachedRange={};
  g_array_available[attr]=available;
  g_gxState.cpRegValid.set(reg); g_gxState.cpRegCache[reg]=value;
  g_gxState.dirty |= DirtyImmediates;
}

bool BuildIndexScanLayout(GXVtxFmt fmt, IndexScanLayout* out) {
  using namespace aurora::gx;
  if(!out) return false;
  IndexScanLayout layout{};
  const auto& vf=g_gxState.vtxFmts[fmt];
  std::uint32_t offset=0;

  for(int i=GX_VA_PNMTXIDX;i<=GX_VA_TEX7;++i) {
    const auto desc=g_gxState.vtxDesc[i];
    if(desc==GX_NONE) continue;
    const auto attr=static_cast<GXAttr>(i);
    const auto& af=vf.attrs[i];

    if(desc==GX_DIRECT) {
      offset+=std::uint32_t(comp_type_size(attr,af.type))*comp_cnt_count(attr,af.cnt);
      continue;
    }
    if(desc!=GX_INDEX8 && desc!=GX_INDEX16) return false;

    const std::uint32_t width=desc==GX_INDEX8 ? 1u : 2u;
    const std::uint32_t count=(i==GX_VA_NRM && af.cnt==GX_NRM_NBT3) ? 3u : 1u;
    if(i>=GX_VA_POS) {
      if(layout.field_count>=layout.fields.size() || offset>std::numeric_limits<std::uint16_t>::max())
        return false;
      // NBT3 carries three independent normal indices. Each index addresses one
      // XYZ vector, not a full N/B/T tuple, so the source element is 3
      // components even though a DIRECT NBT vertex contains 9 components.
      const std::uint32_t components=(i==GX_VA_NRM && af.cnt==GX_NRM_NBT3)
                                         ? 3u : comp_cnt_count(attr,af.cnt);
      const std::uint32_t element_size=std::uint32_t(comp_type_size(attr,af.type))*components;
      if(element_size>std::numeric_limits<std::uint16_t>::max()) return false;
      layout.fields[layout.field_count++]={
          static_cast<std::uint8_t>(i),static_cast<std::uint8_t>(width),
          static_cast<std::uint8_t>(count),0,
          static_cast<std::uint16_t>(offset),static_cast<std::uint16_t>(element_size)};
    }
    offset+=width*count;
  }

  const std::uint32_t expected=VertexSize(fmt);
  if(offset==0u || offset!=expected || offset>std::numeric_limits<std::uint16_t>::max()) {
    static unsigned logs=0;
    if(logs++<16u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_INDEX_SCAN_V2=0 fmt=%u scan_stride=%u fifo_stride=%u action=reject-layout\n",
                   unsigned(fmt)&7u,offset,expected);
    return false;
  }
  layout.stride=static_cast<std::uint16_t>(offset);
  layout.valid=1;
  *out=layout;
  return true;
}

const IndexScanLayout* GetIndexScanLayout(GXVtxFmt fmt) {
  const unsigned index=static_cast<unsigned>(fmt)&7u;
  const auto bit=static_cast<std::uint8_t>(1u<<index);
  if((g_index_scan_valid&bit)!=0u) return &g_index_scan_layout[index];
  IndexScanLayout layout{};
  if(!BuildIndexScanLayout(fmt,&layout)) return nullptr;
  g_index_scan_layout[index]=layout;
  g_index_scan_valid=static_cast<std::uint8_t>(g_index_scan_valid|bit);
  return &g_index_scan_layout[index];
}

bool PrepareIndexedArrayWindows(const std::uint8_t* p, std::size_t len) {
  using namespace aurora::gx;
  if(!p || len<3u) return false;

  const auto fmt=static_cast<GXVtxFmt>(p[0]&GX_VAT_MASK);
  const std::uint32_t vtx_count=Be16(p+1);
  const auto* layout=GetIndexScanLayout(fmt);
  if(!layout) return false;
  const std::size_t vertex_bytes=std::size_t(vtx_count)*layout->stride;
  if(vertex_bytes!=len-3u) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_INDEX_SCAN_V2=0 reason=draw-size-mismatch stride=%u count=%u bytes=%zu expected=%zu\n",
                 unsigned(layout->stride),vtx_count,vertex_bytes,len-3u);
    return false;
  }
  if(layout->field_count==0u) return true;

  // v109: an indexed draw is a guest-memory visibility boundary. The guest CPU
  // can rewrite an array in MEM1 without issuing GXInvalidateVtxCache and there
  // is no host write notification for Aurora's cachedRange. If we carry that
  // cachedRange into another draw, Aurora can also merge the draw because GX
  // state itself did not change, making the second draw consume the first
  // draw's array snapshot. That produces exactly the transient exploded/off-
  // screen geometry seen when engines recycle scratch vertex buffers.
  //
  // Mark the draw dirty unconditionally when indexed arrays participate so
  // Aurora cannot merge it across this memory-visibility boundary. The actual
  // cached ranges are cleared after the exact per-draw windows are established
  // below, avoiding the old multi-MiB whole-MEM1 upload behaviour.
  if(g_indexed_draw_coherency) g_gxState.dirty |= DirtyImmediates;

  // INDEX8 can address at most 256 elements. For the common small SDK arrays
  // it is cheaper to expose that complete bounded window once than to rescan
  // every vertex of every draw just to rediscover the current maximum index.
  // Keep the eager window bounded so large/INDEX16 arrays still use exact
  // per-draw sizing and never recreate the old multi-MiB staging problem.
  constexpr std::uint64_t kEagerIndexWindowBytes=64u*1024u;
  std::array<bool,12> skip_scan{};
  for(std::uint32_t f=0;f<layout->field_count;++f) {
    const auto& field=layout->fields[f];
    if(field.index_width!=1u) continue;
    auto& array=g_gxState.arrays[field.attr];
    if(!array.data) return false;
    const std::uint64_t full=std::uint64_t(255u)*array.stride+field.element_size;
    const std::uint32_t available=g_array_available[field.attr];
    if(full==0u || full>available || full>kEagerIndexWindowBytes) continue;
    const auto full32=static_cast<std::uint32_t>(full);
    if(array.size<full32) {
      if(g_array_base_trace) ++g_array_base_trace_eager_growth;
      array.size=full32;
      array.cachedRange={};
      g_gxState.dirty |= DirtyImmediates;
    }
    skip_scan[f]=true;
  }

  std::array<std::uint32_t,GX_VA_MAX_ATTR> max_index{};
  std::array<bool,GX_VA_MAX_ATTR> seen{};
  const auto* const vertices=p+3;

  // Scan only the indexed fields. v1 walked every VCD attribute for every
  // vertex, which made ProcessOne a measurable hot spot. The cached layout
  // turns the inner loop into fixed-offset byte/halfword reads.
  for(std::uint32_t f=0;f<layout->field_count;++f) {
    if(skip_scan[f]) continue;
    const auto& field=layout->fields[f];
    const auto attr=field.attr;
    const auto* q=vertices+field.offset;
    std::uint32_t maximum=0;
    for(std::uint32_t v=0;v<vtx_count;++v,q+=layout->stride) {
      if(field.index_width==1u) {
        for(std::uint32_t n=0;n<field.index_count;++n)
          maximum=std::max(maximum,std::uint32_t(q[n]));
      } else {
        for(std::uint32_t n=0;n<field.index_count;++n)
          maximum=std::max(maximum,std::uint32_t(Be16(q+n*2u)));
      }
    }
    seen[attr]=true;
    max_index[attr]=maximum;
  }

  static unsigned logs=0;
  for(std::uint32_t f=0;f<layout->field_count;++f) {
    const auto& field=layout->fields[f];
    const int i=field.attr;
    if(!seen[i]) continue;
    auto& array=g_gxState.arrays[i];
    if(!array.data) return false;

    const std::uint64_t required=
        std::uint64_t(max_index[i])*array.stride+field.element_size;
    const std::uint32_t available=g_array_available[i];
    if(required>available || required>std::numeric_limits<std::uint32_t>::max()) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_ARRAY_WINDOW_V2=0 attr=%d max_index=%u stride=%u required=%llu available=%u action=reject\n",
                   i,max_index[i],unsigned(array.stride),
                   static_cast<unsigned long long>(required),available);
      return false;
    }

    const auto required32=static_cast<std::uint32_t>(required);
    if(required32>array.size) {
      if(g_array_base_trace) {
        ++g_array_base_trace_window_growth;
        if(array.cachedRange.size!=0u) {
          ++g_array_base_trace_window_cached;
          g_array_base_trace_window_bytes+=required32;
        }
      }
      array.size=required32;
      array.cachedRange={};
      g_gxState.dirty |= DirtyImmediates;
      if(logs++<32u) {
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_ARRAY_WINDOW_V2=1 attr=%d max_index=%u stride=%u bytes=%u available=%u\n",
                     i,max_index[i],unsigned(array.stride),required32,available);
      }
    }
  }

  if(g_indexed_draw_coherency) {
    std::array<bool,GX_VA_MAX_ATTR> refreshed{};
    bool any=false;
    for(std::uint32_t f=0;f<layout->field_count;++f) {
      const auto attr=layout->fields[f].attr;
      if(attr>=GX_VA_MAX_ATTR || refreshed[attr]) continue;
      refreshed[attr]=true;
      auto& array=g_gxState.arrays[attr];
      // Clear even when the visible window did not grow: guest RAM may have
      // changed in place since the previous draw. push_gx_draw() will upload
      // the current bytes into a fresh storage range for this draw.
      if(array.cachedRange.size!=0u) {
        array.cachedRange={};
        any=true;
      }
    }
    // DirtyImmediates was set above to block draw merging. Keep it asserted if
    // a snapshot was explicitly invalidated as documentation of the contract.
    if(any) g_gxState.dirty |= DirtyImmediates;
  }
  return true;
}

bool PrepareIndexedXfArrayWindow(const std::uint8_t* p, std::size_t len) {
  using namespace aurora::gx;
  if(!p || len!=5u) return false;

  const std::uint8_t op=p[0]&GX_OPCODE_MASK;
  if(op!=GX_LOAD_INDX_A && op!=GX_LOAD_INDX_B &&
     op!=GX_LOAD_INDX_C && op!=GX_LOAD_INDX_D)
    return false;

  const std::uint32_t array_type=
      GX_POS_MTX_ARRAY+(std::uint32_t(op)-GX_LOAD_INDX_A)/0x08u;
  if(array_type>=GX_VA_MAX_ATTR) return false;

  const std::uint32_t source_index=Be16(p+1);
  const std::uint16_t addr_len=Be16(p+3);
  const std::uint32_t words=(std::uint32_t(addr_len)>>12u)+1u;
  const std::uint32_t source_bytes=words*4u;
  auto& array=g_gxState.arrays[array_type];
  const std::uint32_t available=g_array_available[array_type];

  if(!array.data || available==0u) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_XF_ARRAY_WINDOW_V86=0 op=0x%02x attr=%u index=%u stride=%u words=%u reason=unmapped-array action=reject\n",
                 unsigned(op),array_type,source_index,unsigned(array.stride),words);
    return false;
  }

  const std::uint64_t source_offset=std::uint64_t(source_index)*array.stride;
  const std::uint64_t required=source_offset+source_bytes;
  if(required>available || required>std::numeric_limits<std::uint32_t>::max()) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_XF_ARRAY_WINDOW_V86=0 op=0x%02x attr=%u index=%u stride=%u words=%u required=%llu available=%u reason=outside-mapped-guest-memory action=reject\n",
                 unsigned(op),array_type,source_index,unsigned(array.stride),words,
                 static_cast<unsigned long long>(required),available);
    return false;
  }

  // Aurora validates LOAD_INDX against AttrArray::size before reading the
  // matrix/light source. Gekko exposes only base+stride, so grow exactly to
  // the byte touched by this indexed XF command instead of exposing the rest
  // of MEM1/FakeVMEM (which would also poison storage staging sizes).
  const auto required32=static_cast<std::uint32_t>(required);
  if(array.size<required32) {
    array.size=required32;
    array.cachedRange={};
    g_gxState.dirty |= DirtyImmediates;
    static unsigned logs=0u;
    if(logs++<64u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_XF_ARRAY_WINDOW_V86=1 op=0x%02x attr=%u index=%u stride=%u words=%u bytes=%u available=%u\n",
                   unsigned(op),array_type,source_index,unsigned(array.stride),words,
                   required32,available);
  }
  return true;
}

std::uint32_t NormalizeGuestCacheAddress(std::uint32_t address) noexcept {
  // Preserve the SDK VM aperture; normalize cached/uncached MEM1 aliases to the
  // same physical range used by the rest of the standalone runtime.
  if(address>=0x7e000000u && address<0x80000000u) return address;
  return (address & 0x80000000u)!=0u ? address & 0x3fffffffu : address;
}

bool GuestCacheLineOverlaps(std::uint32_t address, std::uint32_t base,
                            std::uint64_t bytes) noexcept {
  if(bytes==0u) return false;
  const std::uint64_t line_begin=std::uint64_t(NormalizeGuestCacheAddress(address)&~31u);
  const std::uint64_t line_end=line_begin+32u;
  const std::uint64_t range_begin=NormalizeGuestCacheAddress(base);
  const std::uint64_t range_end=range_begin+bytes;
  return line_begin<range_end && range_begin<line_end;
}

void NotifyGuestVertexCacheControl(std::uint8_t operation, std::uint32_t address) {
  using namespace aurora::gx;
  // 0/1/2 are dcbst/dcbf/dcbi in HostRuntime. These are the hardware
  // publication/invalidation points for CPU-authored vertex data. icbi is code
  // coherency and intentionally does not enter the renderer.
  if(operation>2u) return;
  ++g_array_cache_control_events;

  unsigned invalidated=0u;
  for(int i=GX_VA_POS;i<GX_VA_MAX_ATTR;++i) {
    auto& array=g_gxState.arrays[i];
    if(array.cachedRange.size==0u || array.size==0u || array.data==nullptr) continue;
    if(!GuestCacheLineOverlaps(address,g_array_guest_base[i],array.size)) continue;
    array.cachedRange={};
    ++invalidated;
    ++g_array_cache_invalidations;
  }
  if(invalidated!=0u) {
    // Prevent Aurora from merging the next retail draw with a command that
    // referenced the pre-flush storage snapshot.
    g_gxState.dirty |= DirtyImmediates;
    if(g_array_cache_control_logs++<64u) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_DYNAMIC_VERTEX_V141 event=invalidate op=%u ea=%08x line=%08x arrays=%u\\n",
                   unsigned(operation),address,NormalizeGuestCacheAddress(address)&~31u,invalidated);
    }
  }
}

void RefreshIndexedArraySnapshots() {
  using namespace aurora::gx;
  bool any=false;
  for(int i=GX_VA_POS;i<=GX_VA_TEX7;++i) {
    auto& array=g_gxState.arrays[i];
    if(array.data && array.cachedRange.size!=0u) {
      array.cachedRange={};
      any=true;
    }
  }
  if(any) g_gxState.dirty |= DirtyImmediates;
}

bool DecodeRawCopyFormat(std::uint32_t word, GXTexFmt* out) {
  using namespace aurora::gx;
  if(!out) return false;
  // BP 0x52 stores the 4-bit EFB-copy format rotated left by one. Dolphin's
  // hardware model expresses the inverse as fmt/2 + (fmt&1)*8.
  const std::uint32_t encoded=Bits(word,3u,4u);
  const std::uint32_t real=(encoded>>1u)|((encoded&1u)<<3u);
  const bool intensity=Bits(word,15u,1u)!=0u;
  const bool auto_conv=Bits(word,16u,1u)!=0u;
  const bool depth=g_gxState.pixelFmt==GX_PF_Z24;

  if(depth) {
    switch(real) {
    case 0x0u: *out=GX_CTF_Z4; return true;
    case 0x1u: *out=GX_TF_Z8; return true;
    case 0x3u: *out=GX_TF_Z16; return true;
    case 0x6u: *out=GX_TF_Z24X8; return true;
    case 0x9u: *out=GX_CTF_Z8M; return true;
    case 0xAu: *out=GX_CTF_Z8L; return true;
    case 0xBu: *out=static_cast<GXTexFmt>(0x3Bu); return true; // Z16R = 0x0B | ZTF | CTF
    case 0xCu: *out=GX_CTF_Z16L; return true;
    default: return false;
    }
  }
  if(intensity) {
    switch(real) {
    case 0x0u: *out=GX_TF_I4; return true;
    case 0x1u: *out=GX_TF_I8; return true;
    case 0x2u: *out=GX_TF_IA4; return true;
    case 0x3u: *out=GX_TF_IA8; return true;
    default: return false;
    }
  }
  switch(real) {
  case 0x0u: *out=GX_CTF_R4; return true;
  case 0x2u: *out=GX_CTF_RA4; return true;
  case 0x3u: *out=GX_CTF_RA8; return true;
  case 0x4u: *out=GX_TF_RGB565; return true;
  case 0x5u: *out=GX_TF_RGB5A3; return true;
  case 0x6u: *out=auto_conv ? GX_CTF_YUVA8 : GX_TF_RGBA8; return true;
  case 0x7u: *out=GX_CTF_A8; return true;
  case 0x8u: *out=GX_CTF_R8; return true;
  case 0x9u: *out=GX_CTF_G8; return true;
  case 0xAu: *out=GX_CTF_B8; return true;
  case 0xBu: *out=GX_CTF_RG8; return true;
  case 0xCu: *out=GX_CTF_GB8; return true;
  case 0xEu: *out=GX_TF_CMPR; return true;
  default: return false;
  }
}

void TrackRawCopyRegister(std::uint8_t reg, std::uint32_t word) {
  using namespace aurora::gx;
  const std::uint32_t value=word&0x00ffffffu;
  switch(reg) {
  case 0x49u: {
    const auto left=static_cast<std::int32_t>(Bits(value,0u,10u));
    const auto top=static_cast<std::int32_t>(Bits(value,10u,10u));
    g_gxState.texCopySrc.x=left;
    g_gxState.texCopySrc.y=top;
    break;
  }
  case 0x4Au: {
    const auto width=Bits(value,0u,10u)+1u;
    const auto height=Bits(value,10u,10u)+1u;
    g_gxState.texCopySrc.width=static_cast<std::int32_t>(width);
    g_gxState.texCopySrc.height=static_cast<std::int32_t>(height);
    // A texture copy has no vertical scaling. The retail BP stream does not
    // carry the high-level GXSetTexCopyDst dimensions directly; for raw FIFO
    // use the exact source rectangle dimensions (the common SDK contract) and
    // retain stride separately for diagnostics.
    g_gxState.texCopyDstWidth=width;
    g_gxState.texCopyDstHeight=height;
    g_gxState.texCopyDstWide=true;
    g_copy_src_valid=true;
    break;
  }
  case 0x4Bu: {
    const std::uint32_t physical=(value&0x001fffffu)<<5u;
    const void* ptr=nullptr; std::uint32_t available=0;
    if(Resolve(physical,1u,&ptr,&available)) {
      g_gxState.texCopyDest=ptr;
      g_copy_dest_physical=physical;
      g_copy_dest_valid=true;
    } else {
      g_gxState.texCopyDest=nullptr;
      g_copy_dest_physical=physical;
      g_copy_dest_valid=false;
    }
    break;
  }
  case 0x4Du:
    g_copy_stride_bytes=(value&0x3ffu)<<5u;
    break;
  case 0x4Eu:
    g_copy_yscale_raw=value&0x1ffu;
    break;
  default:
    break;
  }
}

bool PrepareRawTextureCopy(std::uint32_t word) {
  using namespace aurora::gx;
  GXTexFmt format{};
  const bool format_valid=DecodeRawCopyFormat(word,&format);
  if(g_z16r_copy_trace && Bits(word,3u,4u)==0x7u) {
    const std::uint32_t rows=g_gxState.texCopySrc.height>0 ?
        (static_cast<std::uint32_t>(g_gxState.texCopySrc.height)+3u)/4u : 0u;
    const std::uint64_t span=std::uint64_t(g_copy_stride_bytes)*rows;
    g_z16r_copy_dest=g_copy_dest_physical;
    g_z16r_copy_span=static_cast<std::uint32_t>(std::min<std::uint64_t>(span,0xffffffffu));
    g_z16r_bound_maps=0;
    for(unsigned map=0;map<MaxTextures;++map) {
      if((g_texture_image3_valid&(1u<<map))==0u) continue;
      const std::uint64_t base=std::uint64_t(g_texture_image3_word[map]&0x00ffffffu)<<5u;
      if(base>=g_z16r_copy_dest && base<std::uint64_t(g_z16r_copy_dest)+g_z16r_copy_span)
        g_z16r_bound_maps=static_cast<std::uint8_t>(g_z16r_bound_maps|(1u<<map));
    }
    if(g_z16r_copy_logs++<96u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_Z16R_TRACE_V1 event=copy pe_ctrl=%06x pixfmt=%u ctrl=%06x src=%d,%d,%dx%d dest=%08x span=%u stride=%u format=%u valid=%u\n",
                   g_gxState.bpRegCache[0x43u]&0x00ffffffu,
                   unsigned(g_gxState.pixelFmt),word&0x00ffffffu,
                   g_gxState.texCopySrc.x,g_gxState.texCopySrc.y,
                   g_gxState.texCopySrc.width,g_gxState.texCopySrc.height,
                   g_z16r_copy_dest,g_z16r_copy_span,g_copy_stride_bytes,
                   unsigned(format),format_valid?1u:0u);
  }
  if(!g_copy_src_valid || !g_copy_dest_valid || !g_gxState.texCopyDest || !format_valid) {
    static unsigned failures=0;
    if(failures++<16u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_EFB_COPY_V2=0 src=%u dest=%u phys=%08x ctrl=%06x action=skip-invalid-copy\n",
                   g_copy_src_valid?1u:0u,g_copy_dest_valid?1u:0u,
                   g_copy_dest_physical,word&0x00ffffffu);
    return false;
  }
  g_gxState.texCopyFmt=format;
  static unsigned logs=0;
  if(logs++<24u)
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_EFB_COPY_V2=1 src=%d,%d %dx%d dest_phys=%08x stride=%u fmt=%u depth=%u\n",
                 g_gxState.texCopySrc.x,g_gxState.texCopySrc.y,
                 g_gxState.texCopySrc.width,g_gxState.texCopySrc.height,
                 g_copy_dest_physical,g_copy_stride_bytes,unsigned(format),
                 g_gxState.pixelFmt==GX_PF_Z24?1u:0u);
  return true;
}


XfbZeroCopySlot* FindXfbSlot(std::uint32_t address) {
  for (auto& slot : g_xfb_slots) {
    if (slot.valid && slot.address == address) return &slot;
  }
  return nullptr;
}

// VI field origins are addresses inside XFB memory, not necessarily the exact
// base used by GXCopyDisp.  A common interlaced layout is BOTTOM = TOP + one
// 640x2-byte scanline (0x500).  Resolve such addresses against the latched XFB
// memory range instead of treating them as unrelated textures.
XfbZeroCopySlot* FindXfbSlotForScanout(std::uint32_t address,
                                      std::uint32_t* byte_offset) {
  if (byte_offset) *byte_offset = 0u;
  if (auto* exact = FindXfbSlot(address)) return exact;

  XfbZeroCopySlot* newest = nullptr;
  std::uint32_t newest_offset = 0u;
  for (auto& slot : g_xfb_slots) {
    if (!slot.valid || slot.span_bytes == 0u || address < slot.address) continue;
    const std::uint64_t offset = static_cast<std::uint64_t>(address - slot.address);
    if (offset >= slot.span_bytes) continue;
    if (slot.stride_bytes != 0u && (offset % slot.stride_bytes) != 0u) continue;
    if (!newest || slot.generation > newest->generation) {
      newest = &slot;
      newest_offset = static_cast<std::uint32_t>(offset);
    }
  }
  if (newest && byte_offset) *byte_offset = newest_offset;
  return newest;
}

XfbZeroCopySlot* AcquireXfbSlot(std::uint32_t address) {
  if (auto* existing = FindXfbSlot(address)) return existing;
  for (auto& slot : g_xfb_slots) {
    if (!slot.valid) {
      slot.address = address;
      return &slot;
    }
  }
  auto* oldest = &g_xfb_slots[0];
  for (auto& slot : g_xfb_slots) {
    if (slot.generation < oldest->generation) oldest = &slot;
  }
  oldest->address = address;
  oldest->stride_bytes = 0u;
  oldest->native_width = 0u;
  oldest->native_height = 0u;
  oldest->span_bytes = 0u;
  oldest->generation = 0u;
  oldest->snapshot.reset();
  oldest->frame_buffer = {};
  oldest->resolved = {};
  oldest->valid = false;
  return oldest;
}

bool RenderTextureReusable(const aurora::webgpu::TextureWithSampler& texture,
                           const wgpu::Extent3D& size, wgpu::TextureFormat format) {
  return texture.texture && texture.view && texture.size.width == size.width &&
         texture.size.height == size.height && texture.format == format;
}

const aurora::webgpu::TextureWithSampler* XfbPresentSource(const XfbZeroCopySlot& slot) {
  if (!slot.valid) return nullptr;
  // V12 XFB cache entries are always independent single-sample Aurora copy
  // textures.  Do not select the old MSAA-resolved rotation slot based on the
  // live EFB sample count; that would make every cached XFB invisible whenever
  // host MSAA is enabled.
  if (slot.snapshot)
    return slot.frame_buffer.texture ? &slot.frame_buffer : nullptr;
  if (aurora::webgpu::g_graphicsConfig.msaaSamples > 1)
    return slot.resolved.texture ? &slot.resolved : nullptr;
  return slot.frame_buffer.texture ? &slot.frame_buffer : nullptr;
}


struct InterpolationBlendResources {
  wgpu::BindGroupLayout layout;
  wgpu::RenderPipeline pipeline;
  wgpu::Buffer params;
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
  wgpu::ShaderSourceWGSL source{};
  source.code = R"WGSL(
struct Params { value: vec4<f32>, };
struct VertexOutput { @builtin(position) position: vec4<f32>, @location(0) uv: vec2<f32>, };
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
  if (!g_surface && !refresh_surface(true)) return false;
  if (!g_surface) return false;

  wgpu::SurfaceTexture surface_texture;
  {
    window::SurfaceLock surface_lock;
    if (!window::is_presentable() || !g_surface) return false;
    g_surface.GetCurrentTexture(&surface_texture);
  }
  if (surface_texture.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal || !surface_texture.texture) {
    release_surface();
    return false;
  }
  auto swap_texture = std::move(surface_texture.texture);
  auto swap_view = swap_texture.CreateView();
  if (!swap_view) return false;

  auto& resources = BlendResources();
  const std::array<float, 4> params{alpha, 0.0f, 0.0f, 0.0f};
  g_queue.WriteBuffer(resources.params, 0, params.data(), sizeof(params));
  const std::array bind_entries{
      wgpu::BindGroupEntry{.binding = 0, .buffer = resources.params, .size = 16},
      wgpu::BindGroupEntry{.binding = 1, .sampler = current.sampler},
      wgpu::BindGroupEntry{.binding = 2, .textureView = previous.view},
      wgpu::BindGroupEntry{.binding = 3, .textureView = current.view},
  };
  const wgpu::BindGroupDescriptor bind_desc{.layout = resources.layout, .entryCount = bind_entries.size(),
                                             .entries = bind_entries.data()};
  const auto bind_group = g_device.CreateBindGroup(&bind_desc);
  constexpr wgpu::CommandEncoderDescriptor encoder_desc{.label = "GekkoAOT v49 interpolation encoder"};
  auto encoder = g_device.CreateCommandEncoder(&encoder_desc);
  const auto viewport = PresentViewportForAspect(g_graphicsConfig.surfaceConfiguration.width,
                                                 g_graphicsConfig.surfaceConfiguration.height,
                                                 current.size.width, current.size.height);
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

  // v134: keep interpolation history in the host presentation format and copy
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

bool LatchDisplayCopyCached(std::uint32_t trigger_word, bool clear) {
  using namespace aurora;
  using namespace aurora::gx;
  using namespace aurora::webgpu;

  if (!g_copy_src_valid || !g_copy_dest_valid || !g_gxState.texCopyDest ||
      g_copy_dest_physical == 0u || !g_frame_open) {
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
  slot->address = g_copy_dest_physical;
  slot->stride_bytes = g_copy_stride_bytes != 0u ? g_copy_stride_bytes : copy_width * 2u;
  slot->native_width = copy_width;
  slot->native_height = copy_height;
  slot->span_bytes = static_cast<std::uint64_t>(slot->stride_bytes) * copy_height;
  slot->generation = ++g_xfb_generation;
  slot->valid = true;

  // v110: GXCopyDisp is the producer boundary.  Submit the EFB->XFB resolve
  // here, exactly once, rather than cutting Aurora's live EFB recorder from a
  // later VI/host scanout.  VI may scan the same XFB more than once (fields,
  // retraces, host scheduler collapse); those scanouts must be read-only.
  aurora_end_frame_no_present();
  CaptureFrameBoundary();
  g_frame_open = false;
  if (g_array_frame_refresh) RefreshIndexedArraySnapshots();

  // v127i: v110 moved the producer boundary to GXCopyDisp, but the v48
  // interpolation history was never reconnected to that new boundary.  The
  // snapshot helper consequently became dead code and FrameInterpolationReady()
  // could never become true.  Capture the immutable resolved XFB after the
  // producer frame has been submitted; render-worker queue ordering guarantees
  // that the history copy observes the completed GXCopyDisp result.
  const bool interpolation_ready =
      g_frame_interpolation_enabled && SnapshotInterpolationFrame(slot->frame_buffer);
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

  g_frame_ready_xfb = g_copy_dest_physical;
  g_frame_ready_pending = true;

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

void DrainFifo();
void PollEvents();

std::uint8_t ClampByte(int value) {
  return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

void DecodeYuvyPair(const std::uint8_t* src, std::uint8_t* dst) {
  // GameCube XFB is YUVY 4:2:2: Y0 U Y1 V. BT.601 studio-range
  // conversion matches retail/Dolphin scanout semantics closely enough for a
  // host RGBA texture while keeping guest memory untouched.
  const int u = static_cast<int>(src[1]) - 128;
  const int v = static_cast<int>(src[3]) - 128;
  for (unsigned pixel = 0; pixel < 2u; ++pixel) {
    const int y = std::max(static_cast<int>(src[pixel * 2u]) - 16, 0);
    const int r = (298 * y + 409 * v + 128) >> 8;
    const int g = (298 * y - 100 * u - 208 * v + 128) >> 8;
    const int b = (298 * y + 516 * u + 128) >> 8;
    dst[pixel * 4u + 0u] = ClampByte(r);
    dst[pixel * 4u + 1u] = ClampByte(g);
    dst[pixel * 4u + 2u] = ClampByte(b);
    dst[pixel * 4u + 3u] = 0xffu;
  }
}

std::uint64_t HashXfbRow(std::uint64_t hash, const std::uint8_t* data,
                         std::uint32_t bytes) {
  constexpr std::uint64_t kFnvPrime = 1099511628211ull;
  for (std::uint32_t i = 0; i < bytes; ++i) {
    hash ^= data[i];
    hash *= kFnvPrime;
  }
  return hash;
}

XfbZeroCopySlot* NewestXfbSlot() {
  XfbZeroCopySlot* newest = nullptr;
  for (auto& slot : g_xfb_slots) {
    if (!slot.valid) continue;
    if (!newest || slot.generation > newest->generation) newest = &slot;
  }
  return newest;
}

bool EnsureRamXfbTexture(std::uint32_t width, std::uint32_t height) {
  using namespace aurora::webgpu;
  if (g_ram_xfb_texture && g_ram_xfb_width == width && g_ram_xfb_height == height)
    return true;

  g_ram_xfb_texture = aurora::gfx::new_dynamic_texture_2d(
      width, height, 1u, GX_TF_RGBA8_PC, "GekkoAOT RAM XFB v57");
  if (!g_ram_xfb_texture || !g_ram_xfb_texture->texture ||
      !g_ram_xfb_texture->sampleTextureView) {
    g_ram_xfb_texture.reset();
    g_ram_xfb_present = {};
    g_ram_xfb_width = 0u;
    g_ram_xfb_height = 0u;
    return false;
  }

  g_ram_xfb_present = TextureWithSampler{
      .texture = g_ram_xfb_texture->texture,
      .view = g_ram_xfb_texture->sampleTextureView,
      .size = g_ram_xfb_texture->size,
      .format = g_ram_xfb_texture->format,
      .sampler = g_frameBuffer.sampler,
  };
  g_ram_xfb_width = width;
  g_ram_xfb_height = height;
  return true;
}

bool ResolveXfbField(std::uint32_t address, std::uint32_t width,
                     std::uint32_t stride_bytes, std::uint32_t rows,
                     const std::uint8_t** data) {
  if (!data || address == 0u || width == 0u || rows == 0u) return false;
  const std::uint64_t row_bytes = static_cast<std::uint64_t>(width) * 2u;
  const std::uint64_t span = static_cast<std::uint64_t>(rows - 1u) * stride_bytes + row_bytes;
  if (span == 0u || span > std::numeric_limits<std::uint32_t>::max()) return false;
  const void* pointer = nullptr;
  std::uint32_t available = 0u;
  if (!Resolve(address, static_cast<std::uint32_t>(span), &pointer, &available) ||
      available < span || pointer == nullptr)
    return false;
  *data = static_cast<const std::uint8_t*>(pointer);
  return true;
}

bool PresentRamXfb(std::uint32_t top, std::uint32_t bottom,
                   std::uint32_t width, std::uint32_t stride_bytes,
                   std::uint32_t field_height) {
  if (top == 0u) return false;

  // VI geometry is authoritative when the v57 host ABI is available. Keep a
  // conservative fallback for old callers so this optional DSO symbol remains
  // useful during mixed-version testing.
  const XfbZeroCopySlot* recent = NewestXfbSlot();
  if (width == 0u && recent) width = recent->native_width;
  if (width == 0u) width = 640u;

  const std::uint32_t line_bytes = width * 2u;
  const bool split_origins = bottom != 0u && bottom != top;
  if (stride_bytes == 0u) {
    if (split_origins && bottom > top && bottom - top == line_bytes)
      stride_bytes = line_bytes * 2u;
    else
      stride_bytes = line_bytes;
  }

  // STD/WPL == 2 is the normal interlaced VI layout: top and bottom point at
  // adjacent scanlines, while each field advances by two scanlines.
  const bool interlaced = split_origins && stride_bytes >= line_bytes * 2u;
  if (field_height == 0u && recent && recent->native_height != 0u)
    field_height = interlaced ? std::max(recent->native_height / 2u, 1u)
                              : recent->native_height;
  if (field_height == 0u) field_height = interlaced ? 224u : 448u;

  const std::uint32_t output_height = interlaced ? field_height * 2u : field_height;
  if (width < 2u || (width & 1u) != 0u || width > 1024u ||
      field_height > 576u || output_height == 0u || output_height > 1152u ||
      stride_bytes < line_bytes)
    return false;

  const std::uint8_t* top_data = nullptr;
  const std::uint8_t* bottom_data = nullptr;
  if (!ResolveXfbField(top, width, stride_bytes, field_height, &top_data)) return false;
  if (interlaced &&
      !ResolveXfbField(bottom, width, stride_bytes, field_height, &bottom_data))
    return false;

  const std::size_t rgba_bytes =
      static_cast<std::size_t>(width) * output_height * 4u;
  g_ram_xfb_rgba.resize(rgba_bytes);
  std::uint64_t source_hash = 1469598103934665603ull;
  for (std::uint32_t field_row = 0u; field_row < field_height; ++field_row) {
    const auto decode_row = [&](const std::uint8_t* source, std::uint32_t output_row) {
      const std::uint8_t* src = source + static_cast<std::size_t>(field_row) * stride_bytes;
      source_hash = HashXfbRow(source_hash, src, line_bytes);
      std::uint8_t* dst = g_ram_xfb_rgba.data() +
                          static_cast<std::size_t>(output_row) * width * 4u;
      for (std::uint32_t x = 0u; x < width; x += 2u)
        DecodeYuvyPair(src + static_cast<std::size_t>(x) * 2u,
                       dst + static_cast<std::size_t>(x) * 4u);
    };
    if (interlaced) {
      decode_row(top_data, field_row * 2u);
      decode_row(bottom_data, field_row * 2u + 1u);
    } else {
      decode_row(top_data, field_row);
    }
  }

  // A VI cache miss is not sufficient evidence that guest RAM owns the XFB.
  // Aurora's zero-copy GX path deliberately keeps many display copies only on
  // the GPU; interpreting stale MEM1 at the same address as YUVY produces the
  // characteristic green/black screen seen on ordinary GX-rendered titles.
  //
  // v82: keep independent evidence for several rotating XFBs. Retail titles
  // commonly double/quad-buffer by changing the VI address every field. The
  // old single candidate forgot its hash history on every address change, so a
  // real RAM-owned XFB could remain forever at observations=1/changes=0 even
  // while the same addresses visibly changed on later visits.
  ++g_ram_xfb_candidate_epoch;
  RamXfbCandidate* candidate = nullptr;
  RamXfbCandidate* replacement = &g_ram_xfb_candidates[0];
  for (auto& slot : g_ram_xfb_candidates) {
    if (slot.valid && slot.top == top && slot.bottom == bottom &&
        slot.width == width && slot.stride == stride_bytes &&
        slot.field_height == field_height) {
      candidate = &slot;
      break;
    }
    if (!slot.valid || slot.last_seen < replacement->last_seen)
      replacement = &slot;
  }
  if (!candidate) {
    candidate = replacement;
    *candidate = {};
    candidate->top = top;
    candidate->bottom = bottom;
    candidate->width = width;
    candidate->stride = stride_bytes;
    candidate->field_height = field_height;
    candidate->valid = true;
  }
  candidate->last_seen = g_ram_xfb_candidate_epoch;

  const bool hash_changed = candidate->observations != 0u &&
                            candidate->last_hash != source_hash;
  if (hash_changed) ++candidate->changes;
  ++candidate->observations;
  candidate->last_hash = source_hash;
  if (!candidate->armed && candidate->observations >= 3u &&
      candidate->changes >= 2u) {
    candidate->armed = true;
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_RAM_XFB_V82=1 action=arm top=%08x bottom=%08x width=%u stride=%u field_height=%u observations=%u changes=%llu hash=%016llx policy=multi-xfb-changing-mem1 slots=%zu\n",
                 top, bottom, width, stride_bytes, field_height,
                 candidate->observations,
                 static_cast<unsigned long long>(candidate->changes),
                 static_cast<unsigned long long>(source_hash),
                 kRamXfbCandidateSlots);
  }
  if (!candidate->armed) {
    static unsigned probe_logs = 0u;
    if (probe_logs++ < 128u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_RAM_XFB_V82=0 action=hold top=%08x bottom=%08x width=%u stride=%u field_height=%u observations=%u changes=%llu hash=%016llx reason=awaiting-per-xfb-changing-mem1\n",
                   top, bottom, width, stride_bytes, field_height,
                   candidate->observations,
                   static_cast<unsigned long long>(candidate->changes),
                   static_cast<unsigned long long>(source_hash));
    return false;
  }

  if (!EnsureRamXfbTexture(width, output_height)) return false;
  const wgpu::TexelCopyTextureInfo destination{
      .texture = g_ram_xfb_texture->texture,
      .mipLevel = 0u,
      .origin = {0u, 0u, 0u},
      .aspect = wgpu::TextureAspect::All,
  };
  const wgpu::TexelCopyBufferLayout layout{
      .offset = 0u,
      .bytesPerRow = width * 4u,
      .rowsPerImage = output_height,
  };
  const wgpu::Extent3D upload_size{
      .width = width,
      .height = output_height,
      .depthOrArrayLayers = 1u,
  };

  // v110: RAM-owned XFB scanout is also presentation-only.  Queue::WriteTexture
  // writes directly into the dedicated CopyDst texture and therefore does not
  // need Aurora's active EFB frame packet to be ended/restarted at VI time.
  // The following presentation is enqueued after this queue write.
  aurora::webgpu::g_queue.WriteTexture(&destination, g_ram_xfb_rgba.data(),
                                        rgba_bytes, &layout, &upload_size);
  aurora_present_texture(&g_ram_xfb_present);

  ++g_ram_xfb_generation;
  static unsigned logs = 0u;
  if (logs++ < 128u)
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_RAM_XFB_V59=1 action=present top=%08x bottom=%08x width=%u stride=%u field_height=%u output_height=%u interlaced=%u generation=%llu hash=%016llx changed=%u changes=%llu observations=%u sample=%02x%02x%02x%02x%02x%02x%02x%02x source=mem1-yuvy\n",
                 top, bottom, width, stride_bytes, field_height, output_height,
                 interlaced ? 1u : 0u,
                 static_cast<unsigned long long>(g_ram_xfb_generation),
                 static_cast<unsigned long long>(source_hash),
                 hash_changed ? 1u : 0u,
                 static_cast<unsigned long long>(candidate->changes),
                 candidate->observations,
                 top_data[0], top_data[1], top_data[2], top_data[3],
                 top_data[4], top_data[5], top_data[6], top_data[7]);
  return true;
}

void PresentViXfb(std::uint32_t top, std::uint32_t bottom, bool address_keyed,
                  std::uint32_t width = 0u, std::uint32_t stride_bytes = 0u,
                  std::uint32_t field_height = 0u) {
  if (!g_ready) return;
  DrainFifo();
  PollEvents();

  std::uint32_t byte_offset = 0u;
  XfbZeroCopySlot* slot =
      address_keyed && top != 0u ? FindXfbSlotForScanout(top, &byte_offset) : nullptr;
  const auto* source = slot ? XfbPresentSource(*slot) : nullptr;

  if (address_keyed && top != 0u) {
    if (source == nullptr) {
      if (PresentRamXfb(top, bottom, width, stride_bytes, field_height)) return;
      static unsigned misses = 0;
      if (misses++ < 32u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_XFB_SCANOUT_V12=0 top=%08x bottom=%08x reason=xfb-not-cached action=hold-last-scanout ram_fallback=failed\n",
                     top, bottom);
      return;
    }

    // v110: presentation is a pure consumer.  The producer frame containing
    // this immutable XFB was already submitted at GXCopyDisp.  Never end or
    // restart the live EFB here: VI can legally scan the same generation on
    // multiple field/retrace boundaries.
    aurora_present_texture(source);

    const std::uint32_t row_offset =
        slot->stride_bytes != 0u ? byte_offset / slot->stride_bytes : 0u;
    static unsigned logs = 0;
    if (logs++ < 64u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_XFB_SCANOUT_V12=1 request=%08x top=%08x bottom=%08x base=%08x offset=%u row=%u native=%ux%u stride=%u generation=%llu source=vi-selected-aurora-xfb-cache\n",
                   top, top, bottom, slot->address, byte_offset, row_offset,
                   slot->native_width, slot->native_height, slot->stride_bytes,
                   static_cast<unsigned long long>(slot->generation));
    return;
  }

  // Legacy/boot path only, before VI has supplied a non-zero XFB address.
  if (g_frame_open) {
    aurora_end_frame();
    g_frame_open = false;
    if (g_array_frame_refresh) RefreshIndexedArraySnapshots();
  }
  if (!g_quit && aurora_begin_frame()) g_frame_open = true;
}

void MapTextureImage3(std::uint8_t reg, std::uint32_t word) {
  using namespace aurora::gx;
  const unsigned map = (reg>=0xB4u ? 4u : 0u) + (reg & 3u);
  if(map>=MaxTextures) return;
  auto& slot=g_gxState.loadedTextures[map];
  const std::uint8_t bit=static_cast<std::uint8_t>(1u<<map);
  const std::uint32_t guest=(word & 0x00ffffffu)<<5;
  const std::size_t texture_bytes=aurora::gx::texture::texture_source_size(
      slot.format(),slot.width(),slot.height(),slot.mip_count());
  const std::uint32_t required=texture_bytes>std::numeric_limits<std::uint32_t>::max()
                                   ? 0u : static_cast<std::uint32_t>(texture_bytes);
  if(g_z16r_copy_trace && g_z16r_copy_span!=0u) {
    const std::uint64_t copy_begin=g_z16r_copy_dest;
    const std::uint64_t copy_end=copy_begin+g_z16r_copy_span;
    const std::uint64_t tex_begin=guest;
    const std::uint64_t tex_end=tex_begin+required;
    const bool overlap=required!=0u && tex_begin<copy_end && copy_begin<tex_end;
    const std::uint8_t bit=static_cast<std::uint8_t>(1u<<map);
    g_z16r_bound_maps=static_cast<std::uint8_t>(overlap ?
        (g_z16r_bound_maps|bit) : (g_z16r_bound_maps&~bit));
    if(overlap && g_z16r_bind_logs++<96u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_Z16R_TRACE_V1 event=bind map=%u guest=%08x bytes=%u fmt=%u size=%ux%u copy=%08x span=%u\n",
                   map,guest,required,unsigned(slot.format()),slot.width(),slot.height(),
                   g_z16r_copy_dest,g_z16r_copy_span);
  }
  const void* ptr=nullptr; std::uint32_t available=0;
  if(required==0u || !Resolve(guest,required,&ptr,&available) || available<required) {
    // v87: never retain the previous host pointer when the guest changes
    // IMAGE3 to an unmapped base. Reusing stale texture storage produces the
    // classic "bad VRAM/overclock" corruption pattern across otherwise valid
    // draws. Fail closed and force a fresh bind when a later address resolves.
    g_texture_image3_valid=static_cast<std::uint8_t>(g_texture_image3_valid & ~bit);
    slot.data=nullptr;
    slot.texDataVersion=static_cast<std::uint32_t>(++g_texture_generation);
    g_gxState.dirty |= DirtyTextures;
    aurora::gx::texture::invalidate_bindings();
    static unsigned bad_texture_logs=0;
    if(bad_texture_logs++<32u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_TEXTURE_BASE_V87=0 map=%u guest=%08x required=%u available=%u action=invalidate-stale-mapping\n",
                   map,guest,required,available);
    return;
  }

  // Games commonly re-emit the same IMAGE3 state for every draw. Treat that as
  // a binding, not a texture mutation. Aurora can then reuse its converted GPU
  // resource instead of new_static_texture_2d/convert/upload on every bind.
  const bool stable_bind=(g_texture_image3_valid&bit)!=0u &&
                         g_texture_image3_word[map]==word && slot.data==ptr;
  if(stable_bind && g_texture_bind_cache) return;

  g_texture_image3_word[map]=word;
  g_texture_image3_valid=static_cast<std::uint8_t>(g_texture_image3_valid|bit);
  slot.data=ptr;
  // Keep object identity stable per hardware texture unit. With the retail
  // IMAGE3 cache disabled for correctness, assigning a fresh texObjId on every
  // bind makes Aurora's object cache grow by one entry per draw and can reach
  // several GiB in minutes. texDataVersion is the mutation/version signal; the
  // object ID is identity, not a generation counter.
  slot.texObjId=static_cast<std::uint32_t>(map+1u);
  slot.texDataVersion=static_cast<std::uint32_t>(++g_texture_generation);
  slot.flags &= ~0x80u;
  g_gxState.dirty |= DirtyTextures;
}

void MapTlut() {
  using namespace aurora::gx;
  const auto load=g_gxState.bpRegCache[0x64];
  const auto trigger=g_gxState.bpRegCache[0x65];
  const unsigned idx=trigger & 0x3ffu;
  if(idx>=MaxTluts) return;
  auto& slot=g_gxState.loadedTluts[idx];
  const std::uint32_t guest=(load & 0x00ffffffu)<<5;
  const std::size_t tlut_bytes=aurora::gx::texture::tlut_source_size(slot.numEntries);
  const std::uint32_t required=tlut_bytes>std::numeric_limits<std::uint32_t>::max()
                                   ? 0u : static_cast<std::uint32_t>(tlut_bytes);
  const void* ptr=nullptr; std::uint32_t available=0;
  if(required==0u || !Resolve(guest,required,&ptr,&available) || available<required) {
    slot.data=nullptr;
    slot.tlutDataVersion=static_cast<std::uint32_t>(++g_texture_generation);
    g_gxState.dirty |= DirtyTextures;
    aurora::gx::texture::invalidate_bindings();
    static unsigned bad_tlut_logs=0;
    if(bad_tlut_logs++<16u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_TLUT_BASE_V87=0 slot=%u guest=%08x required=%u available=%u action=invalidate-stale-mapping\n",
                   idx,guest,required,available);
    return;
  }
  // TLUT loads are explicit mutations, so preserve version bumps here.
  slot.data=ptr;
  // Same rule as textures: keep one stable object identity per TLUT slot and
  // bump only the data version on explicit loads. Otherwise a game that reloads
  // palettes every frame creates an unbounded Aurora object-cache population.
  slot.tlutObjId=static_cast<std::uint32_t>(idx+1u);
  slot.tlutDataVersion=static_cast<std::uint32_t>(++g_texture_generation);
  g_gxState.dirty |= DirtyTextures;
}

void InvalidateTextureCacheV87() {
  using namespace aurora::gx;
  // BP 0x66 is the retail GX texture-cache invalidate command. Pinned Aurora
  // currently treats it as an unhandled BP register, which lets converted host
  // textures survive after the guest explicitly invalidated TMEM. Preserve
  // object identity but advance every bound source generation and invalidate
  // bind groups so the next draw re-reads guest memory.
  unsigned bound=0;
  for(unsigned map=0;map<MaxTextures;++map) {
    auto& slot=g_gxState.loadedTextures[map];
    if(slot.data) ++bound;
    slot.texDataVersion=static_cast<std::uint32_t>(++g_texture_generation);
  }
  g_texture_image3_valid=0;
  aurora::gx::texture::invalidate_bindings();
  g_gxState.dirty |= DirtyTextures;
  static unsigned logs=0;
  if(logs++<64u)
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_TEX_INVALIDATE_V87=1 reg=66 bound=%u generation=%llu policy=guest-command\n",
                 bound,static_cast<unsigned long long>(g_texture_generation));
}

enum class DecodeResult : std::uint8_t { Complete, Incomplete, Invalid };

DecodeResult CommandLength(const std::uint8_t* p, std::size_t n,
                           std::size_t* len, std::size_t* need) {
  if(!p||!len||!need) return DecodeResult::Invalid;
  if(n<1u){*need=1u;return DecodeResult::Incomplete;}
  const std::uint8_t cmd=p[0], op=cmd & GX_OPCODE_MASK;
  std::size_t out=1u;
  if(op==kLoadBpOpcode) out=5u;
  else if(op==GX_LOAD_CP_REG) out=6u;
  else if(op==GX_LOAD_XF_REG) {
    if(n<5u){*need=5u;return DecodeResult::Incomplete;}
    out=5u+4u*(((Be32(p+1)>>16)&0xffffu)+1u);
  } else if(op==GX_LOAD_INDX_A||op==GX_LOAD_INDX_B||op==GX_LOAD_INDX_C||op==GX_LOAD_INDX_D) out=5u;
  else if(op==GX_CMD_CALL_DL) out=9u;
  else if(op==GX_CMD_INVL_VC||op==GX_NOP) out=1u;
  else if(op>=0x80u&&op<=0xB8u) {
    if(n<3u){*need=3u;return DecodeResult::Incomplete;}
    const auto fmt=static_cast<GXVtxFmt>(cmd&GX_VAT_MASK);
    const std::uint32_t vertex_size=VertexSize(fmt);
    if(vertex_size==0u) return DecodeResult::Invalid;
    out=3u+std::size_t(Be16(p+1))*vertex_size;
  } else {
    return DecodeResult::Invalid;
  }
  *len=out;
  if(n<out){*need=out;return DecodeResult::Incomplete;}
  *need=1u;
  return DecodeResult::Complete;
}

bool ProcessSpan(const std::uint8_t* data, std::size_t bytes, unsigned depth);

void RememberFifoWrite(std::uint64_t value, std::uint8_t size, std::uint64_t stream_offset,
                       std::uint32_t guest_pc) {
  g_fifo_write_trace[g_fifo_write_trace_head] = {value, stream_offset, guest_pc, size};
  g_fifo_write_trace_head = (g_fifo_write_trace_head + 1u) % g_fifo_write_trace.size();
  g_fifo_write_trace_count = std::min<std::size_t>(g_fifo_write_trace_count + 1u,
                                                   g_fifo_write_trace.size());
}

void RememberDecodedCommand(const std::uint8_t* p, std::size_t len, std::uint64_t stream_offset) {
  if (!p || len == 0u) return;
  FifoCommandTrace trace{};
  trace.stream_offset = stream_offset;
  trace.len = len;
  trace.cmd = p[0];
  g_fifo_last_cmd = p[0];
  g_fifo_last_cmd_len = len;
  const std::uint8_t op = p[0] & GX_OPCODE_MASK;
  if (op >= 0x80u && op <= 0xB8u && len >= 3u) {
    g_fifo_last_draw_fmt = p[0] & GX_VAT_MASK;
    g_fifo_last_draw_count = Be16(p + 1);
    g_fifo_last_draw_stride = g_fifo_last_draw_count == 0u
                                ? 0u
                                : static_cast<std::uint32_t>((len - 3u) / g_fifo_last_draw_count);
    trace.fmt = g_fifo_last_draw_fmt;
    trace.count = g_fifo_last_draw_count;
    trace.stride = g_fifo_last_draw_stride;
  }
  g_fifo_command_trace[g_fifo_command_trace_head] = trace;
  g_fifo_command_trace_head = (g_fifo_command_trace_head + 1u) % g_fifo_command_trace.size();
  g_fifo_command_trace_count = std::min<std::size_t>(g_fifo_command_trace_count + 1u,
                                                     g_fifo_command_trace.size());
}

void DumpFifoFault(std::size_t fault_offset) {
  if (g_fifo_fault_reported) return;
  g_fifo_fault_reported = true;

  const std::size_t fifo_size = g_fifo.size();
  const std::size_t begin = fault_offset > 48u ? fault_offset - 48u : 0u;
  const std::size_t end = std::min<std::size_t>(fifo_size, fault_offset + 64u);
  const unsigned bad = fault_offset < fifo_size ? unsigned(g_fifo[fault_offset]) : 0xffu;

  const std::uint64_t absolute_fault = g_fifo_stream_base + fault_offset;
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_FIFO_FORENSIC_V2=1 fault_offset=%zu absolute=%llu fifo_base=%llu fifo_size=%zu unread=%zu need=%zu bad=0x%02x last_cmd=0x%02x last_len=%zu last_draw_fmt=%u last_draw_count=%u last_draw_stride=%u\n",
               fault_offset, static_cast<unsigned long long>(absolute_fault),
               static_cast<unsigned long long>(g_fifo_stream_base), fifo_size,
               fault_offset <= fifo_size ? fifo_size - fault_offset : 0u,
               g_fifo_need, bad, unsigned(g_fifo_last_cmd), g_fifo_last_cmd_len,
               unsigned(g_fifo_last_draw_fmt), unsigned(g_fifo_last_draw_count),
               unsigned(g_fifo_last_draw_stride));

  std::fprintf(stderr, "GEKKOAOT_NATIVE_GX_FIFO_FORENSIC_BYTES begin=%zu end=%zu data=", begin, end);
  for (std::size_t i = begin; i < end; ++i) {
    if (i == fault_offset) std::fprintf(stderr, "[");
    std::fprintf(stderr, "%02x", unsigned(g_fifo[i]));
    if (i == fault_offset) std::fprintf(stderr, "]");
    if (i + 1u < end) std::fprintf(stderr, " ");
  }
  std::fprintf(stderr, "\n");

  const std::size_t count = g_fifo_write_trace_count;
  const std::size_t first = (g_fifo_write_trace_head + g_fifo_write_trace.size() - count) %
                            g_fifo_write_trace.size();
  std::fprintf(stderr, "GEKKOAOT_NATIVE_GX_FIFO_FORENSIC_WRITES count=%zu", count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto& w = g_fifo_write_trace[(first + i) % g_fifo_write_trace.size()];
    std::fprintf(stderr, " @%llu/pc=%08x/s%u:0x%0*llx",
                 static_cast<unsigned long long>(w.stream_offset), unsigned(w.guest_pc),
                 unsigned(w.size), unsigned(w.size) * 2,
                 static_cast<unsigned long long>(w.value));
  }
  std::fprintf(stderr, "\n");

  const std::size_t command_count = g_fifo_command_trace_count;
  const std::size_t command_first =
      (g_fifo_command_trace_head + g_fifo_command_trace.size() - command_count) %
      g_fifo_command_trace.size();
  std::fprintf(stderr, "GEKKOAOT_NATIVE_GX_FIFO_FORENSIC_COMMANDS count=%zu", command_count);
  for (std::size_t i = 0; i < command_count; ++i) {
    const auto& c = g_fifo_command_trace[(command_first + i) % g_fifo_command_trace.size()];
    std::fprintf(stderr, " @%llu:cmd=%02x,len=%zu",
                 static_cast<unsigned long long>(c.stream_offset), unsigned(c.cmd), c.len);
    if (c.fmt < 8u)
      std::fprintf(stderr, ",fmt=%u,count=%u,stride=%u", unsigned(c.fmt),
                   unsigned(c.count), unsigned(c.stride));
  }
  std::fprintf(stderr, "\n");

  if (g_fifo_last_draw_fmt < 8u) {
    using namespace aurora::gx;
    const unsigned f = g_fifo_last_draw_fmt;
    const bool vcd_lo = g_gxState.cpRegValid.test(0x50u);
    const bool vcd_hi = g_gxState.cpRegValid.test(0x60u);
    const bool vat_a = g_gxState.cpRegValid.test(0x70u + f);
    const bool vat_b = g_gxState.cpRegValid.test(0x80u + f);
    const bool vat_c = g_gxState.cpRegValid.test(0x90u + f);
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_FIFO_FORENSIC_CP fmt=%u vcd_lo=%u:0x%08x vcd_hi=%u:0x%08x vat_a=%u:0x%08x vat_b=%u:0x%08x vat_c=%u:0x%08x aurora_last_fmt=%u aurora_last_size=%u\n",
                 f, vcd_lo ? 1u : 0u, g_gxState.cpRegCache[0x50u],
                 vcd_hi ? 1u : 0u, g_gxState.cpRegCache[0x60u],
                 vat_a ? 1u : 0u, g_gxState.cpRegCache[0x70u + f],
                 vat_b ? 1u : 0u, g_gxState.cpRegCache[0x80u + f],
                 vat_c ? 1u : 0u, g_gxState.cpRegCache[0x90u + f],
                 unsigned(g_gxState.lastVtxFmt), unsigned(g_gxState.lastVtxSize));
  }
}

bool ProcessOne(const std::uint8_t* p, std::size_t len, unsigned depth) {
  const std::uint8_t cmd=p[0], op=cmd & GX_OPCODE_MASK;
  if(g_array_base_trace && op==GX_CMD_INVL_VC) ++g_array_base_trace_invalidate;
  if(op==GX_CMD_CALL_DL) {
    // The retail CP has one display-list call level. A stream may call one DL,
    // but a DL cannot recursively call another DL. The old host recursion cap
    // of 16 accepted command streams the hardware never could have executed.
    if(depth>=1u) {
      static unsigned dl_depth_logs=0;
      if(dl_depth_logs++<32u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_DL_DEPTH_V87=0 depth=%u max=1 action=reject-nested-display-list\n",
                     depth);
      return false;
    }
    const std::uint32_t addr=Be32(p+1), bytes=Be32(p+5);
    const void* data=nullptr; std::uint32_t available=0;
    return Resolve(addr,bytes,&data,&available) && available>=bytes &&
           ProcessSpan(static_cast<const std::uint8_t*>(data),bytes,depth+1u);
  }

  if(op==GX_LOAD_CP_REG && p[1]>=0xA0u && p[1]<=0xAFu) {
    MapArrayBase(p[1],Be32(p+2));
    return true;
  }

  if(op==GX_LOAD_INDX_A || op==GX_LOAD_INDX_B ||
     op==GX_LOAD_INDX_C || op==GX_LOAD_INDX_D) {
    if(!PrepareIndexedXfArrayWindow(p,len)) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_FIFO_ERROR opcode=0x%02x action=reject-xf-array-window\n",
                   unsigned(cmd));
      return false;
    }
  }

  if(op==kLoadBpOpcode) {
    const std::uint32_t word=Be32(p+1);
    const std::uint8_t reg=static_cast<std::uint8_t>(word>>24);
    // BP 0xFE masks exactly one following BP write. Aurora applies that mask
    // in handle_bp(), but intercepted display copies never reach its parser.
    // Decode the same effective word before deciding which copy path to take.
    auto& bp_cache=aurora::gx::g_gxState.bpRegCache;
    const std::uint32_t bp_mask=bp_cache[0xFEu]&0x00ffffffu;
    const std::uint32_t effective_word=reg==0xFEu ? word :
        (std::uint32_t(reg)<<24u)|
        ((bp_cache[reg]&~bp_mask)|(word&bp_mask))&0x00ffffffu;
    if(g_z16r_copy_trace && reg==0x43u &&
       (bp_cache[reg]&7u)!=(effective_word&7u) && g_z16r_bp_logs++<96u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_Z16R_TRACE_V1 event=pe-mode before=%u after=%u bp_mask=%06x effective=%06x\n",
                   bp_cache[reg]&7u,effective_word&7u,bp_mask,effective_word&0x00ffffffu);
    if(g_ztexture_trace && (reg==0xF4u || reg==0xF5u) && g_ztexture_logs++<96u) {
      if(reg==0xF4u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_ZTEXTURE_TRACE_V115 event=bias value=%06x mask=%06x\n",
                     effective_word&0x00ffffffu,bp_mask);
      else
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_ZTEXTURE_TRACE_V115 event=control type=%u op=%u raw=%06x mask=%06x\n",
                     effective_word&3u,(effective_word>>2u)&3u,effective_word&0x00ffffffu,bp_mask);
    }
    const auto consume_intercepted_bp=[&] {
      bp_cache[reg]=effective_word;
      aurora::gx::g_gxState.bpRegValid.set(reg);
      bp_cache[0xFEu]=0x00ffffffu;
    };
    if(reg>=0x49u && reg<=0x4Eu) TrackRawCopyRegister(reg,effective_word);
    if(reg==0x52u && ((effective_word>>14)&1u)!=0u) {
      ++g_display_copy_count;
      consume_intercepted_bp();
      const bool clear=((effective_word>>11)&1u)!=0u;
      const bool latched=LatchDisplayCopyCached(effective_word,clear);
      // Aurora snapshots indexed GX arrays into GPU storage and deliberately
      // reuses that cachedRange until GXInvalidateVtxCache. Retail games also
      // commonly stream/skin geometry by rewriting the same guest RAM between
      // display copies. GekkoAOT sees those CPU writes directly, while Aurora
      // cannot, so keeping the previous cachedRange can render stale vertices
      // with an otherwise perfectly valid FIFO. GXCopyDisp is the safest GX
      // frame boundary we have: the old geometry has already been consumed by
      // the EFB->XFB copy and the following frame may now reuse the same RAM.
      if(latched && g_array_frame_refresh) RefreshIndexedArraySnapshots();
      return latched;
    }
    const bool texture_copy = reg==0x52u;
    if(texture_copy && !PrepareRawTextureCopy(effective_word)) {
      // Bad raw copy state is preferable to poisoning Aurora's copy-texture
      // cache with a null/stale destination. Continue guest execution and let
      // the next complete copy state recover naturally.
      consume_intercepted_bp();
      return true;
    }
    if(texture_copy) {
      // Aurora resolves EFB->texture copies through its render worker. Retail
      // games may issue a burst of dependent copies and immediately bind the
      // results as textures. Keep that copy boundary ordered with respect to
      // prior and following native GX work instead of allowing several copy
      // passes to overlap while the raw FIFO continues mutating GX state.
      if(!aurora::gfx::render_worker::is_worker_thread())
        aurora::gfx::render_worker::synchronize();
      static unsigned trace_before=0;
      if(trace_before++<64u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_EFB_COPY_FENCE_V78=1 phase=before dest_phys=%08x fmt=%u\n",
                     g_copy_dest_physical,unsigned(aurora::gx::g_gxState.texCopyFmt));
    }
    (void)aurora::gx::fifo::process(p,static_cast<std::uint32_t>(len));
    if(reg==0x66u) InvalidateTextureCacheV87();
    if(texture_copy) {
      if(!aurora::gfx::render_worker::is_worker_thread())
        aurora::gfx::render_worker::synchronize();
      static unsigned trace_after=0;
      if(trace_after++<64u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_EFB_COPY_FENCE_V78=1 phase=after dest_phys=%08x fmt=%u\n",
                     g_copy_dest_physical,unsigned(aurora::gx::g_gxState.texCopyFmt));
    }
    if((reg>=0x94u&&reg<=0x97u)||(reg>=0xB4u&&reg<=0xB7u)) MapTextureImage3(reg,effective_word);
    if(reg==0x65u) MapTlut();
    if(g_pe&&(reg==0x45u||reg==0x47u||reg==0x48u))
      g_pe(g_pe_user,reg,effective_word&0x00ffffffu);
    return true;
  }

  if(op>=0x80u&&op<=0xB8u) {
    if(len < 3u) return false;
    const auto fmt=static_cast<GXVtxFmt>(cmd&GX_VAT_MASK);
    const auto prim=static_cast<GXPrimitive>(cmd&GX_OPCODE_MASK);
    const std::uint16_t count=Be16(p+1);
    CaptureDraw(cmd,p,len);

    // GXBegin with zero vertices is a legal no-op in retail code.  The framed
    // command is only the 3-byte opcode/count header; do not send it through
    // the raw-draw helper, whose contract intentionally requires vertex data.
    // Treating this as a FIFO fault made harmless zero-count triangle strips
    // (notably opcode 0x98) terminate the whole native runtime.
    if(count==0u) {
      static bool zero_draw_logged=false;
      if(!zero_draw_logged) {
        zero_draw_logged=true;
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_ZERO_DRAW_V13_1=1 opcode=0x%02x action=noop\n",
                     unsigned(cmd));
      }
      return true;
    }

    if(!PrepareIndexedArrayWindows(p,len)) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_FIFO_ERROR opcode=0x%02x action=reject-array-window\n",
                   unsigned(cmd));
      return false;
    }
    if(g_z16r_copy_trace && g_z16r_bound_maps!=0u && g_z16r_draw_logs<96u) {
      std::uint8_t sampled_maps=0;
      const auto& state=aurora::gx::g_gxState;
      for(unsigned stage=0;stage<state.numTevStages && stage<aurora::gx::MaxTevStages;++stage) {
        const unsigned map=unsigned(state.tevStages[stage].texMapId);
        if(map<aurora::gx::MaxTextures)
          sampled_maps=static_cast<std::uint8_t>(sampled_maps|(1u<<map));
      }
      if((sampled_maps&g_z16r_bound_maps)!=0u) {
        ++g_z16r_draw_logs;
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_Z16R_TRACE_V1 event=draw-consumer bound=%02x sampled=%02x opcode=%02x count=%u\n",
                     g_z16r_bound_maps,sampled_maps,unsigned(cmd),unsigned(count));
      }
    }
    if(g_array_base_trace) {
      ++g_array_base_trace_draws;
      for(int i=GX_VA_POS;i<=GX_VA_TEX7;++i) {
        const auto desc=aurora::gx::g_gxState.vtxDesc[i];
        if(desc!=GX_INDEX8 && desc!=GX_INDEX16) continue;
        const auto& array=aurora::gx::g_gxState.arrays[i];
        if(array.cachedRange.size==0u && array.size!=0u) {
          ++g_array_base_trace_uploads;
          g_array_base_trace_upload_bytes+=array.size;
        }
      }
      if((g_array_base_trace_draws&0x7fffu)==0u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_ARRAY_DRAW_TRACE_V1 draws=%llu uploads=%llu upload_bytes=%llu gx_invalidate=%llu window_growth=%llu window_cached=%llu window_bytes=%llu eager_growth=%llu\n",
                     static_cast<unsigned long long>(g_array_base_trace_draws),
                     static_cast<unsigned long long>(g_array_base_trace_uploads),
                     static_cast<unsigned long long>(g_array_base_trace_upload_bytes),
                     static_cast<unsigned long long>(g_array_base_trace_invalidate),
                     static_cast<unsigned long long>(g_array_base_trace_window_growth),
                     static_cast<unsigned long long>(g_array_base_trace_window_cached),
                     static_cast<unsigned long long>(g_array_base_trace_window_bytes),
                     static_cast<unsigned long long>(g_array_base_trace_eager_growth));
    }
    if(aurora::gx::fifo::submit_raw_draw(prim,fmt,count,p+3,
                                         static_cast<std::uint32_t>(len-3u)))
      return true;

    // The direct path is an optimisation, never a compatibility requirement.
    // If its stricter framing contract rejects a non-empty draw, preserve the
    // pre-v4 behaviour by giving the complete command to Aurora's normal FIFO
    // decoder.  Only fail closed when even that decoder cannot consume it.
    const auto parsed=aurora::gx::fifo::process(p,static_cast<std::uint32_t>(len));
    if(parsed.bytesProcessed==len) {
      static bool fallback_logged=false;
      if(!fallback_logged) {
        fallback_logged=true;
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_RAW_DRAW_FALLBACK_V13_1=1 policy=aurora-parser\n");
      }
      return true;
    }
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_FIFO_ERROR opcode=0x%02x action=raw-draw-and-parser-reject\n",
                 unsigned(cmd));
    return false;
  }

  (void)aurora::gx::fifo::process(p,static_cast<std::uint32_t>(len));
  if(op==GX_LOAD_CP_REG) InvalidateVertexSize(p[1]);
  return true;
}

bool ProcessSpan(const std::uint8_t* data, std::size_t bytes, unsigned depth) {
  std::size_t off=0;
  while(off<bytes) {
    std::size_t len=0,need=1;
    const auto status=CommandLength(data+off,bytes-off,&len,&need);
    if(status!=DecodeResult::Complete) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_FIFO_ERROR offset=%zu remaining=%zu opcode=0x%02x status=%s need=%zu\n",
                   off,bytes-off,unsigned(data[off]),
                   status==DecodeResult::Incomplete?"incomplete":"invalid",need);
      return false;
    }
    if(!ProcessOne(data+off,len,depth)) return false;
    off+=len;
  }
  return true;
}

void CompactFifo() {
  if(g_fifo_read==0u) return;
  if(g_fifo_read>=g_fifo.size()) {
    g_fifo_stream_base += g_fifo.size();
    g_fifo.clear();
    g_fifo_read=0u;
    // A single unusually large display list can make std::vector retain that
    // peak forever. Return pathological FIFO capacity once the queue drains.
    constexpr std::size_t kFifoRetainedCapacity=4u*1024u*1024u;
    if(g_fifo.capacity()>kFifoRetainedCapacity) {
      std::vector<std::uint8_t> compact;
      compact.reserve(1u<<20);
      g_fifo.swap(compact);
    }
    return;
  }
  // Avoid vector::erase/memmove on every command. Compact only after a useful
  // amount of data was consumed or when more than half of the buffer is dead.
  if(g_fifo_read<65536u && g_fifo_read*2u<g_fifo.size()) return;
  const std::size_t remain=g_fifo.size()-g_fifo_read;
  g_fifo_stream_base += g_fifo_read;
  std::memmove(g_fifo.data(),g_fifo.data()+g_fifo_read,remain);
  g_fifo.resize(remain);
  g_fifo_read=0u;
}

void DrainFifo() {
  while(g_fifo_read<g_fifo.size()) {
    const std::size_t available=g_fifo.size()-g_fifo_read;
    if(available<g_fifo_need) break;
    std::size_t len=0,need=1;
    const auto status=CommandLength(g_fifo.data()+g_fifo_read,available,&len,&need);
    if(status==DecodeResult::Incomplete) {
      g_fifo_need=need;
      break;
    }
    if(status==DecodeResult::Invalid) {
      DumpFifoFault(g_fifo_read);
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_FIFO_ERROR offset=%zu opcode=0x%02x action=fail-closed\n",
                   g_fifo_read,unsigned(g_fifo[g_fifo_read]));
      g_fifo_fault=true;
      g_quit=true;
      break;
    }
    RememberDecodedCommand(g_fifo.data()+g_fifo_read,len,g_fifo_stream_base+g_fifo_read);
    if(!ProcessOne(g_fifo.data()+g_fifo_read,len,0u)) {
      DumpFifoFault(g_fifo_read);
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_FIFO_ERROR offset=%zu opcode=0x%02x action=fail-closed-process\n",
                   g_fifo_read,unsigned(g_fifo[g_fifo_read]));
      g_fifo_fault=true;
      g_quit=true;
      break;
    }
    g_fifo_read+=len;
    g_fifo_need=1u;
  }
  CompactFifo();
}

bool RuntimeWindowIsEmbedded() {
  const char* parent = std::getenv("GEKKOAOT_EMBED_WINDOW");
  return parent && *parent;
}

void PollFullscreenShortcut() {
  // The Qt frontend has its own Alt+Enter shortcut, but match-GDB/standalone
  // mode gives keyboard focus to Aurora's SDL window. Handle the shortcut in
  // the NativeGX DSO as well. Embedded X11 windows deliberately leave
  // fullscreen ownership to the Qt parent.
  if (!g_info.window || RuntimeWindowIsEmbedded()) {
    g_alt_enter_latched = false;
    return;
  }
  int key_count = 0;
  const bool* keys = SDL_GetKeyboardState(&key_count);
  const SDL_Keymod mods = SDL_GetModState();
  const bool return_down = keys &&
      ((SDL_SCANCODE_RETURN < key_count && keys[SDL_SCANCODE_RETURN]) ||
       (SDL_SCANCODE_KP_ENTER < key_count && keys[SDL_SCANCODE_KP_ENTER]));
  const bool chord = return_down && (mods & SDL_KMOD_ALT) != 0;
  if (chord && !g_alt_enter_latched) {
    const bool was_fullscreen =
        (SDL_GetWindowFlags(g_info.window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (SDL_SetWindowFullscreen(g_info.window, !was_fullscreen)) {
      // SDL3 window-state changes can be asynchronous (notably Wayland/X11).
      // Synchronize this explicit user action so the very next present sees
      // the new framebuffer size, then refresh the live projection policy.
      (void)SDL_SyncWindow(g_info.window);
      ApplyAspectMode(g_aspect_mode.load(std::memory_order_acquire), false);
      std::fprintf(stderr,
                   "GEKKOAOT_FULLSCREEN_V137=1 source=alt-enter state=%s mode=standalone\n",
                   was_fullscreen ? "windowed" : "fullscreen");
    } else {
      std::fprintf(stderr,
                   "GEKKOAOT_FULLSCREEN_V137=0 source=alt-enter error=%s\n",
                   SDL_GetError());
    }
  }
  g_alt_enter_latched = chord;
}

void PollEvents() {
  const AuroraEvent* ev=aurora_update();
  while(ev && ev->type!=AURORA_NONE) { if(ev->type==AURORA_EXIT) g_quit=true; ++ev; }
  PollFullscreenShortcut();
}

#ifdef GEKKOAOT_HAVE_X11
bool EmbedX11() {
  const char* parent_text=std::getenv("GEKKOAOT_EMBED_WINDOW");
  if(!parent_text||!*parent_text) return false;
  if(!g_info.window) {
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_SINGLE_WINDOW_V001=0 reason=no-sdl-window\n");
    return false;
  }
  char* end=nullptr; const unsigned long parent=std::strtoul(parent_text,&end,0);
  if(end==parent_text||*end!='\0'||!parent) {
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_SINGLE_WINDOW_V001=0 reason=bad-parent-xid\n");
    return false;
  }
  SDL_PropertiesID props=SDL_GetWindowProperties(g_info.window);
  auto* display=static_cast<Display*>(SDL_GetPointerProperty(props,SDL_PROP_WINDOW_X11_DISPLAY_POINTER,nullptr));
  const Window child=static_cast<Window>(SDL_GetNumberProperty(props,SDL_PROP_WINDOW_X11_WINDOW_NUMBER,0));
  if(!display||!child) {
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_SINGLE_WINDOW_V001=0 reason=sdl-not-x11\n");
    return false;
  }
  if(child==static_cast<Window>(parent)) {
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_SINGLE_WINDOW_V001=1 platform=x11 mode=sdl-external xid=%lu\n", parent);
    return true;
  }

  // Fallback for an already-built Aurora cache that has not yet picked up the
  // SDL external-window adapter. Keep it deterministic and resize to GameView.
  XWindowAttributes attrs{};
  if(!XGetWindowAttributes(display,static_cast<Window>(parent),&attrs)) {
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_SINGLE_WINDOW_V001=0 reason=parent-query-failed\n");
    return false;
  }
  XSetWindowBorderWidth(display,child,0);
  XReparentWindow(display,child,static_cast<Window>(parent),0,0);
  XMoveResizeWindow(display,child,0,0,
                    static_cast<unsigned>(std::max(attrs.width,1)),
                    static_cast<unsigned>(std::max(attrs.height,1)));
  XMapWindow(display,child);
  XSync(display,False);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_SINGLE_WINDOW_V001=1 platform=x11 mode=reparent-fallback xid=%lu\n", parent);
  return true;
}

bool PrepareX11Embed() {
  const char* parent = std::getenv("GEKKOAOT_EMBED_WINDOW");
  if (!parent || !*parent) return true;

  const char* window_system = std::getenv("GEKKOAOT_WINDOW_SYSTEM");
  if (window_system && *window_system && !EqualNoCase(window_system, "x11")) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_X11_PREP_V001=0 reason=window-system-%s\n",
                 window_system);
    return false;
  }

  if (::setenv("GEKKOAOT_NATIVE_GX_X11_WINDOW", parent, 1) != 0) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_X11_PREP_V001=0 reason=setenv-xid-failed\n");
    return false;
  }

  // SDL input has initialized EVENTS/GAMEPAD, not SDL_VIDEO. Force X11 before
  // Aurora creates its presentation window so SDL wraps the Qt/XCB GameView
  // instead of opening a separate Wayland top-level.
  if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0) {
    ::setenv("SDL_VIDEODRIVER", "x11", 1);
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "x11", SDL_HINT_OVERRIDE);
  } else {
    const char* driver = SDL_GetCurrentVideoDriver();
    if (!driver || !EqualNoCase(driver, "x11")) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_X11_PREP_V001=0 reason=video-already-%s\n",
                   driver ? driver : "unknown");
      return false;
    }
  }

  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_X11_PREP_V001=1 driver=x11 xid=%s\n", parent);
  return true;
}
#endif
}

GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_init(HostResolveFn resolve, void* user) {
  if(g_ready) return true;
  g_resolve=resolve; g_resolve_user=user; g_fifo.reserve(1u<<20);
  g_fifo_read=0; g_fifo_need=1; g_fifo_fault=false; g_fifo_fault_reported=false;
  g_fifo_write_trace.fill({}); g_fifo_write_trace_head=0; g_fifo_write_trace_count=0;
  g_fifo_last_cmd=0; g_fifo_last_cmd_len=0; g_fifo_last_draw_fmt=0xffu;
  g_fifo_last_draw_count=0; g_fifo_last_draw_stride=0;
  g_vertex_size_cache.fill(0); g_vertex_size_valid=0;
  g_index_scan_layout.fill({}); g_index_scan_valid=0;
  g_texture_image3_word.fill(0); g_texture_image3_valid=0;
  g_array_available.fill(0);
  g_array_guest_base.fill(0);
  g_array_cache_control_events=0;
  g_array_cache_invalidations=0;
  g_array_cache_control_logs=0;
  g_copy_dest_physical=0; g_copy_stride_bytes=0; g_copy_yscale_raw=0x100u; g_copy_src_valid=false; g_copy_dest_valid=false;
  g_frame_ready_pending=false; g_frame_ready_xfb=0;
  g_alt_enter_latched=false;
  g_frame_interpolation_enabled=EnvBool("GEKKOAOT_FRAME_INTERPOLATION",true);
  g_interp_previous={}; g_interp_current={};
  g_interp_previous_valid=false; g_interp_current_valid=false; g_interp_snapshot_count=0;
  g_texture_bind_cache=EnvBool("GEKKOAOT_NATIVE_GX_FIFO_TEXTURE_CACHE",true);
  g_array_base_trace=EnvBool("GEKKOAOT_NATIVE_GX_ARRAY_BASE_TRACE",false);
  g_z16r_copy_trace=EnvBool("GEKKOAOT_NATIVE_GX_Z16R_TRACE",false);
  g_ztexture_trace=EnvBool("GEKKOAOT_NATIVE_GX_ZTEXTURE_TRACE",false);
  g_ztexture_logs=0;
  g_z16r_copy_dest=0; g_z16r_copy_span=0; g_z16r_bound_maps=0;
  g_z16r_bp_logs=0; g_z16r_copy_logs=0; g_z16r_bind_logs=0; g_z16r_draw_logs=0;
  g_array_base_writes=0;
  g_array_base_same_binding=0;
  g_array_base_same_cached=0;
  g_array_base_same_cached_bytes=0;
  g_array_base_trace_draws=0;
  g_array_base_trace_uploads=0;
  g_array_base_trace_upload_bytes=0;
  g_array_base_trace_invalidate=0;
  g_array_base_trace_window_growth=0;
  g_array_base_trace_window_cached=0;
  g_array_base_trace_window_bytes=0;
  g_array_base_trace_eager_growth=0;
  // v115: v114 proved the narrow per-draw indexed-array refresh does not repair
  // MKDD's black 3D scene, while it greatly increases staging traffic. Keep both
  // expensive coherency policies opt-in. The active correctness fix in v115 is
  // Aurora Z-texture (BP F4/F5 + Z8/Z16/Z24X8 sampling + fragment depth).
  g_array_frame_refresh=EnvBool("GEKKOAOT_NATIVE_GX_ARRAY_FRAME_REFRESH",false);
  g_indexed_draw_coherency=EnvBool("GEKKOAOT_NATIVE_GX_INDEXED_DRAW_COHERENCY",false);
  AuroraConfig config{};
  config.appName="GekkoAOT " GEKKOAOT_VERSION;
  config.desiredBackend=BackendFromEnv();
  config.vsync=EnvBool("GEKKOAOT_NATIVE_GX_VSYNC",false);
  config.startFullscreen=EnvBool("GEKKOAOT_NATIVE_GX_FULLSCREEN",false);
  config.windowWidth=EnvUnsigned("GEKKOAOT_NATIVE_GX_WIDTH",1280);
  config.windowHeight=EnvUnsigned("GEKKOAOT_NATIVE_GX_HEIGHT",720);
  config.pauseOnFocusLost=false;
  config.allowJoystickBackgroundEvents=true;
  config.mem1Size=0; config.mem2Size=0;
#ifdef GEKKOAOT_HAVE_X11
  if (!PrepareX11Embed()) {
    std::fprintf(stderr,"GEKKOAOT_AURORA_GX_V001=0 reason=x11-prep-failed\n");
    return false;
  }
#endif
  int argc=1; char arg0[]="gekkoaot"; char* argv[]={arg0,nullptr};
  g_info=aurora_initialize(argc,argv,&config);
  for(auto& slot : g_xfb_slots) slot = {};
  g_xfb_generation=0;
  g_ram_xfb_texture.reset();
  g_ram_xfb_present={};
  g_ram_xfb_rgba.clear();
  g_ram_xfb_width=0u; g_ram_xfb_height=0u; g_ram_xfb_generation=0u;
  for (auto& candidate : g_ram_xfb_candidates) candidate = {};
  g_ram_xfb_candidate_epoch=0u;
  const char* aspect=std::getenv("GEKKOAOT_ASPECT_MODE");
  ApplyAspectMode(AspectModeFromText(aspect), true);
  g_ready=true;
#ifdef GEKKOAOT_HAVE_X11
  const char* embed = std::getenv("GEKKOAOT_EMBED_WINDOW");
  if(embed && *embed && !EmbedX11()) {
    std::fprintf(stderr,"GEKKOAOT_AURORA_GX_V001=0 reason=single-window-embed-failed\n");
    aurora_shutdown();
    g_ready=false;
    return false;
  }
#endif
  PollEvents();
  if (!g_quit && aurora_begin_frame()) g_frame_open=true;
  std::fprintf(stderr,"GEKKOAOT_AURORA_GX_V001=1 backend=direct-retail-fifo vi=native xfb=vi-selected-aurora-copy-cache-v12+ram-yuvy-v57\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_RAM_XFB_V57=1 mode=vi-geometry-yuvy-fallback fastpath=aurora-zero-copy\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_RAM_XFB_V82=1 detector=per-xfb-history slots=16 arm=3-observations+2-changes fallback=cache-miss-only\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_XFB_PRESENT_BOUNDARY_V110=1 producer=gx-copy submit=gx-copy scanout=present-only ram-upload=direct-queue live-efb-cut=forbidden\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_PERF_RECOVERY_V115=1 array-frame-refresh-default=off indexed-draw-coherency-default=off ztexture=aurora-v1 xfb-boundary=v110\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_INDEXED_ARRAY_COHERENCY_V114=1 policy=exact-used-windows+no-cross-draw-merge default=off env=GEKKOAOT_NATIVE_GX_INDEXED_DRAW_COHERENCY\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_ZTEXTURE_V115=1 backend=aurora-bp-f4-f5 trace=%u env=GEKKOAOT_NATIVE_GX_ZTEXTURE_TRACE\n",g_ztexture_trace?1u:0u);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_INDEX_SCAN_V2=1 layout=cached-fields nbt3=xyz-element\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_XF_ARRAY_WINDOW_V86=1 arrays=pos+normal+texture+light sizing=per-indexed-load bounds=guest-mapping\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_COHERENCY_V87=1 texture-invalidate=bp66 stale-array=fail-closed stale-texture=fail-closed dl-depth=1\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_TEXTURE_ID_V10=1 identity=stable-per-slot version=per-load\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_ARRAY_FRAME_REFRESH_V3=%u policy=%s\n",
               g_array_frame_refresh?1u:0u,
               g_array_frame_refresh?"gx-copy+host-frame-boundary":"gx-invalidate-driven");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_INDEXED_DRAW_COHERENCY_V109=%u policy=%s\n",
               g_indexed_draw_coherency?1u:0u,
               g_indexed_draw_coherency?"refresh-used-arrays+forbid-cross-draw-merge":"legacy-cache-reuse");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_DYNAMIC_VERTEX_V141=1 source=dcbst+dcbf+dcbi line=32 target=indexed-arrays policy=invalidate-overlap-no-per-draw-hash\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_EFB_COPY_V2=1 mode=retail-bp-state\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_EFB_COPY_FENCE_V78=1 ordering=render-worker-before+after trace=64\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_FIFO_CAP_V11=1 bytes=%u policy=fail-closed\n",
               16u*1024u*1024u);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_FIFO_BURST_V13=1 abi=optional max=4096\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_PRESENT_V137=1 interpolation-aspect=live-window fullscreen=alt-enter-standalone\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_WIDESCREEN_V156C=1 producer=gc-fit scanout=window-wide "
               "wide-interpolation=%u override=GEKKOAOT_WIDESCREEN_INTERPOLATION\n",
               g_frame_interpolation_enabled ? 1u : 0u);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_WIDESCREEN_V157=1 projection=perspective-only "
               "orthographic=retail-preserved postprocess=safe\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_WIDESCREEN_V158=1 projection=perspective+ui-ortho "
               "ortho-efb-copy=retail-preserved interpolation=%u\n",
               g_frame_interpolation_enabled ? 1u : 0u);
  return true;
}

namespace {

bool AppendNativeFifoBytes(const std::uint8_t* bytes, std::size_t size,
                           std::uint32_t guest_pc) {
  if(!g_ready || g_fifo_fault || g_quit || !bytes || size==0u) return false;
  // WGPIPE is a stream, not an unbounded command archive. If framing stops
  // making progress, fail closed before std::vector can grow into GiB of host
  // memory. This limit is deliberately far above any sensible in-flight FIFO.
  constexpr std::size_t kMaxBufferedFifo = 16u * 1024u * 1024u;
  if(size > kMaxBufferedFifo || g_fifo.size() > kMaxBufferedFifo - size) {
    const std::size_t unread = g_fifo_read <= g_fifo.size() ? g_fifo.size()-g_fifo_read : 0u;
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_FIFO_ERROR buffered=%zu unread=%zu capacity=%zu incoming=%zu action=fail-closed-overflow\n",
                 g_fifo.size(), unread, g_fifo.capacity(), size);
    g_fifo_fault=true;
    g_quit=true;
    return false;
  }
  const std::size_t old=g_fifo.size();
  g_fifo.insert(g_fifo.end(), bytes, bytes+size);
  // One forensic record per host burst is enough to retain the producer PC
  // while keeping the fast path independent from guest-store granularity.
  RememberFifoWrite(0u, static_cast<std::uint8_t>(std::min<std::size_t>(size,255u)),
                    g_fifo_stream_base+old, guest_pc);
  if(g_fifo.size()-g_fifo_read>=g_fifo_need) DrainFifo();
  return !g_fifo_fault;
}

} // namespace

// Aurora shader/presentation overlays query these live values. Keeping aspect
// outside ShaderConfig means resizing/switching modes only rebuilds uniforms,
// not GX pipelines or shaders.
GEKKOAOT_GX_EXPORT float gekkoaot_native_gx_widescreen_x_scale() noexcept {
  return ProjectionXScaleForMode(g_aspect_mode.load(std::memory_order_acquire));
}
GEKKOAOT_GX_EXPORT float gekkoaot_native_gx_widescreen_target_aspect() noexcept {
  return TargetAspectForMode(g_aspect_mode.load(std::memory_order_acquire));
}
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_set_aspect_mode(std::uint32_t mode) {
  ApplyAspectMode(mode, true);
}

GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_set_pe_callback(HostPeEventFn fn, void* user) { g_pe=fn; g_pe_user=user; }
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_write(std::uint64_t value, std::uint8_t size,
                                                  std::uint32_t guest_pc) {
  if(size!=1&&size!=2&&size!=4&&size!=8) return;
  std::uint8_t bytes[8]{};
  for(std::uint8_t i=0;i<size;++i)
    bytes[i]=static_cast<std::uint8_t>(value>>((size-1u-i)*8u));
  (void)AppendNativeFifoBytes(bytes,size,guest_pc);
}
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_write_burst(const std::uint8_t* bytes,
                                                        std::uint32_t size,
                                                        std::uint32_t guest_pc) {
  if(size==0u || size>4096u) return;
  (void)AppendNativeFifoBytes(bytes,size,guest_pc);
}
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_cache_control(std::uint8_t operation,
                                                         std::uint32_t address) {
  if(!g_ready || g_fifo_fault || g_quit) return;
  NotifyGuestVertexCacheControl(operation,address);
}
// v162: CPU-visible EFB Z aperture for retail GXPeekZ loads. Aurora's own
// GXCpu2Efb implementation is intentionally asynchronous: use the newest
// completed depth snapshot, return 0 before the first snapshot exists, and
// request another snapshot for the next observation. Returning true means the
// CPU access was handled even when the bootstrap value is zero.
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_peek_z(std::uint16_t x, std::uint16_t y,
                                                   std::uint32_t* z) {
  if(!g_ready || g_fifo_fault || g_quit || z==nullptr) return false;
  std::uint32_t value=0u;
  const bool valid=aurora::gfx::depth_peek::read_latest(x,y,value);
  aurora::gfx::depth_peek::request_snapshot();
  *z=valid ? (value & 0x00ffffffu) : 0u;
  return true;
}
// v163: matching CPU-visible EFB color aperture for GXPeekARGB. This mirrors
// the asynchronous depth path: consume the newest completed logical-frame
// snapshot, bootstrap with zero, then request a fresher snapshot.
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_peek_argb(std::uint16_t x, std::uint16_t y,
                                                      std::uint32_t* argb) {
  if(!g_ready || g_fifo_fault || g_quit || argb==nullptr) return false;
  std::uint32_t value=0u;
  const bool valid=aurora::gfx::color_peek::read_latest(x,y,value);
  aurora::gfx::color_peek::request_snapshot();
  std::uint32_t color=valid ? value : 0u;
  // Match Flipper's EFB storage precision before the PE alpha-read override.
  // This is the same quantization Dolphin applies to CPU EFB color peeks.
  if(aurora::gx::g_gxState.pixelFmt==GX_PF_RGBA6_Z24) {
    color &= 0xfcfcfcfcu;
    color |= (color >> 6u) & 0x03030303u;
  } else if(aurora::gx::g_gxState.pixelFmt==GX_PF_RGB565_Z16) {
    color &= 0x00f8fcf8u;
    color |= (color >> 5u) & 0x00070007u;
    color |= (color >> 6u) & 0x00000300u;
    color |= 0xff000000u;
  } else {
    color |= 0xff000000u;
  }
  *argb=color;
  return true;
}
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_present() {
  PresentViXfb(0u,0u,false);
}
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_present_xfb(std::uint32_t top,
                                                        std::uint32_t bottom) {
  PresentViXfb(top,bottom,true);
}
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_present_xfb_ex(
    std::uint32_t top, std::uint32_t bottom, std::uint32_t width,
    std::uint32_t stride_bytes, std::uint32_t field_height) {
  PresentViXfb(top,bottom,true,width,stride_bytes,field_height);
}
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_consume_frame_ready(std::uint32_t* xfb_address) {
  if(!g_ready || !g_frame_ready_pending) return false;
  if(xfb_address) *xfb_address=g_frame_ready_xfb;
  g_frame_ready_pending=false;
  return true;
}
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_frame_interpolation_ready() {
  return g_ready && g_frame_interpolation_enabled && g_interp_previous_valid && g_interp_current_valid;
}
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_present_interpolated(float alpha) {
  if(!g_ready || !g_frame_interpolation_enabled || !g_interp_previous_valid || !g_interp_current_valid)
    return false;
  const bool presented =
      PresentInterpolationBlend(g_interp_previous, g_interp_current, std::clamp(alpha,0.0f,1.0f));
  PollEvents();
  return presented;
}
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_should_quit(){ PollEvents(); return g_quit; }
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_shutdown(){
  if(!g_ready) return;
  // Stop accepting host work first, then finish the producer-owned frame while
  // Aurora's render worker and WebGPU device are still alive. The host keeps
  // this DSO mapped until process exit so spontaneous Dawn cancellation
  // callbacks can never return into unmapped plugin code.
  g_ready=false;
  g_quit=true;
  if(!g_fifo_fault) DrainFifo();
  if(g_frame_open) {
    aurora_end_frame_no_present();
    g_frame_open=false;
  }
  PollEvents();
  g_pe=nullptr;
  g_pe_user=nullptr;
  for(auto& slot : g_xfb_slots) slot = {};
  g_xfb_generation=0;
  g_ram_xfb_texture.reset();
  g_ram_xfb_present={};
  g_ram_xfb_rgba.clear();
  g_ram_xfb_width=0u; g_ram_xfb_height=0u; g_ram_xfb_generation=0u;
  for (auto& candidate : g_ram_xfb_candidates) candidate = {};
  g_ram_xfb_candidate_epoch=0u;
  g_interp_previous={}; g_interp_current={};
  g_interp_previous_valid=false; g_interp_current_valid=false;
  aurora_shutdown();
  g_resolve=nullptr;
  g_resolve_user=nullptr;
  if(g_array_base_trace)
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_ARRAY_BASE_TRACE_V1 writes=%llu same_binding=%llu same_cached=%llu discarded_cached_bytes=%llu phase=shutdown\n",
                 static_cast<unsigned long long>(g_array_base_writes),
                 static_cast<unsigned long long>(g_array_base_same_binding),
                 static_cast<unsigned long long>(g_array_base_same_cached),
                 static_cast<unsigned long long>(g_array_base_same_cached_bytes));
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_DYNAMIC_VERTEX_V141_STATS cache_events=%llu array_invalidations=%llu\n",
               static_cast<unsigned long long>(g_array_cache_control_events),
               static_cast<unsigned long long>(g_array_cache_invalidations));
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_SHUTDOWN_V12=1 order=drain-endframe-events-aurora dso=pinned\n");
  g_fifo.clear();
  g_fifo_read=0;
  g_fifo_need=1;
  g_fifo_fault=false;
  g_fifo_fault_reported=false;
  g_fifo_write_trace.fill({});
  g_fifo_write_trace_head=0;
  g_fifo_write_trace_count=0;
  g_fifo_command_trace.fill({});
  g_fifo_command_trace_head=0;
  g_fifo_command_trace_count=0;
  g_fifo_stream_base=0;
  g_fifo_last_cmd=0;
  g_fifo_last_cmd_len=0;
  g_fifo_last_draw_fmt=0xffu;
  g_fifo_last_draw_count=0;
  g_fifo_last_draw_stride=0;
  g_vertex_size_cache.fill(0);
  g_vertex_size_valid=0;
  g_index_scan_layout.fill({});
  g_index_scan_valid=0;
  g_texture_image3_word.fill(0);
  g_texture_image3_valid=0;
  g_array_frame_refresh=false;
  g_array_available.fill(0);
  g_array_guest_base.fill(0);
  g_array_cache_control_events=0;
  g_array_cache_invalidations=0;
  g_array_cache_control_logs=0;
  g_copy_dest_physical=0;
  g_copy_stride_bytes=0;
  g_copy_yscale_raw=0x100u;
  g_copy_src_valid=false;
  g_copy_dest_valid=false;
  g_ready=false;
  g_quit=true;
  g_frame_open=false;
  for(auto& slot : g_xfb_slots) slot = {};
  g_xfb_generation=0;
  g_display_copy_count=0;
  g_interp_previous={}; g_interp_current={};
  g_interp_previous_valid=false; g_interp_current_valid=false; g_interp_snapshot_count=0;
  g_fifo_fault=false;
  g_resolve=nullptr;
  g_resolve_user=nullptr;
  g_pe=nullptr;
  g_pe_user=nullptr;
}
