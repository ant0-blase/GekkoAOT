// SPDX-License-Identifier: GPL-3.0-or-later
// GekkoAOT direct retail GX -> Aurora bridge. No emulator runtime.
#include <aurora/aurora.h>
#include "../native_index_range_cache.h"
#include "../native_attribute_plan.h"
#include "../native_vertex_program.h"
#include "../native_vertex_gather.h"
#include "../native_command_stream.h"
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx/GXCommandList.h>
#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXAurora.h>
#include "gx/command_processor.hpp"
#include "gx/regs.hpp"
#include "gx/gx.hpp"
#include "gx/texture.hpp"
#include "gfx/render_worker.hpp"
#include "gfx/clear.hpp"
#include "gfx/recording.hpp"
#include "gfx/color_peek.hpp"
#include "gfx/depth_peek.hpp"
#include "gfx/frame.hpp"
#include "gfx/texture.hpp"
#include "dolphin/vi/vi_internal.hpp"
#include "webgpu/gpu.hpp"
#include <SDL3/SDL.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "window.hpp"
#include "../host/tlut_memory.h"
#include "bounding_box.h"
#include "../graphics_settings.h"

#include <algorithm>
#include "../widescreen_policy.h"
#include "../frame_replay_policy.h"
#include "perf_hud.h"
#include "../array_upload_policy.h"
#include "../compact_array_window.h"
#include "../cached_scanout.h"
#include "interpolation_shader.h"
#include <atomic>
#include <cctype>
#include <cstddef>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>

#ifndef GEKKOAOT_VERSION
#define GEKKOAOT_VERSION "dev"
#endif
#include <string>
#include <vector>
#include <unordered_map>
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
// v363: pre-rendered scenes (a full-screen backdrop writing depth from a
// texture: Resident Evil rooms) hold no image beyond 4:3. While one is shown,
// frames are rendered and presented at the original 4:3 instead of
// stretching the backdrop under hor+ characters.
bool g_wide_prerendered_43 = true;
bool g_frame_prerendered = false;
unsigned g_prerendered_idle = 0;
std::atomic<bool> g_frame_force_43{false};
bool g_present_force_43 = false; // render worker: the presented slot was recorded at 4:3
bool g_recorded_force_43 = false; // mode of the frame the current display copy ends
// Aspect mode the current frame records with.
std::uint32_t RecordAspectMode() noexcept {
  return g_frame_force_43.load(std::memory_order_relaxed) ? 0u : g_aspect_mode.load(std::memory_order_relaxed);
}
bool g_alt_enter_latched = false;
bool g_screen_fill_draw = false;
// v345: logical width of the last GXCopyDisp (the frame the VI scans out).
std::atomic<std::uint32_t> g_display_copy_width{640u};
// v345: ProjectionAspect normalizes a viewport against a 640-wide 4:3 display.
// Titles with a narrower EFB (Nemo: 512x480, stretched to the full 4:3 screen
// by the VI scaler) render a 4:3 camera into it; measure the viewport against
// the width of the frame the VI actually scans out (the last GXCopyDisp).
float DisplayViewWidth(float width) noexcept {
  const float display = static_cast<float>(g_display_copy_width.load(std::memory_order_relaxed));
  return display >= 320.f && display < 636.f && width <= display * 1.02f ? width * 640.f / display : width;
}

// v348: HUD insets (viewport/scissor spans clear of both canvas edges) follow
// the UI transform once a frame shows them without copying them to a texture.
bool g_ui_insets = true;
GekkoAOT::GX::DisplayInsets g_display_insets;
// v354: screen-derived EFB copies (bloom/blur pyramids) keep the retail width.
bool g_copy_provenance_enabled = true;
bool g_wide_perspective_fills = true;
// v362: perspective cards of a 2D screen (no depth-writing 3D for a few
// frames) get the UI scissor, so off-screen menu elements stay hidden.
bool g_wide_perspective_ui = true;
GekkoAOT::GX::FlatFrameHistory g_flat_frames;
unsigned g_frame_depth_draws = 0;
bool g_draw_perspective_ui = false;
// FPS-unlock feasibility probe (GEKKOAOT_INTERP_PROBE=1): how many draws of a
// guest frame find a structural partner in the previous frame, and how many of
// those changed matrices (what a replay renderer would interpolate).
struct InterpProbe {
  std::vector<std::pair<std::uint64_t,std::uint64_t>> current, previous;
  std::uint64_t frames=0, draws=0, matched=0, moved=0;
};
bool g_interp_probe_enabled = false;
InterpProbe g_interp_probe;
// v358 FPS unlock: frame interpolation re-renders the last guest frame's real
// geometry at host present times, with its uniforms extrapolated from the
// frame before. Blocks are keyed by the draw that built them; a frame is
// replayed only while Aurora still holds its encoded passes.
struct ReplayBlock {
  std::uint64_t key=0;
  std::uint32_t offset=0, size=0, proj=0, data=0, pnmtx=0;
};
struct ReplayCapture {
  std::uint64_t frame_id=0;
  std::vector<ReplayBlock> blocks;
  std::vector<std::uint8_t> bytes;
  std::unordered_map<std::uint64_t,std::uint32_t> index;
  std::chrono::steady_clock::time_point produced{};
  aurora::webgpu::TextureWithSampler present;
  bool eligible=false;
};
bool g_replay_mode = true;
// Default: show poses between the previous and the newest guest frame (no
// overshoot, one guest frame of latency). GEKKOAOT_REPLAY_PREDICT=1 instead
// predicts past the newest frame (no added latency, overshoots on sudden
// direction changes).
bool g_replay_interpolate = true;
bool g_vsync_default = false;
// Steady-clock nanoseconds of the last swapchain present (render worker).
std::atomic<std::int64_t> g_last_present_ns{0};
// Present pacing over the current log window (render worker writes, the GX
// thread reads approximately for the 5 s log).
struct PresentPacing { double sum=0.0, sum_sq=0.0, worst=0.0; std::uint64_t count=0; };
PresentPacing g_present_pacing;
std::atomic<bool> g_replay_capture_on{false};
ReplayCapture g_replay_building;
std::unordered_map<std::uint64_t,std::uint32_t> g_replay_occurrences;
std::uint64_t g_replay_draw_sig = 0;
std::uint32_t g_replay_draw_pnmtx = 0;
bool g_replay_frame_bbox = false;
std::mutex g_replay_mutex;
std::shared_ptr<const ReplayCapture> g_replay_current, g_replay_previous;
std::atomic<bool> g_replay_busy{false};
// Diagnostics: captures/eligible (GX thread), requests and declines (host thread).
std::atomic<std::uint64_t> g_replay_captures{0}, g_replay_eligible{0}, g_replay_requests{0},
    g_replay_declined_early{0}, g_replay_declined_period{0}, g_replay_declined_busy{0}, g_replay_queued{0};
struct ReplayStats {
  std::uint64_t replays=0, skipped=0, blocks=0, matched=0, moved=0, cuts=0, frame_cuts=0;
  double alpha=0.0;
};
ReplayStats g_replay_stats; // render worker only
GekkoAOT::GX::CopyProvenance g_copy_provenance;
// Logical width of the canvas the VI scans out (Nemo: a 512-wide EFB).
float DisplayCanvasWidth() noexcept {
  const float display = static_cast<float>(g_display_copy_width.load(std::memory_order_relaxed));
  return display >= 320.f && display < 636.f ? display
                                             : static_cast<float>(aurora::vi::configured_fb_size().x);
}

// v343: untextured full-width orthographic spans also bypass the UI x-scale.
bool g_wide_solid_spans = true;
unsigned g_wide_span_logs = 0;
bool g_wide_span_trace = false;
constexpr unsigned kMaxSpanVertices = 24;
AuroraInfo g_info{};
std::vector<std::uint8_t> g_fifo;
std::size_t g_fifo_read = 0;
std::size_t g_fifo_need = 1;
GekkoAOT::GX::NativeCommandStream g_native_commands;
bool g_typed_arrays = false;
bool g_direct_matrices = false;
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
  std::uint8_t has_index8 = 0;  // v336: skip the eager INDEX8 pass when none
  std::array<IndexScanField, 12> fields{};
  GekkoAOT::GX::NativeAttributePlan bindings{};
  GekkoAOT::GX::NativeVertexProgram native{};
  GekkoAOT::GX::NativeVertexGather gather{};
};
std::array<IndexScanLayout, 8> g_index_scan_layout{};
std::uint8_t g_index_scan_valid = 0;
GekkoAOT::GX::NativeIndexRangeCache g_native_index_ranges;
bool g_native_index_range_cache=false;
bool g_native_attribute_plan=true;
bool g_native_vertex_program=false;
bool g_native_vertex_gather=false;
std::uint64_t g_native_gather_draws=0,g_native_gather_bytes=0,g_native_gather_windows=0;
GekkoAOT::GX::NativeVertexGather::Workspace g_native_gather_workspace;
bool g_native_vertex_profile=false;
std::uint64_t g_native_vertex_draws=0, g_native_vertex_vertices=0;
std::array<std::uint32_t, 8> g_texture_image3_word{};
std::array<std::uint32_t, 8> g_texture_mapped_image0{};
std::array<std::uint32_t, 8> g_texture_mapped_mips{};
std::uint8_t g_texture_image3_written = 0;
std::array<std::uint32_t, GX_VA_MAX_ATTR> g_array_available{};
// v141: retain the authoritative guest base separately from Aurora's host
// pointer/visible byte window. CPU D-cache maintenance publishes dynamic
// vertex data to GX; use that boundary to invalidate only overlapping GPU
// snapshots instead of hashing vertex arrays on every draw.
std::array<std::uint32_t, GX_VA_MAX_ATTR> g_array_guest_base{};

// v161h: persistent GPU-side snapshots for indexed GX arrays.
struct PersistentArraySlot {
  std::uint32_t offset = 0;
  std::uint32_t capacity = 0;
  const void* identity = nullptr;
  std::uint32_t last_frame = UINT32_MAX;
  std::shared_ptr<const std::uint8_t[]> snapshot;
  std::uint32_t snapshot_size = 0;
};
std::array<PersistentArraySlot, GX_VA_MAX_ATTR> g_persistent_array_slots{};
std::array<GekkoAOT::GX::ArrayUploadPolicy, GX_VA_MAX_ATTR> g_array_upload_policy{};
bool g_adaptive_array_upload = true;
bool g_content_array_cache = true;
std::uint64_t g_content_array_hits = 0;
std::uint32_t g_persistent_array_cursor = 0;
std::uint32_t g_persistent_array_frame = UINT32_MAX;
bool g_persistent_array_reset_pending = false;
bool g_persistent_array_cache = true;
std::uint32_t g_persistent_array_min_bytes = 8u * 1024u;
std::uint64_t g_persistent_array_uploads = 0;
std::uint64_t g_persistent_array_upload_bytes = 0;
std::uint64_t g_persistent_array_hits = 0;
std::uint64_t g_persistent_array_fallbacks = 0;
GekkoAOT::GX::CachedScanout g_cached_scanout;
std::uint64_t g_adaptive_array_fallbacks = 0;
bool g_compact_array_windows = false;
bool g_native_array_tiles = false;
bool g_native_array_snapshots = false;
std::uint64_t g_native_array_snapshot_stages = 0;
std::uint64_t g_compact_array_uploads = 0;
std::uint64_t g_compact_array_bytes = 0;
std::uint64_t g_compact_array_prefix_bytes = 0;
std::uint64_t g_compact_array_rollovers = 0;
std::uint64_t g_compact_array_fallbacks = 0;
using CompactArrayPlan = std::array<GekkoAOT::GX::CompactArrayWindow, GX_VA_MAX_ATTR>;

std::uint64_t g_array_cache_control_events = 0;
std::uint64_t g_array_cache_invalidations = 0;
unsigned g_array_cache_control_logs = 0;
std::uint8_t g_texture_image3_valid = 0;
std::uint64_t g_texture_generation = 1;
// v174: Terminator 3 needs in-place font/UI texture changes to invalidate a
// stable IMAGE3 binding, but forcing a new texture version on every low-bit
// format rebind also hits the bitmap-heavy Monkey Ball render path. Remember
// the source bytes instead and only break the stable-bind fast path on a real
// content change.
std::array<std::uint64_t, 8> g_texture_ui_source_hash{};
std::uint8_t g_texture_ui_source_hash_valid = 0;
// v159: retail games issue many draws between texture-state changes. Avoid
// rescanning all eight image/TLUT slots before every draw; BP writes and memory
// publication events mark only the expensive resolver passes dirty.
bool g_texture_mapping_scan_dirty = true;
bool g_tlut_binding_scan_dirty = true;
std::uint64_t g_texture_memory_invalidations = 0;
std::uint64_t g_texture_cache_only_invalidations = 0;
std::uint64_t g_display_copy_count = 0;
// Observe the camera on a real 3D draw, before later HUD projections replace
// GX's current projection. Publish one packed pair without reading GX state
// from the CPU worker at a frame boundary.
std::atomic<std::uint64_t> g_camera_signature{0};
std::atomic<std::uint64_t> g_camera_observed_copy{0};
std::atomic<std::uint64_t> g_camera_copy_epoch{0};
std::uint64_t g_camera_area_epoch = ~std::uint64_t{0};
float g_camera_best_area = 0.f;
std::array<float,4> g_camera_coefficients{};
std::uint64_t g_camera_cached_signature = 0;
std::uint64_t g_camera_published_signature = 0;
std::uint64_t g_camera_published_copy = ~std::uint64_t{0};

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
// Single atomic snapshot: producer and CPU may coalesce completed frames,
// without exposing a mismatched pending flag/XFB address pair.
std::atomic_uint32_t g_frame_ready_xfb{0};
// v48 universal presentation interpolation. These are dedicated single-sample
// GPU snapshots, so the previous frame remains immutable even when a game
// reuses the same XFB address every frame. One GPU copy is paid per guest frame;
// intermediate host presents never touch guest memory or advance VI/DSP/AI.
bool g_frame_interpolation_enabled = false;
// v196 universal final-XFB presentation filter. The Balanced defaults mirror
// the restrained HPCOS Remaster look; Cinematic enables the stronger MOH-style
// filmic/edge-soft presentation without touching guest EFB/RAM semantics.
std::atomic<unsigned> g_render_percent{100};
std::atomic<unsigned> g_render_width{0}, g_render_height{0};
GekkoAOT::GX::GraphicsSettings g_video;
bool g_interpolation_requested = false;
bool g_post_processing_enabled = true;
float g_post_sharpen = 0.28f;
float g_post_bloom = 0.10f;
float g_post_vignette = 0.06f;
float g_post_grain = 0.0f;
float g_post_tone = 0.15f;
float g_post_local_contrast = 0.08f;
float g_post_cinematic = 0.0f;
std::string g_post_preset = "balanced";
std::uint64_t g_post_frame = 0;
bool g_interp_previous_valid = false;
bool g_interp_current_valid = false;
aurora::webgpu::TextureWithSampler g_interp_previous;
aurora::webgpu::TextureWithSampler g_interp_current;
std::uint64_t g_interp_snapshot_count = 0;

struct PendingEfbPoke {
  bool valid = false;
  std::uint16_t x = 0;
  std::uint16_t y = 0;
  std::uint16_t width = 0;
  std::uint16_t height = 1;
  std::uint16_t completed_y = 0;
  std::uint16_t completed_height = 0;
  std::uint32_t value = 0;
};
std::array<PendingEfbPoke, 2> g_pending_efb_pokes{};
bool g_efb_poke_batch = true;
bool g_efb_poke_stats = false;
bool g_efb_poke_first_flush_logged = false;
std::uint64_t g_efb_poke_writes = 0;
std::uint64_t g_efb_poke_draws = 0;

void EmitEfbPoke(const PendingEfbPoke& poke, bool depth) {
  if (!poke.valid || !g_frame_open) return;
  const auto size=aurora::gfx::get_render_target_size();
  if (size.x==0u || size.y==0u) return;
  const auto scaled=[](std::uint32_t pixel, std::uint32_t extent,
                       std::uint32_t native_extent) {
    return static_cast<std::int32_t>((std::uint64_t(pixel)*extent)/native_extent);
  };
  const auto left=scaled(poke.x,size.x,640u);
  const auto top=scaled(poke.y,size.y,528u);
  const auto right=static_cast<std::int32_t>(
      (std::uint64_t(poke.x+poke.width)*size.x+639u)/640u);
  const auto bottom=static_cast<std::int32_t>(
      (std::uint64_t(poke.y+poke.height)*size.y+527u)/528u);
  if (left>=static_cast<std::int32_t>(size.x) || top>=static_cast<std::int32_t>(size.y))
    return;
  const auto value=poke.value;
  const float z=static_cast<float>(value & 0x00ffffffu)/16777215.0f;
  aurora::gfx::push_draw_command(aurora::gfx::clear::DrawData{
      .pipeline=aurora::gfx::pipeline_ref(aurora::gfx::clear::PipelineConfig{
          .clearColor=!depth, .clearAlpha=!depth, .clearDepth=depth}),
      .color=wgpu::Color{
          .r=double((value>>16u)&0xffu)/255.0,
          .g=double((value>>8u)&0xffu)/255.0,
          .b=double(value&0xffu)/255.0,
          .a=double((value>>24u)&0xffu)/255.0},
      .depth=aurora::gfx::uses_reversed_z() ? 1.0f-z : z,
      .rect={.x=left,.y=top,
             .width=std::max(1,std::min(right,static_cast<std::int32_t>(size.x))-left),
             .height=std::max(1,std::min(bottom,static_cast<std::int32_t>(size.y))-top)}});
  ++g_efb_poke_draws;
}

void FlushOneEfbPoke(PendingEfbPoke& poke, bool depth) {
  if (poke.completed_height) {
    if (poke.x==0u && poke.width==640u &&
        poke.y==std::uint32_t(poke.completed_y)+poke.completed_height) {
      ++poke.completed_height;
      poke.valid=false;
    }
    EmitEfbPoke(PendingEfbPoke{.valid=true,.x=0u,.y=poke.completed_y,
                               .width=640u,.height=poke.completed_height,
                               .value=poke.value},depth);
  }
  EmitEfbPoke(poke,depth);
  poke={};
}

void FlushEfbPokes() {
  const auto draws_before=g_efb_poke_draws;
  for (unsigned depth=0; depth<g_pending_efb_pokes.size(); ++depth)
    FlushOneEfbPoke(g_pending_efb_pokes[depth],depth!=0u);
  if (g_efb_poke_stats && !g_efb_poke_first_flush_logged &&
      g_efb_poke_writes>=100000u && g_efb_poke_draws!=draws_before) {
    g_efb_poke_first_flush_logged=true;
    std::fprintf(stderr,"GEKKOAOT_EFB_POKE_V164_FIRST_FLUSH writes=%llu draws=%llu\n",
                 static_cast<unsigned long long>(g_efb_poke_writes),
                 static_cast<unsigned long long>(g_efb_poke_draws));
  }
}

void QueueEfbPoke(std::uint16_t x, std::uint16_t y, std::uint32_t value,
                  bool depth) {
  if (!g_efb_poke_batch) {
    EmitEfbPoke(PendingEfbPoke{.valid=true,.x=x,.y=y,.width=1u,.value=value},depth);
    ++g_efb_poke_writes;
    if (g_efb_poke_stats && g_efb_poke_writes%100000u==0u)
      std::fprintf(stderr,"GEKKOAOT_EFB_POKE_V164_STATS writes=%llu draws=%llu\n",
                   static_cast<unsigned long long>(g_efb_poke_writes),
                   static_cast<unsigned long long>(g_efb_poke_draws));
    return;
  }
  auto& poke=g_pending_efb_pokes[depth ? 1u : 0u];
  if (poke.valid && poke.y==y && poke.value==value &&
      std::uint32_t(poke.x)+poke.width==x) {
    ++poke.width;
  } else if (poke.valid && poke.value==value && poke.x==0u &&
             poke.width==640u && x==0u && y==std::uint32_t(poke.y)+1u) {
    if (poke.completed_height==0u) poke.completed_y=poke.y;
    ++poke.completed_height;
    poke.x=0u;
    poke.y=y;
    poke.width=1u;
  } else {
    FlushOneEfbPoke(poke,depth);
    poke={.valid=true,.x=x,.y=y,.width=1u,.value=value};
  }
  ++g_efb_poke_writes;
  if (g_efb_poke_stats && g_efb_poke_writes%100000u==0u)
    std::fprintf(stderr,"GEKKOAOT_EFB_POKE_V164_STATS writes=%llu draws=%llu\n",
                 static_cast<unsigned long long>(g_efb_poke_writes),
                 static_cast<unsigned long long>(g_efb_poke_draws));
}

// Native zero-copy XFB cache. Each slot owns the Aurora EFB render targets
// that were active when GXCopyDisp targeted the guest physical XFB address.
struct XfbZeroCopySlot {
  std::uint32_t address = 0;
  std::uint32_t stride_bytes = 0;
  std::uint32_t native_width = 0;
  std::uint32_t native_height = 0;
  std::uint64_t span_bytes = 0;
  std::uint64_t generation = 0;
  // Profile only: last generation handed to VI scanout (dead-copy accounting).
  std::uint64_t presented_generation = 0;
  bool valid = false;
  bool force_43 = false; // v363: recorded at the original 4:3
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
// GXCopyDisp results replaced before VI ever scanned them (their GPU work was invisible).
std::uint64_t g_xfb_unpresented_overwrites = 0;

// v320 profile: what separates consecutive draws (merge-blocker census).
// Bits: 1 BP-texture 2 BP-TEV/blend/z 4 BP-other 8 CP-array-base/stride
// 16 CP-VCD/VAT 32 XF-matrix-mem 64 XF-other 128 INDX-load 256 CALL_DL 512 copy.
std::uint32_t g_draw_gap_mask = 0;
bool g_draw_census = false;
std::array<std::uint64_t, 1024> g_draw_gap_hist{};
std::array<std::uint64_t, 6> g_draw_size_hist{};  // vertices: <=4 <=8 <=16 <=64 <=256 >256
std::uint64_t g_draw_census_draws = 0;
std::uint64_t g_draw_run_runs = 0, g_draw_run_members = 0;  // v320 draw-run coalescing
// Draw commands directly followed by another draw that did not join a run:
// [0] different VAT, [1] member too large/invalid, [2] state, [3] line/point.
std::array<std::uint64_t, 4> g_draw_run_misses{};
// v324 texture re-version census: BP66 invalidations, non-stable IMAGE3 binds,
// forced re-versions after BP66 and the source bytes each one makes Aurora hash.
std::uint64_t g_tex_census_bp66 = 0, g_tex_census_binds = 0, g_tex_census_forced = 0;
std::uint64_t g_tex_census_bind_bytes = 0, g_tex_census_forced_bytes = 0;

// v324: texture source versions between TMEM invalidations. Every non-stable
// IMAGE3 bind (a unit switching to another texture) used to take a brand-new
// texDataVersion, so Aurora re-hashed the whole source (XXH3-128, HPCOS
// ~670 MB/s) only to find the decoded texture it already had. Reuse the
// version an identical source (address, IMAGE0, mip count) received at its
// previous bind. The table is cleared on BP66 (GXInvalidateTex*: v237 source
// re-check policy) and on texture copies, and loses every entry whose guest
// range is written, so a reused version never outlives a known mutation.
struct TextureSourceVersion {
  std::uint32_t guest = 0, image0 = 0, span = 0, version = 0;
  std::uint8_t mips = 0;
};
std::array<TextureSourceVersion, 512> g_texture_source_versions{};
std::uint32_t g_texture_source_lo = 0xffffffffu, g_texture_source_hi = 0u;
bool g_texture_version_reuse = true;
std::uint64_t g_tex_census_reused = 0;

void ForgetTextureSourceVersions() {
  if (g_texture_source_hi == 0u) return;
  g_texture_source_versions.fill({});
  g_texture_source_lo = 0xffffffffu;
  g_texture_source_hi = 0u;
}

void ForgetTextureSourceRange(std::uint32_t address, std::uint32_t size) {
  const std::uint64_t end = std::uint64_t(address) + size;
  if (size == 0u || address >= g_texture_source_hi || end <= g_texture_source_lo) return;
  for (auto& entry : g_texture_source_versions)
    if (entry.version != 0u && address < std::uint64_t(entry.guest) + entry.span && entry.guest < end)
      entry = {};
}

TextureSourceVersion& TextureSourceSlot(std::uint32_t guest, std::uint32_t image0, std::uint8_t mips) {
  const std::uint32_t h = (guest >> 5u) * 0x9E3779B1u ^ image0 * 0x85EBCA77u ^ mips;
  return g_texture_source_versions[(h >> 7u) & (g_texture_source_versions.size() - 1u)];
}

void RememberTextureSourceVersion(std::uint32_t guest, std::uint32_t image0, std::uint8_t mips,
                                  std::uint32_t span, std::uint32_t version) {
  if (!g_texture_version_reuse || span == 0u) return;
  TextureSourceSlot(guest, image0, mips) = {guest, image0, span, version, mips};
  g_texture_source_lo = std::min(g_texture_source_lo, guest);
  g_texture_source_hi = static_cast<std::uint32_t>(
      std::min<std::uint64_t>(0xffffffffu, std::max<std::uint64_t>(g_texture_source_hi, std::uint64_t(guest) + span)));
}

// Version for a non-stable bind: the remembered one, or a new generation.
std::uint32_t BindTextureSourceVersion(std::uint32_t guest, std::uint32_t image0, std::uint8_t mips,
                                       std::uint32_t span, std::uint32_t generation) {
  if (g_texture_version_reuse && span != 0u) {
    const auto& entry = TextureSourceSlot(guest, image0, mips);
    if (entry.version != 0u && entry.guest == guest && entry.image0 == image0 &&
        entry.mips == mips && entry.span == span) {
      if (g_draw_census) ++g_tex_census_reused;
      return entry.version;
    }
  }
  RememberTextureSourceVersion(guest, image0, mips, span, generation);
  return generation;
}
inline void NoteDrawGap(std::uint32_t bit) { if (g_draw_census) g_draw_gap_mask |= bit; }
void NoteDraw(std::uint32_t vertices) {
  if (!g_draw_census) return;
  ++g_draw_census_draws;
  ++g_draw_gap_hist[g_draw_gap_mask & 1023u];
  g_draw_gap_mask = 0;
  const unsigned bucket = vertices <= 4u ? 0u : vertices <= 8u ? 1u : vertices <= 16u ? 2u
                        : vertices <= 64u ? 3u : vertices <= 256u ? 4u : 5u;
  ++g_draw_size_hist[bucket];
}
void ReportDrawCensus() {
  if (!g_draw_census || g_draw_census_draws == 0u) return;
  std::vector<std::pair<std::uint64_t, std::uint32_t>> top;
  for (std::uint32_t m = 0; m < g_draw_gap_hist.size(); ++m)
    if (g_draw_gap_hist[m]) top.emplace_back(g_draw_gap_hist[m], m);
  std::sort(top.begin(), top.end(), std::greater<>());
  std::string line;
  for (std::size_t i = 0; i < top.size() && i < 10u; ++i) {
    char item[40];
    std::snprintf(item, sizeof(item), " %03x:%.1f%%", top[i].second,
                  100.0 * double(top[i].first) / double(g_draw_census_draws));
    line += item;
  }
  std::fprintf(stderr, "GEKKOAOT_DRAW_RUNS_V320_STATS runs=%llu members=%llu misses(vat,size,state,line)=%llu,%llu,%llu,%llu\n",
               static_cast<unsigned long long>(std::exchange(g_draw_run_runs, 0u)),
               static_cast<unsigned long long>(std::exchange(g_draw_run_members, 0u)),
               static_cast<unsigned long long>(g_draw_run_misses[0]),
               static_cast<unsigned long long>(g_draw_run_misses[1]),
               static_cast<unsigned long long>(g_draw_run_misses[2]),
               static_cast<unsigned long long>(g_draw_run_misses[3]));
  g_draw_run_misses.fill(0u);
  std::fprintf(stderr, "GEKKOAOT_TEXTURE_REVERSION_V324 bp66=%llu binds=%llu reused=%llu bind_kb=%llu forced=%llu forced_kb=%llu\n",
               static_cast<unsigned long long>(std::exchange(g_tex_census_bp66, 0u)),
               static_cast<unsigned long long>(std::exchange(g_tex_census_binds, 0u)),
               static_cast<unsigned long long>(std::exchange(g_tex_census_reused, 0u)),
               static_cast<unsigned long long>(std::exchange(g_tex_census_bind_bytes, 0u) >> 10u),
               static_cast<unsigned long long>(std::exchange(g_tex_census_forced, 0u)),
               static_cast<unsigned long long>(std::exchange(g_tex_census_forced_bytes, 0u) >> 10u));
  std::fprintf(stderr, "GEKKOAOT_DRAW_CENSUS_V320=1 draws=%llu sizes(<=4,8,16,64,256,>)=%llu,%llu,%llu,%llu,%llu,%llu gaps%s\n",
               static_cast<unsigned long long>(g_draw_census_draws),
               static_cast<unsigned long long>(g_draw_size_hist[0]), static_cast<unsigned long long>(g_draw_size_hist[1]),
               static_cast<unsigned long long>(g_draw_size_hist[2]), static_cast<unsigned long long>(g_draw_size_hist[3]),
               static_cast<unsigned long long>(g_draw_size_hist[4]), static_cast<unsigned long long>(g_draw_size_hist[5]),
               line.c_str());
  g_draw_gap_hist.fill(0u); g_draw_size_hist.fill(0u); g_draw_census_draws = 0u;
}


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
// Selective EFB continuation policy.
//
// v176 kept a host recording open across color-preserving depth clears. That
// preserves guest EFB semantics, but titles which emit many such GXCopyDisp
// operations (notably HPCOS) can accumulate tens of thousands of draws in one
// Aurora frame and exhaust the per-frame vertex/index/storage arenas.
//
// v177 fixed the unbounded frame by ending at every GXCopyDisp and loading the
// post-copy EFB into the next recording, but loading *all* color+depth state for
// every display copy regressed HPCOS.
//
// The default below is the narrow version: every GXCopyDisp remains a bounded
// host producer boundary, while masked clear copies carry forward only the EFB
// attachment(s) that the guest did NOT clear. This is sufficient for the MOH
// Frontline viewmodel path and Terminator 3 channel-masked clears without
// globally inheriting stale EFB/depth state.
bool g_partial_clear_copy_continuation = false; // legacy v176 opt-in only
bool g_masked_clear_efb_continuity = true;
bool g_efb_boundary_continuity = false; // legacy/global v177 diagnostic opt-in
unsigned g_display_copy_state_logs = 0;
unsigned g_partial_clear_copy_logs = 0;
// v36: environment variables are immutable for one renderer instance.  Never
// call getenv() from FIFO/texture hot paths; cache diagnostic/coherency toggles
// once during gekkoaot_native_gx_init().
bool g_trace_gx_copy = false;
bool g_trace_video = false;
bool g_invalidate_texture_sources = true;
bool g_texture_source_invalidation_pending = false;
bool g_trace_font = false;
bool g_paranoid_texture_hash = false;
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
bool g_fifo_forensic_trace = false;
bool g_fifo_streaming = true; // Default streaming policy; explicit =0 keeps the buffered path
bool g_fifo_stream_stats = false;
std::uint64_t g_fifo_submitted_bytes = 0;
std::uint64_t g_fifo_copied_bytes = 0;
std::uint64_t g_fifo_direct_bytes = 0;
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
float EnvFloat(const char* name, float fallback, float minimum, float maximum) {
  const char* v=std::getenv(name); if(!v||!*v) return fallback;
  char* end=nullptr; const float x=std::strtof(v,&end);
  return end!=v && *end=='\0' && std::isfinite(x) && x>=minimum && x<=maximum ? x : fallback;
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

// v276: the widescreen scissor/projection helpers run for every GX draw. Each
// SDL3 window query validates the window under SDL's global object rwlock, so
// querying the live size per draw cost three rwlock acquisitions per primitive.
// Draws read this snapshot instead; PollEvents() refreshes it at every event
// poll (present/frame cadence), which observes every resize, display-scale,
// fullscreen and embedding change before the next frame is recorded.
std::atomic<std::uint32_t> g_host_window_aspect_bits{0u};

float QueryHostWindowAspect() noexcept {
  const auto size = aurora::window::get_window_size();
  const std::uint32_t width = size.native_fb_width != 0u ? size.native_fb_width : size.fb_width;
  const std::uint32_t height = size.native_fb_height != 0u ? size.native_fb_height : size.fb_height;
  if (width == 0u || height == 0u) return 4.0f / 3.0f;
  return static_cast<float>(width) / static_cast<float>(height);
}

float RefreshHostWindowAspect() noexcept {
  const float aspect = QueryHostWindowAspect();
  std::uint32_t bits = 0u;
  std::memcpy(&bits, &aspect, sizeof(bits));
  g_host_window_aspect_bits.store(bits, std::memory_order_relaxed);
  return aspect;
}

float HostWindowAspect() noexcept {
  const std::uint32_t bits = g_host_window_aspect_bits.load(std::memory_order_relaxed);
  if (bits == 0u) return RefreshHostWindowAspect();
  float aspect = 0.0f;
  std::memcpy(&aspect, &bits, sizeof(aspect));
  return aspect;
}

float TargetAspectForMode(std::uint32_t mode) noexcept {
  return mode == 1u ? HostWindowAspect() : 4.0f / 3.0f;
}

float ProjectionXScaleForMode(std::uint32_t mode) noexcept {
  if (mode != 1u) return 1.0f;
  return GekkoAOT::GX::UiXScale(HostWindowAspect());
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
  const auto mode = g_present_force_43 ? 0u : g_aspect_mode.load(std::memory_order_acquire);
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
  (void)mode;
  return g_interpolation_requested;
}

void ApplyAspectMode(std::uint32_t mode, bool log_change) {
  mode = mode == 1u ? 1u : 0u;
  if (g_info.window) RefreshHostWindowAspect();
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

bool ClassifyScreenFill(const std::uint8_t* data,std::size_t length,
                        GXPrimitive primitive,GXVtxFmt format,unsigned count) {
  using namespace aurora::gx;
  const auto& state=g_gxState;
  // v356: a fade or flash drawn as a 3D quad in front of the camera covers the
  // retail view exactly; hor+ would shrink it to 4:3 and leave the widened
  // scene visible beside it (Call of Duty: Finest Hour level fades).
  const bool perspective=state.projType==GX_PERSPECTIVE;
  if (perspective ? !g_wide_perspective_fills : state.projType!=GX_ORTHOGRAPHIC) return false;
  // Meshes never qualify: only a single quad (or two triangles) can be a fade.
  if (perspective && !((count==4u && (primitive==GX_QUADS || primitive==GX_TRIANGLESTRIP ||
                                       primitive==GX_TRIANGLEFAN)) ||
                       (count==6u && primitive==GX_TRIANGLES))) return false;
  const bool fill=!perspective && state.numTexGens==1u && state.numTevStages<=2u &&
      state.blendMode==GX_BM_NONE &&
      ((count==4u && (primitive==GX_QUADS || primitive==GX_TRIANGLESTRIP)) ||
       (count==6u && primitive==GX_TRIANGLES));
  // v343: full-width spans (letterbox bars, fades, flashes) also bypass the UI
  // x-scale. Otherwise they stay centred at 4:3 width and leave the widened 3D
  // scene visible beside a cinematic bar or a fade.
  const bool span=g_wide_solid_spans && count<=kMaxSpanVertices &&
      ((primitive==GX_QUADS && count>=4u && count%4u==0u) ||
       (primitive==GX_TRIANGLES && count>=3u && count%3u==0u) ||
       // v357: fans too (Call of Duty: Finest Hour's mission fade-in).
       ((primitive==GX_TRIANGLESTRIP || primitive==GX_TRIANGLEFAN) && count>=4u));
  // A stage samples its texture only when a combiner input reads TEXC/TEXA
  // (Aurora's shader_info rule); CoS binds texmap 0 to its solid cinematic
  // bars without reading it.
  const auto samples=[](const TevStage& stage) {
    if (stage.texMapId==GX_TEXMAP_NULL) return false;
    const auto& c=stage.colorPass; const auto& a=stage.alphaPass;
    for (const auto arg : {c.a,c.b,c.c,c.d}) if (arg==GX_CC_TEXC || arg==GX_CC_TEXA) return true;
    for (const auto arg : {a.a,a.b,a.c,a.d}) if (arg==GX_CA_TEXA) return true;
    return false;
  };
  bool sampled=state.numIndStages!=0u || state.zTexOp!=GX_ZT_DISABLE, uniform=!sampled;
  // Perspective quads test their geometry first (cheap) and textures after.
  const auto texture_uniformity=[&] {
  for (unsigned stage=0;span && uniform && stage<state.numTevStages && stage<MaxTevStages;++stage) {
    const auto map=state.tevStages[stage].texMapId;
    if (!samples(state.tevStages[stage])) continue;
    sampled=true;
    const auto& slot=state.loadedTextures[unsigned(map)&7u];
    const std::size_t bytes=slot.data ? texture::texture_source_size(slot.raw_format(),slot.width(),slot.height(),1u) : 0u;
    uniform=GekkoAOT::GX::IsUniformTexture(static_cast<const std::uint8_t*>(slot.data),
                                           std::min<std::size_t>(bytes,16384u),slot.raw_format());
  }
  };
  if (!perspective) texture_uniformity();
  // v353: a whole-screen quad sampling only EFB copies composites screen-
  // derived data: bloom/blur chains downsample the widened EFB into small
  // copies and add the result back over the frame (Shadow the Hedgehog). The
  // small copy fails the screen-copy size test, but it still covers the whole
  // retail width, so it must keep it rather than shrink to 4:3.
  bool copy_only=false, screen_inputs=false;
  const aurora::gfx::ClipRect* copy_source=nullptr;
  if (span && sampled && state.numIndStages==0u && !perspective) {
    bool any=false;
    copy_only=true;
    screen_inputs=g_copy_provenance_enabled;
    for (unsigned stage=0;stage<state.numTevStages && stage<MaxTevStages;++stage) {
      if (!samples(state.tevStages[stage])) continue;
      any=true;
      const auto& slot=state.loadedTextures[unsigned(state.tevStages[stage].texMapId)&7u];
      const auto copy=slot.data ? state.copyTextures.find(slot.data) : state.copyTextures.end();
      if (copy==state.copyTextures.end()) { copy_only=false; break; }
      screen_inputs=screen_inputs && g_copy_provenance.IsScreenDerived(slot.data);
      copy_source=copy_source && copy_source!=&copy->second.guestSource ? nullptr : &copy->second.guestSource;
    }
    copy_only=copy_only && any;
    screen_inputs=screen_inputs && copy_only;
  }
  // v353: a single modulate stage (RASC x TEXC) with a black raster colour
  // draws black whatever the texture holds (Metroid Prime's letterbox bars);
  // with alpha that ignores the texture, or an opaque texture, it is solid.
  bool vertex_black=false;
  if (!perspective && span && sampled && !uniform && state.numTevStages==1u && state.numIndStages==0u &&
      state.zTexOp==GX_ZT_DISABLE) {
    const auto& st=state.tevStages[0];
    const auto& c=st.colorPass; const auto& a=st.alphaPass;
    const bool modulate=c.a==GX_CC_ZERO && c.d==GX_CC_ZERO &&
        ((c.b==GX_CC_RASC && c.c==GX_CC_TEXC) || (c.b==GX_CC_TEXC && c.c==GX_CC_RASC));
    const bool alpha_reads_texture=a.a==GX_CA_TEXA || a.b==GX_CA_TEXA || a.c==GX_CA_TEXA || a.d==GX_CA_TEXA;
    const auto& slot=state.loadedTextures[unsigned(st.texMapId)&7u];
    const std::size_t bytes=slot.data ? texture::texture_source_size(slot.raw_format(),slot.width(),slot.height(),1u) : 0u;
    const bool alpha_solid=!alpha_reads_texture ||
        GekkoAOT::GX::IsOpaqueTexture(static_cast<const std::uint8_t*>(slot.data),
                                      std::min<std::size_t>(bytes,16384u),slot.raw_format());
    if (modulate && alpha_solid && (st.channelId==GX_COLOR0A0 || st.channelId==GX_COLOR0)) {
      if (state.colorChannelConfig[0].matSrc==GX_SRC_REG) {
        const auto& m=state.colorChannelState[0].matColor;
        uniform=m[0]==0.f && m[1]==0.f && m[2]==0.f;
      } else {
        vertex_black=state.vtxDesc[GX_VA_CLR0]==GX_DIRECT;
      }
    }
  }
  // Diagnostic: GEKKOAOT_WIDESCREEN_DRAWLOG_AT=<s> logs every small draw for
  // ~0.1 s that many seconds after the first draw.
  static const double drawlog_at=[] { const char* v=std::getenv("GEKKOAOT_WIDESCREEN_DRAWLOG_AT"); return v ? std::atof(v) : 0.0; }();
  static const auto drawlog_origin=std::chrono::steady_clock::now();
  bool drawlog=false;
  if (drawlog_at>0.0) {
    const double t=std::chrono::duration<double>(std::chrono::steady_clock::now()-drawlog_origin).count();
    static const double drawlog_for=[] { const char* v=std::getenv("GEKKOAOT_WIDESCREEN_DRAWLOG_FOR"); return v ? std::atof(v) : 4.0; }();
    drawlog=t>=drawlog_at && t<drawlog_at+drawlog_for;
  }
  const bool trace=(g_wide_span_trace || drawlog) && count>=3u && count<=kMaxSpanVertices &&
      primitive!=GX_POINTS && primitive!=GX_LINES && primitive!=GX_LINESTRIP;
  if (!fill && !span && !trace) return false;
  const auto display=aurora::vi::configured_fb_size();
  const auto& viewport=state.logicalViewport;
  // Horizontal stretching only needs a full-width viewport; PoA renders its
  // cinematics into a 640x448 viewport of a 640x480 display.
  const bool wide_viewport=DisplayViewWidth(viewport.width)>=display.x*.95f;
  const bool full_viewport=wide_viewport && viewport.height>=display.y*.95f;
  if (!wide_viewport && !trace && !copy_only) return false;
  const unsigned stride=VertexSize(format);
  if (!stride || length!=3u+count*stride) return false;
  unsigned position_offset=0, matrix_offset=0;
  bool indexed_matrix=false;
  for (int attr=GX_VA_PNMTXIDX;attr<GX_VA_POS;++attr) {
    const auto desc=state.vtxDesc[attr];
    if (desc==GX_NONE) continue;
    if (desc!=GX_DIRECT) return false;
    if (attr==GX_VA_PNMTXIDX) { indexed_matrix=true;matrix_offset=position_offset; }
    const auto& f=state.vtxFmts[format].attrs[attr];
    position_offset+=unsigned(comp_type_size(static_cast<GXAttr>(attr),f.type))*
                     comp_cnt_count(static_cast<GXAttr>(attr),f.cnt);
  }
  const auto desc=state.vtxDesc[GX_VA_POS];
  const auto& posfmt=state.vtxFmts[format].attrs[GX_VA_POS];
  const unsigned components=comp_cnt_count(GX_VA_POS,posfmt.cnt);
  const unsigned size=comp_type_size(GX_VA_POS,posfmt.type);
  if (components<2u || components>3u || !size) return false;
  // CLR0 offset, laid out exactly like calc_vtx_size().
  unsigned color_offset=0;
  const auto color_type=state.vtxFmts[format].attrs[GX_VA_CLR0].type;
  if (vertex_black) {
    for (int attr=GX_VA_PNMTXIDX;attr<GX_VA_CLR0;++attr) {
      const auto d=state.vtxDesc[attr];
      const auto& f=state.vtxFmts[format].attrs[attr];
      if (d==GX_DIRECT)
        color_offset+=unsigned(comp_type_size(static_cast<GXAttr>(attr),f.type))*
                      comp_cnt_count(static_cast<GXAttr>(attr),f.cnt);
      else if (d==GX_INDEX8 || d==GX_INDEX16)
        color_offset+=(attr==GX_VA_NRM && f.cnt==GX_NRM_NBT3 ? 3u : 1u)*(d==GX_INDEX16 ? 2u : 1u);
    }
    vertex_black=color_offset+comp_type_size(GX_VA_CLR0,color_type)<=stride;
  }
  int vertex_alpha=-1;
  std::array<std::array<float,2>,kMaxSpanVertices> corners{};
  std::array<float,kMaxSpanVertices> view_z{};
  for (unsigned i=0;i<count;++i) {
    const auto* vertex=data+3u+i*stride;
    if (vertex_black) {
      const auto* p=vertex+color_offset;
      unsigned rgb=1u, alpha=255u;
      switch (color_type) {
      case GX_RGB565: rgb=(unsigned(p[0])<<8)|p[1]; break;
      case GX_RGB8: case GX_RGBX8: rgb=unsigned(p[0])|p[1]|p[2]; break;
      case GX_RGBA4: rgb=unsigned(p[0])|(p[1]&0xf0u); alpha=(p[1]&0x0fu)*17u; break;
      case GX_RGBA6: { const unsigned v=(unsigned(p[0])<<16)|(unsigned(p[1])<<8)|p[2]; rgb=v>>6; alpha=v&63u; break; }
      case GX_RGBA8: rgb=unsigned(p[0])|p[1]|p[2]; alpha=p[3]; break;
      default: break;
      }
      if (rgb!=0u || (vertex_alpha>=0 && unsigned(vertex_alpha)!=alpha)) vertex_black=false;
      vertex_alpha=int(alpha);
    }
    const auto* pos=vertex+position_offset;
    bool little=false;
    if (desc==GX_INDEX8 || desc==GX_INDEX16) {
      const unsigned index=desc==GX_INDEX8 ? *pos : Be16(pos);
      const auto& array=state.arrays[GX_VA_POS];
      const std::uint64_t offset=std::uint64_t(index)*array.stride;
      if (!array.data || offset+components*size>array.size) return false;
      pos=static_cast<const std::uint8_t*>(array.data)+offset;little=array.le;
    } else if (desc!=GX_DIRECT || position_offset+components*size>stride) return false;
    float xyz[4]={0,0,0,1};
    for (unsigned axis=0;axis<components;++axis) {
      const auto* q=pos+axis*size;
      const unsigned bits=size==1u ? *q : size==2u ?
          (little ? unsigned(q[0])|(unsigned(q[1])<<8u) : Be16(q)) :
          (little ? unsigned(q[0])|(unsigned(q[1])<<8u)|(unsigned(q[2])<<16u)|(unsigned(q[3])<<24u) : Be32(q));
      switch (posfmt.type) {
      case GX_F32: std::memcpy(&xyz[axis],&bits,4u);break;
      case GX_U8: case GX_U16: xyz[axis]=std::ldexp(float(bits),-posfmt.frac);break;
      case GX_S8: xyz[axis]=std::ldexp(float(static_cast<std::int8_t>(bits)),-posfmt.frac);break;
      case GX_S16: xyz[axis]=std::ldexp(float(static_cast<std::int16_t>(bits)),-posfmt.frac);break;
      default: return false;
      }
    }
    const unsigned matrix=indexed_matrix ? (vertex[matrix_offset]&63u) : state.currentPnMtx;
    if (matrix+2u>=state.xfPosRows.size()) return false;
    float transformed[3]={0,0,0};
    for (unsigned axis=0;axis<(perspective ? 3u : 2u);++axis)
      for (unsigned j=0;j<4;++j) transformed[axis]+=state.xfPosRows[matrix+axis][j]*xyz[j];
    view_z[i]=transformed[2];
    if (perspective) {
      const auto row=[&](const auto& m) {
        return m[0]*transformed[0]+m[1]*transformed[1]+m[2]*transformed[2]+m[3];
      };
      const float w=row(state.proj.m3);
      if (!(w>1e-6f)) return false;
      corners[i]={row(state.proj.m0)/w,row(state.proj.m1)/w};
    } else {
      corners[i]={state.proj.m0[0]*transformed[0]+state.proj.m0[3],
                  state.proj.m1[1]*transformed[1]+state.proj.m1[3]};
    }
    // HUD sprites and glyphs fail the span test at their first vertex.
    if (!fill && !trace && !copy_only && std::abs(corners[i][0])<.975f) return false;
  }
  if (g_wide_prerendered_43 && state.zTexOp!=GX_ZT_DISABLE && state.depthUpdate && count<=6u &&
      GekkoAOT::GX::IsFullScreenSpan({corners.data(),count}))
    g_frame_prerendered=true;
  if (trace) {
    float lo_x=corners[0][0],hi_x=lo_x,lo_y=corners[0][1],hi_y=lo_y;
    for (unsigned i=1;i<count;++i) {
      lo_x=std::min(lo_x,corners[i][0]); hi_x=std::max(hi_x,corners[i][0]);
      lo_y=std::min(lo_y,corners[i][1]); hi_y=std::max(hi_y,corners[i][1]);
    }
    unsigned maps=0;
    for (unsigned stage=0;stage<state.numTevStages && stage<MaxTevStages;++stage)
      maps=maps*16u+(state.tevStages[stage].texMapId==GX_TEXMAP_NULL ? 15u : unsigned(state.tevStages[stage].texMapId)&7u);
    const std::uint64_t signature=(std::uint64_t(primitive)<<56)^(std::uint64_t(count)<<48)^
        (std::uint64_t(state.numTexGens)<<44)^(std::uint64_t(maps)<<24)^(std::uint64_t(state.blendMode)<<20)^
        (std::uint64_t(viewport.height)<<8)^std::uint64_t(int(lo_y*8.f)&15)^(std::uint64_t(int(hi_y*8.f)&15)<<4);
    for (unsigned stage=0;stage<state.numTevStages && stage<MaxTevStages;++stage) {
      if (!samples(state.tevStages[stage])) continue;
      const auto& slot=state.loadedTextures[unsigned(state.tevStages[stage].texMapId)&7u];
      const auto copy=state.copyTextures.find(slot.data);
      if (!slot.data || copy==state.copyTextures.end()) continue;
      const auto& src=copy->second.guestSource;
      static std::vector<std::uint64_t> sprites;
      const std::uint64_t key=(std::uint64_t(src.width)<<48)^(std::uint64_t(src.height)<<36)^
          (std::uint64_t(int(lo_x*16.f)&255)<<16)^(std::uint64_t(int(hi_x*16.f)&255)<<8)^std::uint64_t(viewport.width);
      if (sprites.size()<64u && std::find(sprites.begin(),sprites.end(),key)==sprites.end()) {
        sprites.push_back(key);
        std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_COPY_SPRITE_TRACE src=%d,%d,%dx%d tex=%ux%u viewport=%.0fx%.0f@%.0f,%.0f "
                     "x=%.3f..%.3f y=%.3f..%.3f count=%u prim=0x%02x\n",src.x,src.y,src.width,src.height,
                     slot.width(),slot.height(),viewport.width,viewport.height,viewport.left,viewport.top,
                     lo_x,hi_x,lo_y,hi_y,count,unsigned(primitive));
      }
    }
    static std::vector<std::uint64_t> seen;
    if (hi_x-lo_x>=1.5f && seen.size()<128u &&
        std::find(seen.begin(),seen.end(),signature)==seen.end()) {
      seen.push_back(signature);
      const auto& st0=state.tevStages[0];
      std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_SPAN_TRACE_V343 prim=0x%02x count=%u texgens=%u tev=%u maps=%x blend=%u "
                   "viewport=%.0fx%.0f@%.0f,%.0f full=%u x=%.3f..%.3f y=%.3f..%.3f fill=%u span=%u sampled=%u uniform=%u "
                   "c0=%u,%u,%u,%u a0=%u,%u,%u,%u chan=%u kc=%u matsrc=%u mat=%.2f,%.2f,%.2f,%.2f clr0=%u tex=%ux%u:%u\n",
                   unsigned(primitive),count,unsigned(state.numTexGens),unsigned(state.numTevStages),maps,
                   unsigned(state.blendMode),viewport.width,viewport.height,viewport.left,viewport.top,
                   full_viewport?1u:0u,lo_x,hi_x,lo_y,hi_y,fill?1u:0u,span?1u:0u,sampled?1u:0u,uniform?1u:0u,
                   unsigned(st0.colorPass.a),unsigned(st0.colorPass.b),unsigned(st0.colorPass.c),unsigned(st0.colorPass.d),
                   unsigned(st0.alphaPass.a),unsigned(st0.alphaPass.b),unsigned(st0.alphaPass.c),unsigned(st0.alphaPass.d),
                   unsigned(st0.channelId),unsigned(st0.kcSel),unsigned(state.colorChannelConfig[0].matSrc),
                   state.colorChannelState[0].matColor[0],state.colorChannelState[0].matColor[1],
                   state.colorChannelState[0].matColor[2],state.colorChannelState[0].matColor[3],
                   unsigned(state.vtxDesc[GX_VA_CLR0]),
                   st0.texMapId==GX_TEXMAP_NULL?0u:state.loadedTextures[unsigned(st0.texMapId)&7u].width(),
                   st0.texMapId==GX_TEXMAP_NULL?0u:state.loadedTextures[unsigned(st0.texMapId)&7u].height(),
                   st0.texMapId==GX_TEXMAP_NULL?0u:unsigned(state.loadedTextures[unsigned(st0.texMapId)&7u].raw_format()));
      if (st0.texMapId!=GX_TEXMAP_NULL && state.loadedTextures[unsigned(st0.texMapId)&7u].data) {
        const auto* tb=static_cast<const std::uint8_t*>(state.loadedTextures[unsigned(st0.texMapId)&7u].data);
        std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_SPAN_TEX");
        for (unsigned i=0;i<48;++i) std::fprintf(stderr,"%s%02x",i%8?"":" ",tb[i]);
        std::fprintf(stderr,"\n");
      }
    }
  }
  if (vertex_black) uniform=true;
  if (drawlog) {
    float lo_x=corners[0][0],hi_x=lo_x,lo_y=corners[0][1],hi_y=lo_y;
    for (unsigned i=1;i<count;++i) {
      lo_x=std::min(lo_x,corners[i][0]); hi_x=std::max(hi_x,corners[i][0]);
      lo_y=std::min(lo_y,corners[i][1]); hi_y=std::max(hi_y,corners[i][1]);
    }
    float vz_lo=view_z[0],vz_hi=view_z[0];
    for (unsigned i=1;i<count;++i) { vz_lo=std::min(vz_lo,view_z[i]); vz_hi=std::max(vz_hi,view_z[i]); }
    const auto& st0=state.tevStages[0];
    // Wide draws, and draws entirely outside the 4:3 view that widescreen
    // can reveal in the side bands.
    if (hi_x-lo_x>=1.f || (hi_x<-1.f && hi_x>-1.4f) || (lo_x>1.f && lo_x<1.4f))
    std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_DRAWLOG copy=%llu ztex=%u fill=%u ztest=%u zwrite=%u vz=%.2f..%.2f pm=%.3f,%.3f proj=%u prim=0x%02x count=%u x=%.3f..%.3f y=%.3f..%.3f view=%.0fx%.0f@%.0f,%.0f scissor=%d,%d,%dx%d tev=%u texgens=%u blend=%u span=%u sampled=%u uniform=%u copy_only=%u c0=%u,%u,%u,%u a0=%u,%u,%u,%u tex=%ux%u:%u\n",
                 static_cast<unsigned long long>(g_display_copy_count),unsigned(state.zTexOp),fill?1u:0u,
                 state.depthCompare?1u:0u,state.depthUpdate?1u:0u,
                 vz_lo,vz_hi,state.proj.m0[0],state.proj.m1[1],unsigned(state.projType),unsigned(primitive),count,
                 lo_x,hi_x,lo_y,hi_y,viewport.width,viewport.height,viewport.left,viewport.top,
                 state.logicalScissor.x,state.logicalScissor.y,state.logicalScissor.width,state.logicalScissor.height,
                 unsigned(state.numTevStages),unsigned(state.numTexGens),unsigned(state.blendMode),span?1u:0u,
                 sampled?1u:0u,uniform?1u:0u,copy_only?1u:0u,
                 unsigned(st0.colorPass.a),unsigned(st0.colorPass.b),unsigned(st0.colorPass.c),unsigned(st0.colorPass.d),
                 unsigned(st0.alphaPass.a),unsigned(st0.alphaPass.b),unsigned(st0.alphaPass.c),unsigned(st0.alphaPass.d),
                 st0.texMapId==GX_TEXMAP_NULL?0u:state.loadedTextures[unsigned(st0.texMapId)&7u].width(),
                 st0.texMapId==GX_TEXMAP_NULL?0u:state.loadedTextures[unsigned(st0.texMapId)&7u].height(),
                 st0.texMapId==GX_TEXMAP_NULL?0u:unsigned(state.loadedTextures[unsigned(st0.texMapId)&7u].raw_format()));
  }
  if (perspective) {
    if (!span || !wide_viewport || !GekkoAOT::GX::IsFullScreenSpan({corners.data(),count})) return false;
    texture_uniformity();
    const bool screen=uniform;
    static unsigned perspective_logs=0;
    if (screen && perspective_logs++<8u)
      std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_PERSPECTIVE_FILL_V356 prim=0x%02x count=%u blend=%u sampled=%u action=retail-width\n",
                   unsigned(primitive),count,unsigned(state.blendMode),sampled?1u:0u);
    return screen;
  }
  if (copy_only && g_copy_provenance_enabled) {
    // Logical EFB rectangle covered by this pass (GX viewport y points down).
    float lo_x=corners[0][0],hi_x=lo_x,lo_y=corners[0][1],hi_y=lo_y;
    for (unsigned i=1;i<count;++i) {
      lo_x=std::min(lo_x,corners[i][0]); hi_x=std::max(hi_x,corners[i][0]);
      lo_y=std::min(lo_y,corners[i][1]); hi_y=std::max(hi_y,corners[i][1]);
    }
    const float x0=viewport.left+(std::max(lo_x,-1.f)+1.f)*.5f*viewport.width;
    const float x1=viewport.left+(std::min(hi_x,1.f)+1.f)*.5f*viewport.width;
    const float y0=viewport.top+(1.f-std::min(hi_y,1.f))*.5f*viewport.height;
    const float y1=viewport.top+(1.f-std::max(lo_y,-1.f))*.5f*viewport.height;
    // v354 rule A: a pass sampling only screen-derived copies is EFB pixel
    // work (downsample/blur/upsample/composite) and keeps the retail width.
    // Rule B: a copy drawn back onto exactly its own source rectangle
    // restores saved EFB contents (Lemony saves the quarter its bloom
    // pyramid overwrites) and must land on the same pixels.
    const bool restore=copy_source &&
        std::abs(x0-float(copy_source->x))<=2.f && std::abs(y0-float(copy_source->y))<=2.f &&
        std::abs(x1-float(copy_source->x+copy_source->width))<=2.f &&
        std::abs(y1-float(copy_source->y+copy_source->height))<=2.f;
    if (screen_inputs || restore) {
      if (screen_inputs) g_copy_provenance.NoteScreenDraw(x0,y0,x1,y1);
      static unsigned provenance_logs=0;
      if (provenance_logs++<16u)
        std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_COPY_PROVENANCE_V354 rule=%s rect=%.0f,%.0f-%.0f,%.0f viewport=%.0fx%.0f action=retail-width\n",
                     screen_inputs ? "screen-derived" : "restore",x0,y0,x1,y1,viewport.width,viewport.height);
      return true;
    }
  }
  if (span && wide_viewport && copy_only &&
      GekkoAOT::GX::IsFullScreenSpan({corners.data(),count})) {
    static unsigned composite_logs=0;
    if (composite_logs++<8u)
      std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_COPY_COMPOSITE_V353 prim=0x%02x count=%u blend=%u action=retail-width\n",
                   unsigned(primitive),count,unsigned(state.blendMode));
    return true;
  }
  if (span && wide_viewport && uniform &&
      GekkoAOT::GX::IsEdgeAnchoredSpan({corners.data(),count})) {
    if (g_wide_span_logs<8u) {
      ++g_wide_span_logs;
      std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_SPAN_V343 prim=0x%02x count=%u sampled=%u blend=%u y=%.3f..%.3f\n",
                   unsigned(primitive),count,sampled?1u:0u,unsigned(state.blendMode),
                   corners[0][1],corners[count-1u][1]);
    }
    return true;
  }
  if (!fill || !full_viewport) return false;
  bool screen=false;
  if (count==6u) {
    std::array<std::array<float,2>,6> triangles{};
    std::copy_n(corners.begin(),6u,triangles.begin());
    screen=GekkoAOT::GX::IsScreenFillTriangles(triangles);
  } else {
    std::array<std::array<float,2>,4> quad{};
    std::copy_n(corners.begin(),4u,quad.begin());
    screen=GekkoAOT::GX::IsScreenFillQuad(quad,primitive==GX_TRIANGLESTRIP);
  }
  if (screen) {
    // Diagnostic: which textures the opaque full-screen rule stretches.
    const auto& slot=state.loadedTextures[0];
    const std::uint64_t key=(std::uint64_t(slot.width())<<32)|(std::uint64_t(slot.height())<<8)|slot.raw_format();
    static std::vector<std::uint64_t> fills;
    if (fills.size()<8u && std::find(fills.begin(),fills.end(),key)==fills.end()) {
      fills.push_back(key);
      std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_FILL_V253 tex=%ux%u fmt=%u data=%u tev=%u\n",
                   slot.width(),slot.height(),slot.raw_format(),slot.data?1u:0u,unsigned(state.numTevStages));
    }
  }
  return screen;
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
      if(!layout.bindings.Add(static_cast<unsigned>(i))) return false;
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
  for(unsigned f=0;f<layout.field_count;++f)
    if(layout.fields[f].index_width==1u) layout.has_index8=1;
  if(layout.field_count>=2u && layout.field_count<=4u) {
    std::array<std::uint16_t,4> offsets{};
    bool eligible=true;
    for(unsigned f=0;f<layout.field_count;++f) {
      const auto& field=layout.fields[f];
      offsets[f]=field.offset;
      eligible=eligible && field.index_width==2u && field.index_count==1u;
    }
    if(eligible) layout.native.Compile(layout.stride,{offsets.data(),layout.field_count});
  }
  if(layout.field_count && layout.field_count<=GekkoAOT::GX::NativeVertexGather::MaxFields) {
    std::array<GekkoAOT::GX::NativeVertexGather::Field,4> fields;
    bool eligible=true;
    for(unsigned f=0;f<layout.field_count;++f) {
      const auto& field=layout.fields[f];
      fields[f]={field.offset,field.element_size};
      eligible=eligible && field.index_width==2u && field.index_count==1u;
    }
    if(eligible) layout.gather.Compile(layout.stride,{fields.data(),layout.field_count});
  }
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

bool PrepareIndexedArrayWindows(const std::uint8_t* p, std::size_t len,
                                CompactArrayPlan* compact = nullptr,
                                const GekkoAOT::GX::NativeAttributePlan** bindings = nullptr) {
  using namespace aurora::gx;
  if(!p || len<3u) return false;

  const auto fmt=static_cast<GXVtxFmt>(p[0]&GX_VAT_MASK);
  const std::uint32_t vtx_count=Be16(p+1);
  const auto* layout=GetIndexScanLayout(fmt);
  if(!layout) return false;
  if(bindings) *bindings=&layout->bindings;
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
  if(g_indexed_draw_coherency || g_native_array_snapshots) g_gxState.dirty |= DirtyImmediates;

  // INDEX8 can address at most 256 elements. For the common small SDK arrays
  // it is cheaper to expose that complete bounded window once than to rescan
  // every vertex of every draw just to rediscover the current maximum index.
  // Keep the eager window bounded so large/INDEX16 arrays still use exact
  // per-draw sizing and never recreate the old multi-MiB staging problem.
  constexpr std::uint64_t kEagerIndexWindowBytes=64u*1024u;
  std::array<bool,12> skip_scan{};
  for(std::uint32_t f=0;layout->has_index8 && f<layout->field_count;++f) {
    const auto& field=layout->fields[f];
    if(field.index_width!=1u || (g_native_array_snapshots && compact)) continue;
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

  const auto* const vertices=p+3;

  // Scan only the indexed fields. v1 walked every VCD attribute for every
  // vertex, which made ProcessOne a measurable hot spot. The cached layout
  // turns the inner loop into fixed-offset byte/halfword reads.
  //
  // v300: one pass over the vertices for all scanned fields. Retail draws are
  // short (a few vertices) and a separate loop per field mispredicted at every
  // draw-dependent exit; the common one-INDEX16-per-field layout also drops
  // the per-vertex width/count branches. Results are identical (the minimum
  // is only consumed for INDEX16 compact windows).
  struct ScanState {
    std::uint16_t offset;
    std::uint8_t width;
    std::uint8_t count;
    std::uint8_t attr;
    std::uint32_t maximum;
    std::uint32_t minimum;
  };
  std::array<ScanState,std::tuple_size_v<decltype(layout->fields)>> scan;
  std::uint32_t scan_count=0;
  bool single_index16=true;
  for(std::uint32_t f=0;f<layout->field_count;++f) {
    if(skip_scan[f]) continue;
    const auto& field=layout->fields[f];
    scan[scan_count++]={field.offset,field.index_width,field.index_count,field.attr,0u,65535u};
    single_index16=single_index16 && field.index_width==2u && field.index_count==1u;
  }
  const std::size_t vertex_stride=layout->stride;
  // Fixed field counts unroll the per-vertex work; the vertex loop exit is the
  // only data-dependent branch left on the common one-INDEX16-per-field path.
  const auto scan_index16=[&]<std::uint32_t N>() {
    std::array<std::uint32_t,N> offset{}, maximum{}, minimum{};
    for(std::uint32_t k=0;k<N;++k) {
      offset[k]=scan[k].offset;
      minimum[k]=65535u;
    }
    const auto* base=vertices;
    for(std::uint32_t v=0;v<vtx_count;++v,base+=vertex_stride) {
      for(std::uint32_t k=0;k<N;++k) {
        const auto index=std::uint32_t(Be16(base+offset[k]));
        maximum[k]=std::max(maximum[k],index);
        minimum[k]=std::min(minimum[k],index);
      }
    }
    for(std::uint32_t k=0;k<N;++k) {
      scan[k].maximum=maximum[k];
      scan[k].minimum=minimum[k];
    }
  };
  // CPU-native decode cache: reuse exact bounds for repeated immutable draws.
  // Include skip decisions because eager INDEX8 admission depends on arrays.
  struct CacheLayout {IndexScanLayout layout;std::array<bool,12> skipped;};
  CacheLayout cache_layout;
  const GekkoAOT::GX::NativeIndexRangeCache::Bounds* cached=nullptr;
  if(g_native_index_range_cache && scan_count) {
    // Avoid constructing a large shadow at all on the default uncached path.
    // Zero object representation as well: exact keys must not depend on padding.
    std::memset(&cache_layout,0,sizeof cache_layout);
    cache_layout.layout=*layout;cache_layout.skipped=skip_scan;
    cached=g_native_index_ranges.Find(&cache_layout,sizeof cache_layout,vertices,vertex_bytes);
  }
  if(cached) {
    for(std::uint32_t k=0;k<scan_count;++k) {
      scan[k].minimum=cached->minimum[k];scan[k].maximum=cached->maximum[k];
    }
  } else if(g_native_vertex_program && layout->native.fields==scan_count &&
            layout->native.fields>=2u) {
    std::array<GekkoAOT::GX::NativeVertexProgram::Bounds,4> bounds;
    // Layout and draw-size were proved above. NBT3/mixed formats are not
    // admitted and preserve the existing scan/visibility path below.
    if(!layout->native.Execute({vertices,vertex_bytes},vtx_count,bounds)) return false;
    for(unsigned k=0;k<scan_count;++k) {
      scan[k].minimum=bounds[k].minimum;scan[k].maximum=bounds[k].maximum;
    }
    if(g_native_vertex_profile) {
      ++g_native_vertex_draws;g_native_vertex_vertices+=vtx_count;
    }
  } else if(scan_count!=0u && single_index16) {
    switch(scan_count) {
    case 1u: scan_index16.template operator()<1u>(); break;
    case 2u: scan_index16.template operator()<2u>(); break;
    case 3u: scan_index16.template operator()<3u>(); break;
    case 4u: scan_index16.template operator()<4u>(); break;
    default: {
      const auto* base=vertices;
      for(std::uint32_t v=0;v<vtx_count;++v,base+=vertex_stride) {
        for(std::uint32_t k=0;k<scan_count;++k) {
          const auto index=std::uint32_t(Be16(base+scan[k].offset));
          scan[k].maximum=std::max(scan[k].maximum,index);
          scan[k].minimum=std::min(scan[k].minimum,index);
        }
      }
      break;
    }
    }
  } else if(scan_count!=0u) {
    const auto* base=vertices;
    for(std::uint32_t v=0;v<vtx_count;++v,base+=vertex_stride) {
      for(std::uint32_t k=0;k<scan_count;++k) {
        auto& state=scan[k];
        const auto* q=base+state.offset;
        if(state.width==1u) {
          for(std::uint32_t n=0;n<state.count;++n) {
            const auto index=std::uint32_t(q[n]);
            state.maximum=std::max(state.maximum,index);
            if(compact) state.minimum=std::min(state.minimum,index);
          }
        } else {
          for(std::uint32_t n=0;n<state.count;++n) {
            const auto index=std::uint32_t(Be16(q+n*2u));
            state.maximum=std::max(state.maximum,index);
            if(compact) state.minimum=std::min(state.minimum,index);
          }
        }
      }
    }
  }
  if(!cached && g_native_index_range_cache && scan_count) {
    GekkoAOT::GX::NativeIndexRangeCache::Bounds bounds;
    for(std::uint32_t k=0;k<scan_count;++k) {
      bounds.minimum[k]=scan[k].minimum;bounds.maximum[k]=scan[k].maximum;
    }
    g_native_index_ranges.Insert(&cache_layout,sizeof cache_layout,vertices,vertex_bytes,bounds);
  }
  static unsigned logs=0;
  std::uint32_t scanned=0;
  for(std::uint32_t f=0;f<layout->field_count;++f) {
    if(skip_scan[f]) continue;
    const auto& field=layout->fields[f];
    const auto& bounds=scan[scanned++];
    const int i=field.attr;
    auto& array=g_gxState.arrays[i];
    if(!array.data) return false;

    const std::uint64_t required=
        std::uint64_t(bounds.maximum)*array.stride+field.element_size;
    const std::uint32_t available=g_array_available[i];
    if(required>available || required>std::numeric_limits<std::uint32_t>::max()) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_ARRAY_WINDOW_V2=0 attr=%d max_index=%u stride=%u required=%llu available=%u action=reject\n",
                   i,bounds.maximum,unsigned(array.stride),
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
                     i,bounds.maximum,unsigned(array.stride),required32,available);
      }
    }
    if(compact && (field.index_width==2u || g_native_array_snapshots)) {
      (*compact)[i]=GekkoAOT::GX::MakeCompactArrayWindow(
          bounds.minimum,bounds.maximum,array.stride,field.element_size,
          available,array.size,g_native_array_snapshots);
      if(g_native_array_tiles)
        (*compact)[i]=GekkoAOT::GX::TileCompactArrayWindow((*compact)[i],available,array.size);
    }
  }

  if(g_indexed_draw_coherency || g_native_array_snapshots) {
    bool any=false;
    for(std::uint32_t f=0;f<layout->field_count;++f) {
      const auto attr=layout->fields[f].attr;
      // BuildIndexScanLayout proves one field per attribute (NBT3 keeps its
      // three indices in that single field). No per-draw duplicate map.
      auto& array=g_gxState.arrays[attr];
      if(!g_indexed_draw_coherency && (!g_native_array_snapshots || array.size>8u*1024u)) continue;
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

bool GuestRangeOverlaps(std::uint32_t address, std::uint32_t size,
                        std::uint32_t base, std::uint64_t bytes) noexcept {
  if(size==0u || bytes==0u) return false;
  const std::uint64_t range_begin=NormalizeGuestCacheAddress(address);
  const std::uint64_t range_end=range_begin+size;
  const std::uint64_t object_begin=NormalizeGuestCacheAddress(base);
  const std::uint64_t object_end=object_begin+bytes;
  return range_begin<object_end && object_begin<range_end;
}

constexpr std::uint32_t PersistentArrayBytesV161H = 64u * 1024u * 1024u;

std::uint32_t AlignPersistentArraySize(std::uint32_t value) noexcept {
  constexpr std::uint32_t kAlign = 256u;
  return (value + kAlign - 1u) & ~(kAlign - 1u);
}

void BeginPersistentArrayFrameV161H(std::uint32_t frame) {
  if (g_persistent_array_frame == frame) return;
  g_persistent_array_frame = frame;
  if (!g_persistent_array_reset_pending) return;

  g_persistent_array_cursor = static_cast<std::uint32_t>(aurora::gfx::StorageBufferSize);
  for (auto& slot : g_persistent_array_slots) slot = {};
  for (auto& array : aurora::gx::g_gxState.arrays) array.cachedRange = {};
  aurora::gx::g_gxState.dirty |= aurora::gx::DirtyImmediates;
  g_persistent_array_reset_pending = false;
  std::fprintf(stderr,
               "GEKKOAOT_AURORA_ARRAY_CACHE_V178=1 action=reset frame=%u base=%u bytes=%u\n",
               frame, static_cast<unsigned>(aurora::gfx::StorageBufferSize),
               PersistentArrayBytesV161H);
}

bool UploadPersistentArrayV161H(std::uint32_t attr, std::uint32_t frame) {
  using namespace aurora::gx;
  if (!g_persistent_array_cache || attr >= GX_VA_MAX_ATTR) return false;
  auto& array = g_gxState.arrays[attr];
  // v178: the upper storage region survives frame rotation and guest-memory
  // writes already invalidate cachedRange through NotifyGuestMemoryWriteRange.
  // Promote medium/static indexed arrays too: otherwise the same 8-64 KiB
  // window is memcpy'd through push_storage every frame. Tiny scratch arrays
  // stay on mapped per-frame staging where Queue.WriteBuffer would cost more.
  if (array.size < g_persistent_array_min_bytes) {
    ++g_persistent_array_fallbacks;
    return false;
  }
  if (array.cachedRange.size != 0u) {
    ++g_persistent_array_hits;
    return true;
  }
  if (array.data == nullptr || array.size == 0u) return false;

  const std::uint32_t padded = AlignPersistentArraySize(array.size);
  const std::uint32_t base = static_cast<std::uint32_t>(aurora::gfx::StorageBufferSize);
  const std::uint32_t limit = base + PersistentArrayBytesV161H;
  auto& slot = g_persistent_array_slots[attr];
  // Guest invalidation remains authoritative. Once invalidated, exact byte
  // equality can prove that the previous immutable GPU snapshot is still the
  // right one. No hash or address/SDK assumption can admit different data.
  if (g_content_array_cache && slot.snapshot && slot.offset >= base &&
      slot.capacity >= padded && slot.snapshot_size >= array.size &&
      std::memcmp(slot.snapshot.get(), array.data, array.size) == 0) {
    array.cachedRange = {slot.offset, array.size};
    slot.last_frame = frame;
    ++g_content_array_hits;
    return true;
  }
  if (g_adaptive_array_upload && !g_array_upload_policy[attr].UsePersistent(frame)) {
    ++g_persistent_array_fallbacks;
    ++g_adaptive_array_fallbacks;
    return false;
  }
  const bool reusable = slot.offset >= base && slot.capacity >= padded &&
                        slot.last_frame != frame;
  std::uint32_t offset = reusable ? slot.offset : g_persistent_array_cursor;

  if (!reusable) {
    if (padded > PersistentArrayBytesV161H || offset > limit || padded > limit - offset) {
      ++g_persistent_array_fallbacks;
      g_persistent_array_reset_pending = true;
      return false;
    }
    g_persistent_array_cursor = offset + padded;
  }

  const std::size_t write_size =
      (static_cast<std::size_t>(array.size) + 3u) & ~std::size_t{3u};
  // All payload bytes are overwritten; initialize only the 0-3 copy-alignment
  // padding bytes. The slot and queued upload share this immutable snapshot.
  std::shared_ptr<std::uint8_t[]> snapshot(new std::uint8_t[write_size]);
  std::memcpy(snapshot.get(), array.data, array.size);
  std::memset(snapshot.get() + array.size, 0, write_size - array.size);
  auto storage = aurora::gfx::detail::resources().storageBuffer;

  aurora::gfx::render_worker::enqueue_work(
      [storage = std::move(storage), offset, snapshot, write_size]() mutable {
        aurora::webgpu::g_queue.WriteBuffer(storage, offset, snapshot.get(), write_size);
      });

  slot.offset = offset;
  slot.capacity = padded;
  slot.identity = array.data;
  slot.last_frame = frame;
  slot.snapshot = std::move(snapshot);
  slot.snapshot_size = array.size;
  g_array_upload_policy[attr].Uploaded(frame);
  array.cachedRange = {offset, array.size};
  ++g_persistent_array_uploads;
  g_persistent_array_upload_bytes += array.size;

  if (g_persistent_array_uploads <= 32u) {
    std::fprintf(stderr,
                 "GEKKOAOT_AURORA_ARRAY_CACHE_V178=1 action=upload attr=%u frame=%u offset=%u bytes=%u cap=%u reused=%u\n",
                 attr, frame, offset, array.size, padded, reusable ? 1u : 0u);
  }
  return true;
}

void PrimePersistentIndexedArraysV161H(std::uint32_t compact_mask,
                                     const GekkoAOT::GX::NativeAttributePlan& bindings) {
  using namespace aurora::gx;
  if (!g_persistent_array_cache) return;
  const auto frame = aurora::gfx::current_frame();
  // Recycle before ANY draw in the new frame uses an old persistent range.
  // A delayed reset after a cached draw would let an upload overwrite it.
  BeginPersistentArrayFrameV161H(frame);
  bindings.ForEach(g_native_attribute_plan,[&](unsigned i) {
    if((compact_mask & (1u << i))!=0u) return;
    const auto desc = g_gxState.vtxDesc[i];
    if (desc != GX_INDEX8 && desc != GX_INDEX16) return;
    auto& array = g_gxState.arrays[i];
    if (array.cachedRange.size != 0u) {
      auto& slot = g_persistent_array_slots[i];
      // A surviving snapshot can be used in a later frame without an upload.
      // Record that use: a subsequent mutation in this SAME frame must receive
      // another GPU range, or it would change geometry of the earlier draw.
      if (array.cachedRange.offset >= aurora::gfx::StorageBufferSize &&
          array.cachedRange.offset == slot.offset)
        slot.last_frame = frame;
      return;
    }
    if (array.size == 0u || array.data == nullptr) return;
    (void)UploadPersistentArrayV161H(static_cast<std::uint32_t>(i), frame);
  });
}

std::uint32_t SelectCompactArrayMask(const CompactArrayPlan& plan,
                                   const GekkoAOT::GX::NativeAttributePlan& bindings) {
  using namespace aurora::gx;
  std::uint32_t mask=0;
  bindings.ForEach(g_native_attribute_plan,[&](unsigned attr) {
    if(plan[attr] && g_gxState.arrays[attr].cachedRange.size==0u)
      mask|=1u<<attr;
  });
  return mask;
}

// Reserve before uploading any compact array. The shader receives a wrapping
// u32 bias, never a host pointer/offset or a WebGPU binding range. Clear these
// one-draw ranges immediately after submission; frame rotation and storage
// rollover must never interpret the bias as a persistent storage allocation.
struct CompactArrayDraw {
  std::uint32_t mask=0;
  void Clear() {
    if(!mask) return;
    for(unsigned attr=GX_VA_POS;attr<=GX_VA_TEX7;++attr)
      if((mask&(1u<<attr))!=0u) aurora::gx::g_gxState.arrays[attr].cachedRange={};
    aurora::gx::g_gxState.dirty|=aurora::gx::DirtyImmediates;
    mask=0;
  }
  ~CompactArrayDraw() { Clear(); }

  void Prepare(const CompactArrayPlan& plan,const GekkoAOT::GX::NativeAttributePlan& bindings) {
    using namespace aurora::gx;
    auto selected=SelectCompactArrayMask(plan,bindings);
    PrimePersistentIndexedArraysV161H(selected,bindings);
    if(!selected && !g_native_array_snapshots) return;
    const auto projected=[&](std::uint32_t candidate) {
      std::uint64_t end=aurora::gfx::storage_size();
      const auto alignment=aurora::gfx::storage_alignment();
      const auto append=[&](std::uint64_t bytes) {
        if(!bytes) return;
        // Conservative for every upload order, including compact arrays first.
        end+=(alignment ? alignment-1u : 0u)+bytes;
      };
      bindings.ForEach(g_native_attribute_plan,[&](unsigned attr) {
        const auto& array=g_gxState.arrays[attr];
        const auto desc=g_gxState.vtxDesc[attr];
        if((desc!=GX_INDEX8 && desc!=GX_INDEX16) || array.cachedRange.size) return;
        append((candidate&(1u<<attr))!=0u ? plan[attr].bytes : array.size);
      });
      // Aurora may build/change its fog LUT while choosing the shader/pipeline.
      // Its exact maximum payload is one f32 per render-target pixel column.
      append(std::uint64_t(aurora::gfx::get_render_target_size().x)*sizeof(float));
      return end;
    };
    auto required=projected(selected);
    if(required>aurora::gfx::storage_capacity()) {
      if(!aurora::gfx::rollover_storage_pass(static_cast<std::size_t>(required))) {
        ++g_compact_array_fallbacks;
        PrimePersistentIndexedArraysV161H(0,bindings);
        return;
      }
      ++g_compact_array_rollovers;
      selected=SelectCompactArrayMask(plan,bindings);
      PrimePersistentIndexedArraysV161H(selected,bindings);
      required=projected(selected);
      if(required>aurora::gfx::storage_capacity()) {
        ++g_compact_array_fallbacks;
        PrimePersistentIndexedArraysV161H(0,bindings);
        return;
      }
    }
    bool prepared=false;
    bool failed=false;
    bindings.ForEach(g_native_attribute_plan,[&](unsigned attr) {
      if(failed) return;
      const bool compact=(selected&(1u<<attr))!=0u;
      auto& array=g_gxState.arrays[attr];
      const auto desc=g_gxState.vtxDesc[attr];
      // v303: short full arrays (including INDEX8 and low INDEX16 indices)
      // need the same immutable resource identity as compact suffixes. Stage
      // them before draw selection so native batching sees validated bases.
      if(!compact && (!g_native_array_snapshots || array.cachedRange.size || !array.data ||
          !array.size || array.size>8u*1024u || (desc!=GX_INDEX8 && desc!=GX_INDEX16))) return;
      const auto window=compact ? plan[attr] : GekkoAOT::GX::CompactArrayWindow{0,array.size};
      const auto range=aurora::gfx::push_storage_snapshot(
          static_cast<const std::uint8_t*>(array.data)+window.prefix,window.bytes,!compact);
      if(range.size!=window.bytes) {
        ++g_compact_array_fallbacks;
        Clear();
        PrimePersistentIndexedArraysV161H(0,bindings);
        failed=true;
        return;
      }
      array.cachedRange={GekkoAOT::GX::CompactArrayShaderBase(range.offset,window.prefix),
                         window.bytes};
      prepared=true;
      if(compact) {
        mask|=1u<<attr;
        ++g_compact_array_uploads;
        g_compact_array_bytes+=window.bytes;
        g_compact_array_prefix_bytes+=array.size;
      } else {
        ++g_native_array_snapshot_stages;
      }
    });
    if(prepared) g_gxState.dirty|=DirtyImmediates;
  }
};

// v318: one native resource allocation/copy for all indexed attributes of a
// short draw, without min/max window uploads or per-attribute snapshot lookup.
// Rewritten indices live exclusively in host memory. CP state, guest arrays
// and display-list bytes keep their original representation.
struct GatheredArrayDraw {
  const std::uint8_t* vertices=nullptr;
  std::uint32_t mask=0;
  ~GatheredArrayDraw() {
    if(!mask) return;
    for(unsigned attr=GX_VA_POS;attr<=GX_VA_TEX7;++attr)
      if(mask&(1u<<attr)) aurora::gx::g_gxState.arrays[attr].cachedRange={};
    aurora::gx::g_gxState.dirty|=aurora::gx::DirtyImmediates;
  }
  bool Prepare(const std::uint8_t* p,std::size_t len,GXVtxFmt fmt,unsigned count) {
    using namespace aurora::gx;
    if(!g_native_vertex_gather ||
       count>GekkoAOT::GX::NativeVertexGather::MaxVertices) return false;
    const auto* layout=GetIndexScanLayout(fmt);
    if(!layout || !layout->gather.count) return false;
    std::array<GekkoAOT::GX::NativeVertexGather::Source,4> sources;
    for(unsigned f=0;f<layout->field_count;++f) {
      const auto attr=layout->fields[f].attr;
      const auto& array=g_gxState.arrays[attr];
      sources[f]={static_cast<const std::uint8_t*>(array.data),g_array_available[attr],array.stride};
    }
    GekkoAOT::GX::NativeVertexGather::Result gathered;
    if(!layout->gather.Execute({p+3,len-3},count,{sources.data(),layout->field_count},
                              g_native_gather_workspace,gathered)) return false;
    const auto alignment=aurora::gfx::storage_alignment();
    const auto padding=alignment ? alignment-1u : 0u;
    const auto required=aurora::gfx::storage_size()+padding+
        gathered.bytes+padding+
        std::size_t(aurora::gfx::get_render_target_size().x)*sizeof(float);
    if(required>aurora::gfx::storage_capacity() &&
       !aurora::gfx::rollover_storage_pass(required)) return false;
    const auto uploaded=aurora::gfx::push_storage(g_native_gather_workspace.storage.data(),gathered.bytes);
    if(uploaded.size!=gathered.bytes) return false;
    for(unsigned f=0;f<layout->field_count;++f) {
      const auto attr=layout->fields[f].attr;
      auto& array=g_gxState.arrays[attr];
      const auto& range=gathered.ranges[f];
      array.size=std::max(array.size,range.maximum*array.stride+layout->fields[f].element_size);
      array.cachedRange={uploaded.offset+range.offset,range.bytes};
      mask|=1u<<attr;
    }
    vertices=g_native_gather_workspace.vertices.data();
    g_gxState.dirty|=DirtyImmediates;
    ++g_native_gather_draws;
    g_native_gather_bytes+=gathered.bytes;
    g_native_gather_windows+=gathered.source_window_bytes;
    return true;
  }
};

void NotifyGuestMemoryWriteRange(std::uint32_t address, std::uint32_t size) {
  using namespace aurora::gx;
  if(size==0u) return;
  ForgetTextureSourceRange(address,size);

  unsigned invalidated_arrays=0u;
  for(int i=GX_VA_POS;i<GX_VA_MAX_ATTR;++i) {
    auto& array=g_gxState.arrays[i];
    if(array.cachedRange.size==0u || array.size==0u || array.data==nullptr) continue;
    if(!GuestRangeOverlaps(address,size,g_array_guest_base[i],array.size)) continue;
    array.cachedRange={};
    ++invalidated_arrays;
    ++g_array_cache_invalidations;
  }
  if(invalidated_arrays!=0u)
    g_gxState.dirty |= DirtyImmediates;

  unsigned invalidated_textures=0u;
  for(unsigned map=0;map<MaxTextures;++map) {
    const auto bit=static_cast<std::uint8_t>(1u<<map);
    if((g_texture_image3_valid&bit)==0u) continue;
    auto& slot=g_gxState.loadedTextures[map];
    if(slot.data==nullptr) continue;
    const std::uint32_t guest=(g_texture_image3_word[map]&0x00ffffffu)<<5u;
    const auto span=texture::texture_source_size(slot.format(),slot.width(),slot.height(),slot.mip_count());
    if(span==0u || !GuestRangeOverlaps(address,size,guest,span)) continue;
    slot.texDataVersion=static_cast<std::uint32_t>(++g_texture_generation);
    ++invalidated_textures;
    ++g_texture_memory_invalidations;
  }
  if(invalidated_textures!=0u) {
    texture::invalidate_bindings();
    g_gxState.dirty |= DirtyTextures;
  }

  if((invalidated_arrays!=0u || invalidated_textures!=0u) &&
     g_array_cache_control_logs++<64u) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_MEMORY_COHERENCY_V159=1 ea=%08x bytes=%u arrays=%u textures=%u\n",
                 address,size,invalidated_arrays,invalidated_textures);
  }
}

void NotifyGuestVertexCacheControl(std::uint8_t operation, std::uint32_t address) {
  // 0/1/2 are dcbst/dcbf/dcbi in HostRuntime. GekkoAOT's RAM is host-coherent,
  // but these guest cache-management instructions are the publication signal
  // for CPU-authored GX arrays and textures. Invalidate only the touched line.
  if(operation>2u) return;
  ++g_array_cache_control_events;
  NotifyGuestMemoryWriteRange(address&~31u,32u);
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

GekkoAOT::GX::TextureViewportRoles g_texture_viewport_roles;

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
  const unsigned shift=Bits(word,9u,1u);
  g_gxState.texCopyDstWidth=std::max(1u,static_cast<unsigned>(g_gxState.texCopySrc.width)>>shift);
  g_gxState.texCopyDstHeight=std::max(1u,static_cast<unsigned>(g_gxState.texCopySrc.height)>>shift);
  g_gxState.texCopyFmt=format;
  const auto display=aurora::vi::configured_fb_size();
  const auto& view=g_gxState.logicalViewport;
  g_texture_viewport_roles.Observe(view.left,view.top,view.width,view.height,
      g_gxState.texCopySrc.x,g_gxState.texCopySrc.y,g_gxState.texCopySrc.width,
      g_gxState.texCopySrc.height,float(display.x),float(display.y),
      g_camera_copy_epoch.load(std::memory_order_relaxed));
  g_display_insets.NoteCopy(float(g_gxState.texCopySrc.x),float(g_gxState.texCopySrc.width),
                            g_display_copy_count);
  g_copy_provenance.NoteCopy(g_gxState.texCopyDest,float(g_gxState.texCopySrc.x),float(g_gxState.texCopySrc.y),
                             float(g_gxState.texCopySrc.width),float(g_gxState.texCopySrc.height),
                             DisplayCanvasWidth(),float(aurora::vi::configured_fb_size().y));
  static unsigned copy_logs=0;
  if(g_trace_gx_copy && copy_logs++<128u)
    std::fprintf(stderr,"GEKKOAOT_TRACE_GX_COPY event=texture-copy width=%u height=%u half=%u format=%u\n",
                 g_gxState.texCopyDstWidth,g_gxState.texCopyDstHeight,shift,unsigned(format));
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

// v358: publish the uniform blocks of the frame this display copy ends.
void FinishReplayCapture(const XfbZeroCopySlot& slot) {
  auto capture=std::make_shared<ReplayCapture>(std::move(g_replay_building));
  g_replay_building={};
  g_replay_building.blocks.reserve(capture->blocks.size());
  g_replay_building.bytes.reserve(capture->bytes.size());
  g_replay_occurrences.clear();
  capture->frame_id=aurora::gfx::gekkoaot_recording_frame_id();
  capture->produced=std::chrono::steady_clock::now();
  if (const auto* source=XfbPresentSource(slot)) capture->present=*source;
  // Bounding-box draws feed values the guest reads back. (A frame that
  // loads the previous EFB is restored by Aurora before each replay, v360.)
  capture->eligible=capture->frame_id!=0 && capture->present.texture && !capture->blocks.empty() &&
                    !g_replay_frame_bbox;
  g_replay_frame_bbox=false;
  g_replay_captures.fetch_add(1,std::memory_order_relaxed);
  if (capture->eligible) g_replay_eligible.fetch_add(1,std::memory_order_relaxed);
  static unsigned capture_logs=0;
  if (capture_logs++<4u)
    std::fprintf(stderr,"GEKKOAOT_FRAME_REPLAY_CAPTURE_V358 frame=%llu blocks=%zu bytes=%zu present=%u eligible=%u\n",
                 static_cast<unsigned long long>(capture->frame_id),capture->blocks.size(),capture->bytes.size(),
                 capture->present.texture ? 1u : 0u,capture->eligible ? 1u : 0u);
  capture->index.reserve(capture->blocks.size());
  for (std::uint32_t i=0;i<capture->blocks.size();++i) capture->index.emplace(capture->blocks[i].key,i);
  std::lock_guard lock{g_replay_mutex};
  g_replay_previous=std::move(g_replay_current);
  g_replay_current=std::move(capture);
}


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
  source.code = R"WGSL(
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
  return textureSampleLevel(source_tex, tex_sampler, uv, 0.0);
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
  if ((!g_post_processing_enabled && !g_video.fxaa) || !source.texture || !source.view ||
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
      g_video.tint*enabled, float(g_video.fxaa),0,0,0};
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

void PresentFinalTexture(const aurora::webgpu::TextureWithSampler& source) {
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
  if ((g_post_processing_enabled || g_video.fxaa) && PresentUniversalPostFx(source)) return;
  aurora_present_texture(&source);
}

// v361: the display refresh rate (frames above it are never seen; presenting
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

void ObservePerspectiveCamera() {
  using namespace aurora::gx;
  if (g_gxState.projType != GX_PERSPECTIVE || aurora::gfx::is_offscreen()) return;
  const auto& proj = g_gxState.proj;
  const auto& view = g_gxState.logicalViewport;
  if (!(view.width > 0.f) || !(view.height > 0.f)) return;
  if (g_camera_area_epoch != g_display_copy_count) {
    g_camera_area_epoch = g_display_copy_count;
    g_camera_best_area = 0.f;
  }
  const float area = view.width * view.height;
  if (!std::isfinite(area) || area < g_camera_best_area) return;
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
  g_camera_best_area = area;
}

// v300 GEKKOAOT_RUNTIME_PROFILE: averaged Aurora work per display copy, read
// from the stats of the last frame the render worker submitted.
void ReportAuroraFrameStats() {
  {
    // v359: frames that really reached the swapchain (what MangoHud counts).
    static auto last=std::chrono::steady_clock::now();
    static std::uint64_t last_presents=aurora::gfx::presentation_count();
    const auto now=std::chrono::steady_clock::now();
    const double seconds=std::chrono::duration<double>(now-last).count();
    if (seconds>=5.0) {
      const auto presents=aurora::gfx::presentation_count();
      const auto pacing=g_present_pacing;
      g_present_pacing={};
      const double mean=pacing.count ? pacing.sum/double(pacing.count) : 0.0;
      const double jitter=pacing.count ? std::sqrt(std::max(0.0,pacing.sum_sq/double(pacing.count)-mean*mean)) : 0.0;
      std::fprintf(stderr,"GEKKOAOT_DISPLAY_FPS_V359 display_fps=%.1f interval_ms=%.2f jitter_ms=%.2f worst_ms=%.1f replay_captures=%llu eligible=%llu requests=%llu early=%llu period=%llu busy=%llu queued=%llu\n",
                   double(presents-last_presents)/seconds,mean,jitter,pacing.worst,
                   static_cast<unsigned long long>(g_replay_captures.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(g_replay_eligible.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(g_replay_requests.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(g_replay_declined_early.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(g_replay_declined_period.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(g_replay_declined_busy.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(g_replay_queued.load(std::memory_order_relaxed)));
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
  // v349: CPU EFB peeks observe the finished frame, not the fresh pass the
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
  slot->address = g_copy_dest_physical;
  slot->stride_bytes = g_copy_stride_bytes != 0u ? g_copy_stride_bytes : copy_width * 2u;
  slot->native_width = copy_width;
  g_display_copy_width.store(copy_width, std::memory_order_relaxed);
  slot->native_height = copy_height;
  slot->span_bytes = static_cast<std::uint64_t>(slot->stride_bytes) * copy_height;
  slot->generation = ++g_xfb_generation;
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
  if (const bool replay=g_frame_interpolation_enabled && g_replay_mode;
      replay!=g_replay_capture_on.load(std::memory_order_relaxed)) {
    // Switch at a frame boundary; the first captured frame starts next.
    g_replay_capture_on.store(replay,std::memory_order_relaxed);
    aurora::gfx::detail::gekkoaot_replay_set_enabled(replay);
    // v361: replayed frames are presented in step with the display; without
    // vsync several per refresh tore and judder.
    aurora_enable_vsync(replay || g_vsync_default);
    g_replay_building={};
    g_replay_occurrences.clear();
    std::lock_guard lock{g_replay_mutex};
    g_replay_current.reset();
    g_replay_previous.reset();
  } else if (replay) {
    FinishReplayCapture(*slot);
  }
  aurora_end_frame_no_present();
  GekkoAOT::GX::Hud::NoteGameFrame();
  ReportAuroraFrameStats();
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
  PresentFinalTexture(g_ram_xfb_present);

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
                  std::uint32_t field_height = 0u, bool poll_events = true) {
  if (!g_ready) return;
  FlushEfbPokes();
  DrainFifo();
  // v300: the GX worker runs ordered scanouts; SDL events stay on the host
  // thread that owns the window (gekkoaot_native_gx_should_quit).
  if (poll_events) PollEvents();

  // Interlaced VI can expose TOP=0/BOTTOM=0x500 for an XFB whose base is
  // physical zero. Use whichever field origin is non-zero for the cache lookup.
  const std::uint32_t scanout_address = top != 0u ? top : bottom;

  std::uint32_t byte_offset = 0u;
  XfbZeroCopySlot* slot =
      address_keyed && scanout_address != 0u
          ? FindXfbSlotForScanout(scanout_address, &byte_offset)
          : nullptr;
  const auto* source = slot ? XfbPresentSource(*slot) : nullptr;

  if (address_keyed && scanout_address != 0u) {
    if (source == nullptr) {
      aurora::gfx::render_worker::enqueue_work([] { g_cached_scanout.Reset(); });
      if (PresentRamXfb(scanout_address,
                        bottom != 0u ? bottom : scanout_address,
                        width, stride_bytes, field_height))
        return;
      static unsigned misses = 0;
      if (misses++ < 32u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_XFB_SCANOUT_V202=0 request=%08x top=%08x bottom=%08x reason=xfb-not-cached action=hold-last-scanout ram_fallback=failed\n",
                     scanout_address, top, bottom);
      return;
    }

    // v110: presentation is a pure consumer.  The producer frame containing
    // this immutable XFB was already submitted at GXCopyDisp.  Never end or
    // restart the live EFB here: VI can legally scan the same generation on
    // multiple field/retrace boundaries.
    // VI keeps observing RAM scanout and field changes independently of GX
    // production. Avoid GPU work only for the same immutable GX snapshot and
    // the same host view. Resize/aspect/resume must redraw even a static image.
    if (!aurora::window::is_presentable()) {
      aurora::gfx::render_worker::enqueue_work([] { g_cached_scanout.Reset(); });
      return;
    }
    const auto window_size = aurora::window::get_window_size();
    // v359: the whole cached copy is presented whichever field VI selects, so
    // the field origins are not part of the image. Keying on them presented
    // every interlaced frame twice (30 fps titles showed 60 in MangoHud).
    const GekkoAOT::GX::CachedScanoutKey scanout{
        reinterpret_cast<std::uintptr_t>(source->texture.Get()), slot->generation,
        0u, 0u, window_size.native_fb_width, window_size.native_fb_height,
        slot->force_43 ? 0u : g_aspect_mode.load(std::memory_order_acquire)};
    // Cache state belongs exclusively to the render worker. A failed surface
    // acquisition/present is retried at the next VI field; only success allows
    // deduplication. This preserves pipelining without a new CPU/GPU wait.
    slot->presented_generation = slot->generation;
    aurora::gfx::render_worker::enqueue_work([retained = *source, scanout, force_43 = slot->force_43] {
      if (g_cached_scanout.Duplicate(scanout)) return;
      const auto before = aurora::gfx::presentation_count();
      g_present_force_43 = force_43;
      PresentFinalTexture(retained);
      g_present_force_43 = false;
      if (aurora::gfx::presentation_count() != before)
        g_cached_scanout.Queued(scanout);
    });

    const std::uint32_t row_offset =
        slot->stride_bytes != 0u ? byte_offset / slot->stride_bytes : 0u;
    static unsigned logs = 0;
    if (logs++ < 64u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_XFB_SCANOUT_V202=1 request=%08x top=%08x bottom=%08x base=%08x offset=%u row=%u native=%ux%u stride=%u generation=%llu source=vi-selected-aurora-xfb-cache\n",
                   scanout_address, top, bottom, slot->address, byte_offset, row_offset,
                   slot->native_width, slot->native_height, slot->stride_bytes,
                   static_cast<unsigned long long>(slot->generation));
    return;
  }

  // Legacy/boot path only, before VI has supplied a non-zero XFB address.
  aurora::gfx::render_worker::enqueue_work([] { g_cached_scanout.Reset(); });
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
  g_texture_image3_word[map] = word;
  g_texture_image3_written = static_cast<std::uint8_t>(g_texture_image3_written | bit);
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
    g_texture_ui_source_hash_valid =
        static_cast<std::uint8_t>(g_texture_ui_source_hash_valid & ~bit);
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

  static unsigned video_logs=0;
  if(g_trace_video && video_logs++<256u) {
    const auto* bytes=static_cast<const std::uint8_t*>(ptr);
    std::uint64_t hash=1469598103934665603ull;
    for(unsigned i=0;i<required;++i) hash=(hash^bytes[i])*1099511628211ull;
    std::fprintf(stderr,"GEKKOAOT_TRACE_VIDEO event=texture map=%u guest=%08x format=%u width=%u height=%u bytes=%u hash=%016llx\n",
                 map,guest,unsigned(slot.format()),slot.width(),slot.height(),required,
                 static_cast<unsigned long long>(hash));
  }

  // Games commonly re-emit the same IMAGE3 state for every draw. Treat that as
  // a binding, not a texture mutation. Aurora can then reuse its converted GPU
  // resource instead of new_static_texture_2d/convert/upload on every bind.
  const bool stable_bind=(g_texture_image3_valid&bit)!=0u &&
                         g_texture_mapped_image0[map]==slot.image0 &&
                         g_texture_mapped_mips[map]==slot.mip_count() && slot.data==ptr;
  // v174: small intensity/alpha and CI textures are the formats commonly used
  // by retail bitmap-font atlases. Terminator 3 rewrites some of those atlases
  // in place while retaining IMAGE3, so a pointer-only stable-bind cache misses
  // the mutation. v170 fixed that by versioning *every* such rebind, which also
  // turns Monkey Ball's bitmap-heavy UI into constant texture churn. Hash these
  // source bytes only on the low-bit stable-bind path and invalidate when their
  // contents actually changed. Normal material formats keep the zero-hash fast
  // path, while T3 still observes real in-place font updates.
  const auto source_format=slot.format();
  const bool font_ui_format=
      source_format==GX_TF_I4 || source_format==GX_TF_I8 ||
      source_format==GX_TF_IA4 || source_format==GX_TF_IA8 ||
      source_format==GX_TF_C4 || source_format==GX_TF_C8 ||
      source_format==GX_TF_C14X2;
  bool font_ui_changed=false;
  // v34: only hash low-bit/font/video textures when IMAGE3 is actually a
  // stable rebind.  The old v174 code hashed every I4/I8/IA/CI IMAGE3 write,
  // including ordinary texture switches and the three I8 Y/U/V planes used by
  // retail video paths.  That made MapTextureImage3 itself a double-digit CPU
  // hotspot.  A non-stable bind already bumps texDataVersion and uploads the
  // new source, so hashing it cannot improve coherency.  On the first stable
  // rebind we establish a baseline; later stable rebinds still detect genuine
  // in-place bitmap/font mutations exactly as v174 intended.
  const bool paranoid_texture_hash=g_paranoid_texture_hash;
  if(font_ui_format && (stable_bind || paranoid_texture_hash)) {
    const auto* bytes=static_cast<const std::uint8_t*>(ptr);
    std::uint64_t hash=1469598103934665603ull;
    for(std::uint32_t i=0;i<required;++i) hash=(hash^bytes[i])*1099511628211ull;
    const bool had_hash=(g_texture_ui_source_hash_valid&bit)!=0u;
    font_ui_changed=had_hash && g_texture_ui_source_hash[map]!=hash;
    g_texture_ui_source_hash[map]=hash;
    g_texture_ui_source_hash_valid=
        static_cast<std::uint8_t>(g_texture_ui_source_hash_valid|bit);
  } else if(font_ui_format) {
    // The pointer/descriptor changed, so the previous map hash belongs to a
    // different texture identity.  Avoid comparing it against the first stable
    // rebind of the new source.
    g_texture_ui_source_hash_valid=
        static_cast<std::uint8_t>(g_texture_ui_source_hash_valid&~bit);
  }
  if(stable_bind && g_texture_bind_cache && (!font_ui_format || !font_ui_changed)) return;

  g_texture_mapped_image0[map] = slot.image0;
  g_texture_mapped_mips[map] = slot.mip_count();
  g_texture_image3_valid=static_cast<std::uint8_t>(g_texture_image3_valid|bit);
  slot.data=ptr;
  // Keep object identity stable per hardware texture unit. With the retail
  // IMAGE3 cache disabled for correctness, assigning a fresh texObjId on every
  // bind makes Aurora's object cache grow by one entry per draw and can reach
  // several GiB in minutes. texDataVersion is the mutation/version signal; the
  // object ID is identity, not a generation counter.
  slot.texObjId=static_cast<std::uint32_t>(map+1u);
  const auto generation=static_cast<std::uint32_t>(++g_texture_generation);
  // Font/CI sources keep a fresh version per rebind (v174 in-place updates).
  slot.texDataVersion=font_ui_format ? generation :
      BindTextureSourceVersion(guest,slot.image0,static_cast<std::uint8_t>(slot.mip_count()),
                               required,generation);
  if(g_draw_census) { ++g_tex_census_binds; g_tex_census_bind_bytes+=required; }
  slot.flags &= ~0x80u;
  g_gxState.dirty |= DirtyTextures;
}

void RefreshTextureMappings() {
  for (unsigned map = 0; map < aurora::gx::MaxTextures; ++map) {
    const auto bit = static_cast<std::uint8_t>(1u << map);
    if ((g_texture_image3_written & bit) == 0) continue;
    const auto& slot = aurora::gx::g_gxState.loadedTextures[map];
    if ((g_texture_image3_valid & bit) == 0 ||
        g_texture_mapped_image0[map] != slot.image0 ||
        g_texture_mapped_mips[map] != slot.mip_count()) {
      // IMAGE0/MODE1 can be changed without writing IMAGE3. Validate the new
      // complete source span at draw time, after the descriptor is installed.
      MapTextureImage3(static_cast<std::uint8_t>((map < 4 ? 0x94 : 0xb4) + (map & 3)),
                       g_texture_image3_word[map]);
    }
  }
}

GekkoAOT::GX::TlutMemory g_tlut_memory;
std::array<std::uint64_t, 8> g_tlut_revisions{};
std::array<std::uint32_t, 8> g_tlut_bindings{};

void MapTlut() {
  using namespace aurora::gx;
  const auto load=g_gxState.bpRegCache[0x64];
  const auto trigger=g_gxState.bpRegCache[0x65];
  const std::uint32_t guest=GekkoAOT::GX::TlutMemory::SourceAddress(load);
  const auto required=GekkoAOT::GX::TlutMemory::UploadBytes(trigger);
  const void* ptr=nullptr; std::uint32_t available=0;
  const bool valid=required==0u ||
      (Resolve(guest,required,&ptr,&available) && available>=required);
  const bool copied=valid && g_tlut_memory.Load(trigger,
      {static_cast<const std::uint8_t*>(ptr),valid ? required : 0u});
  static unsigned logs=0;
  if(g_trace_font && logs++<128u)
    std::fprintf(stderr,"GEKKOAOT_TRACE_FONT event=tlut-load guest=%08x tmem=%05x bytes=%u success=%u revision=%llu\n",
                 guest,GekkoAOT::GX::TlutMemory::Offset(trigger),required,copied?1u:0u,
                 static_cast<unsigned long long>(g_tlut_memory.Revision()));
  if(!copied) { g_fifo_fault=true; g_quit=true; }
  else g_tlut_binding_scan_dirty=true;
}

void BindRawTluts() {
  using namespace aurora::gx;
  for(unsigned map=0;map<MaxTextures;++map) {
    auto& texture=g_gxState.loadedTextures[map];
    const auto format=texture.format();
    const unsigned entries=format==GX_TF_C4 ? 16u : format==GX_TF_C8 ? 256u :
                           format==GX_TF_C14X2 ? 16384u : 0u;
    if(!entries) continue;
    const unsigned reg=0x98u+(map&3u)+(map>=4u ? 0x20u : 0u);
    const auto binding=g_gxState.bpRegCache[reg]&0xfffu;
    // Include texture format: the same TMEM address can be rebound as CI4/CI8.
    const auto key=binding|(format<<16u);
    if(g_tlut_revisions[map]==g_tlut_memory.Revision()+1u && g_tlut_bindings[map]==key) continue;
    const auto bytes=g_tlut_memory.Palette(binding,entries);
    auto& palette=g_gxState.loadedTluts[map];
    palette.data=bytes.empty() ? nullptr : bytes.data();
    palette.numEntries=static_cast<std::uint16_t>(entries);
    palette.format=static_cast<GXTlutFmt>((binding>>10u)&3u);
    palette.tlutObjId=map+1u;
    palette.tlutDataVersion=static_cast<std::uint32_t>(++g_texture_generation);
    texture.tlut=static_cast<GXTlut>(map);
    g_tlut_bindings[map]=key;
    g_tlut_revisions[map]=g_tlut_memory.Revision()+1u;
    g_gxState.dirty |= DirtyTextures;
    aurora::gx::texture::invalidate_bindings();
  }
}

void InvalidateTextureCacheV87() {
  using namespace aurora::gx;
  // v237: direct/uncached guest stores can mutate MEM1 without a dcbst/dcbf
  // publication callback. Invalidating TMEM must therefore make the next
  // sampled static texture check its source again. Otherwise Bink movies
  // retain their initial black frame. Coalesce the SDK's paired BP66 pulses
  // until the next draw; Aurora reuses its content cache if bytes are equal.
  // GPU-owned EFB copy textures retain their separate copy-texture identity.
  unsigned bound=0u;
  for(unsigned map=0;map<MaxTextures;++map) {
    auto& texture=g_gxState.loadedTextures[map];
    if(!texture.data) continue;
    ++bound;
  }
  if(g_invalidate_texture_sources) g_texture_source_invalidation_pending=true;
  ++g_texture_cache_only_invalidations;
  if(g_draw_census) ++g_tex_census_bp66;
  ForgetTextureSourceVersions();
  texture::invalidate_bindings();
  g_gxState.dirty |= DirtyTextures;
  static unsigned logs=0;
  if(logs++<64u)
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_TEX_INVALIDATE_V237=1 reg=66 bound=%u source_recheck=%u policy=coalesce-until-draw\n",
                 bound,g_invalidate_texture_sources?1u:0u);
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
bool ProcessOne(const std::uint8_t* p, std::size_t len, unsigned depth);

// v320: draw-run coalescing. Retail engines (HPCOS: ~1.5M draws/s, 78% with
// <=4 vertices, 95% directly following the previous draw) emit long runs of
// tiny identical-format draws with no command in between. GX state cannot
// change inside such a run, so one draw over the concatenated vertices is
// equivalent: list primitives are concatenated, strips/fans become triangle
// lists using exactly Aurora's own vertex order (same winding/culling and
// rasterization order). The merged draw then needs one array window, one
// upload and one recorded/encoded draw instead of one per GXBegin.
// v323: members only need the same vertex format. A run of one list primitive
// (quads or triangles) stays that primitive and is a plain concatenation; any
// strip/fan member or a mix of primitives becomes one triangle list, using
// Aurora's prepare_idx_buffer order for every source primitive (quads as
// (0,1,2)(2,3,0)), so winding, culling and rasterization order are unchanged.
// Strips/fans are expanded vertex by vertex, which only pays for small ones;
// list members are copied as one block and may be larger.
constexpr std::uint32_t kDrawRunExpandMax = 64u;
constexpr std::uint32_t kDrawRunListMax = 256u;
constexpr std::uint32_t kDrawRunMaxVertices = 16384u;
bool g_draw_runs = true;
std::vector<std::uint8_t> g_draw_run_buffer;

std::uint32_t DrawRunTriangleVertices(std::uint8_t op, std::uint32_t count) {
  switch (op) {
  case GX_QUADS: return count / 4u * 6u;
  case GX_TRIANGLESTRIP:
  case GX_TRIANGLEFAN: return 3u * (count - 2u);
  default: return count;
  }
}
bool DrawRunMember(std::uint8_t op, std::uint32_t count) {
  if (count == 0u) return false;
  switch (op) {
  case GX_QUADS: return count <= kDrawRunListMax && count % 4u == 0u;
  case GX_TRIANGLES: return count <= kDrawRunListMax && count % 3u == 0u;
  case GX_TRIANGLESTRIP:
  case GX_TRIANGLEFAN: return count >= 3u && count <= kDrawRunExpandMax;
  default: return false;  // lines/points keep Aurora's dedicated paths
  }
}
bool DrawRunStateAllows() {
  using namespace aurora::gx;
  const auto& state = g_gxState;
  // A lone 4/6-vertex orthographic draw may be a screen fill whose
  // classification feeds uniforms; never fold one into a larger draw.
  return !(state.projType == GX_ORTHOGRAPHIC && state.numTexGens == 1u &&
           state.numTevStages <= 2u && state.blendMode == GX_BM_NONE);
}
// Copy one vertex of a run member. Strides are small and vary per VAT, so a
// libc memcpy call per expanded vertex dominated the run assembly; two
// overlapping fixed-size moves cover every stride from 4 to 64 bytes.
inline void CopyRunVertex(std::uint8_t* dst, const std::uint8_t* src, std::uint32_t stride) {
  const auto pair = [&]<std::size_t N>() {
    std::uint8_t head[N], tail[N];
    std::memcpy(head, src, N);
    std::memcpy(tail, src + stride - N, N);
    std::memcpy(dst, head, N);
    std::memcpy(dst + stride - N, tail, N);
  };
  if (stride >= 32u && stride <= 64u) pair.template operator()<32>();
  else if (stride >= 16u && stride < 32u) pair.template operator()<16>();
  else if (stride >= 8u && stride < 16u) pair.template operator()<8>();
  else if (stride >= 4u && stride < 8u) pair.template operator()<4>();
  else std::memcpy(dst, src, stride);
}
bool IsDrawCommand(std::uint8_t cmd) {
  const std::uint8_t op = cmd & GX_OPCODE_MASK;
  return op >= 0x80u && op <= 0xB8u;
}

// Process the framed command at data[0..len); if it starts a run of small
// draws in the same vertex format, consume the whole run (bounded by
// `available`). Same failure contract as ProcessOne.
bool ProcessCommandOrRun(const std::uint8_t* data, std::size_t available, std::size_t len,
                         unsigned depth, std::size_t* consumed) {
  *consumed = len;
  const std::uint8_t cmd = data[0], op = cmd & GX_OPCODE_MASK;
  if (!g_draw_runs || !IsDrawCommand(cmd) || len < 3u || len + 3u > available ||
      !IsDrawCommand(data[len]))
    return ProcessOne(data, len, depth);
  const std::uint8_t vat = cmd & GX_VAT_MASK;
  const std::uint32_t first = Be16(data + 1);
  const auto miss = [&](unsigned reason) {
    if (g_draw_census) ++g_draw_run_misses[reason];
    return ProcessOne(data, len, depth);
  };
  if ((data[len] & GX_VAT_MASK) != vat) return miss(0u);
  if (op == GX_LINES || op == GX_LINESTRIP || op == GX_POINTS) return miss(3u);
  if (!DrawRunMember(op, first)) return miss(1u);
  if (!DrawRunStateAllows()) return miss(2u);
  const auto fmt = static_cast<GXVtxFmt>(vat);
  const std::uint32_t stride = VertexSize(fmt);
  if (stride == 0u || len != 3u + std::size_t(first) * stride) return ProcessOne(data, len, depth);
  std::size_t end = len;
  std::uint32_t members = 1u, triangles = DrawRunTriangleVertices(op, first), list = first;
  bool mixed = false;
  while (end + 3u <= available && IsDrawCommand(data[end]) &&
         (data[end] & GX_VAT_MASK) == vat) {
    const std::uint8_t member_op = data[end] & GX_OPCODE_MASK;
    const std::uint32_t count = Be16(data + end + 1);
    const std::size_t bytes = 3u + std::size_t(count) * stride;
    if (!DrawRunMember(member_op, count) || end + bytes > available) break;
    const std::uint32_t expanded = DrawRunTriangleVertices(member_op, count);
    if (triangles + expanded > kDrawRunMaxVertices) break;
    mixed |= member_op != op;
    triangles += expanded;
    list += count;
    end += bytes;
    ++members;
  }
  if (members == 1u) return miss(1u);

  const bool keep_list = !mixed && (op == GX_QUADS || op == GX_TRIANGLES);
  const std::uint32_t total = keep_list ? list : triangles;
  auto& out = g_draw_run_buffer;
  out.resize(3u + std::size_t(total) * stride);
  out[0] = keep_list ? cmd : static_cast<std::uint8_t>(GX_TRIANGLES | vat);
  out[1] = static_cast<std::uint8_t>(total >> 8u);
  out[2] = static_cast<std::uint8_t>(total);
  std::uint8_t* dst = out.data() + 3u;
  for (std::size_t at = 0u; at < end;) {
    const std::uint8_t member_op = data[at] & GX_OPCODE_MASK;
    const std::uint32_t count = Be16(data + at + 1);
    const std::uint8_t* v = data + at + 3u;
    const auto put = [&](std::uint32_t index) {
      CopyRunVertex(dst, v + std::size_t(index) * stride, stride);
      dst += stride;
    };
    if (keep_list || member_op == GX_TRIANGLES) {
      std::memcpy(dst, v, std::size_t(count) * stride);
      dst += std::size_t(count) * stride;
    } else if (member_op == GX_QUADS) {
      // Identical to aurora::gx::prepare_idx_buffer's quad expansion.
      for (std::uint32_t q = 0u; q < count; q += 4u) {
        put(q); put(q + 1u); put(q + 2u);
        put(q + 2u); put(q + 3u); put(q);
      }
    } else {
      for (std::uint32_t i = 2u; i < count; ++i) {
        // Identical to aurora::gx::prepare_idx_buffer's strip/fan expansion.
        if (member_op == GX_TRIANGLEFAN) { put(0u); put(i - 1u); put(i); }
        else if (i == 2u || (i & 1u) == 0u) { put(i - 2u); put(i - 1u); put(i); }
        else { put(i - 1u); put(i - 2u); put(i); }
      }
    }
    at += 3u + std::size_t(count) * stride;
  }
  *consumed = end;
  ++g_draw_run_runs;
  g_draw_run_members += members;
  return ProcessOne(out.data(), out.size(), depth);
}

void RememberFifoWrite(std::uint64_t value, std::uint8_t size, std::uint64_t stream_offset,
                       std::uint32_t guest_pc) {
  if(!g_fifo_forensic_trace) return;
  g_fifo_write_trace[g_fifo_write_trace_head] = {value, stream_offset, guest_pc, size};
  static_assert((g_fifo_write_trace.size() & (g_fifo_write_trace.size() - 1u)) == 0u);
  g_fifo_write_trace_head =
      (g_fifo_write_trace_head + 1u) & (g_fifo_write_trace.size() - 1u);
  if (g_fifo_write_trace_count < g_fifo_write_trace.size()) ++g_fifo_write_trace_count;
}

void RememberDecodedCommand(const std::uint8_t* p, std::size_t len, std::uint64_t stream_offset) {
  if (!p || len == 0u) return;
  g_fifo_last_cmd = p[0];
  g_fifo_last_cmd_len = len;
  const std::uint8_t op = p[0] & GX_OPCODE_MASK;
  if (op >= 0x80u && op <= 0xB8u && len >= 3u) {
    g_fifo_last_draw_fmt = p[0] & GX_VAT_MASK;
    g_fifo_last_draw_count = Be16(p + 1);
    g_fifo_last_draw_stride = g_fifo_last_draw_count == 0u
                                ? 0u
                                : static_cast<std::uint32_t>((len - 3u) / g_fifo_last_draw_count);
  }
  if(!g_fifo_forensic_trace) return;
  FifoCommandTrace trace{};
  trace.stream_offset = stream_offset;
  trace.len = len;
  trace.cmd = p[0];
  trace.fmt = g_fifo_last_draw_fmt;
  trace.count = g_fifo_last_draw_count;
  trace.stride = g_fifo_last_draw_stride;
  g_fifo_command_trace[g_fifo_command_trace_head] = trace;
  static_assert((g_fifo_command_trace.size() & (g_fifo_command_trace.size() - 1u)) == 0u);
  g_fifo_command_trace_head =
      (g_fifo_command_trace_head + 1u) & (g_fifo_command_trace.size() - 1u);
  if (g_fifo_command_trace_count < g_fifo_command_trace.size()) ++g_fifo_command_trace_count;
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
  // v288: GekkoAOT has already framed and validated this complete command.
  // Enter Aurora's native state handlers directly instead of asking its FIFO
  // interpreter to decode the same opcode/header a second time. Keep BP copy,
  // mask, token, texture provenance and CP array handling below authoritative.
  static const bool direct_state=EnvBool("GEKKOAOT_NATIVE_GX_DIRECT_STATE",true);
  static const bool trace_xf=EnvBool("GEKKOAOT_TRACE_GX_STATE",false);
  if(trace_xf && op==GX_LOAD_XF_REG) {
    const auto header=Be32(p+1);
    const unsigned address=header&0xffffu, count=(header>>16)+1u;
    static unsigned xf_logs=0;
    if(address>=0x101au && address<=0x1026u && xf_logs++<128u)
      std::fprintf(stderr,"GEKKOAOT_TRACE_GX_STATE event=xf-projection-viewport address=%04x words=%u first=%08x\n",
                   address,count,Be32(p+5));
  }
  if(g_array_base_trace && op==GX_CMD_INVL_VC) ++g_array_base_trace_invalidate;
  if(g_draw_census) {
    if(op==GX_CMD_CALL_DL) NoteDrawGap(256u);
    else if(op==GX_LOAD_CP_REG) NoteDrawGap(p[1]>=0xA0u ? 8u : 16u);
    else if(op==GX_LOAD_XF_REG) NoteDrawGap((Be32(p+1)&0xffffu)<0x1000u ? 32u : 64u);
    else if(op==GX_LOAD_INDX_A||op==GX_LOAD_INDX_B||op==GX_LOAD_INDX_C||op==GX_LOAD_INDX_D) NoteDrawGap(128u);
    else if(op==kLoadBpOpcode) {
      const std::uint8_t reg=p[1];
      if(reg==0x52u) NoteDrawGap(512u);
      else if((reg>=0x80u&&reg<=0xBBu)||reg==0x64u||reg==0x65u) NoteDrawGap(1u);
      else if((reg>=0xC0u&&reg<=0xFDu)||(reg>=0x28u&&reg<=0x2Fu)||(reg>=0x40u&&reg<=0x42u)) NoteDrawGap(2u);
      else if(reg!=0x45u&&reg!=0x47u&&reg!=0x48u) NoteDrawGap(4u);
    }
  }
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
    // XF aliases can invalidate decoded BP state without changing its word.
    const bool bp_changed = reg==0xFEu || !aurora::gx::g_gxState.bpRegValid.test(reg) || bp_cache[reg]!=effective_word;
    // v159: SDKs and retail render loops aggressively re-emit GX state. Aurora
    // quite reasonably treats a BP write as a state mutation, but feeding an
    // identical value through handle_bp() dirties pipelines/textures anyway.
    // Suppress only registers whose write has no command-like side effect.
    // Token/draw-done/copy/TLUT/cache/preload and BP-mask writes always pass.
    const bool bp_side_effect = reg==0x45u || reg==0x47u || reg==0x48u ||
                                reg==0x52u || (reg>=0x55u && reg<=0x57u) ||
                                reg==0x65u || reg==0x66u || reg==0xFEu ||
                                (reg>=0x60u && reg<=0x67u);
    if(!bp_changed && !bp_side_effect) {
      // Every non-mask BP command consumes the current write mask. The value is
      // already present in Aurora's cache, so no other state needs touching.
      bp_cache[0xFEu]=0x00ffffffu;
      return true;
    }

    // IMAGE0 and MODE1 can change dimensions/mip span without a new IMAGE3
    // address write. Track those register groups only on an actual mutation,
    // then skip the eight-slot scans on ordinary draws.
    if(bp_changed && ((reg>=0x84u&&reg<=0x8bu)||(reg>=0xa4u&&reg<=0xabu)))
      g_texture_mapping_scan_dirty=true;
    if(bp_changed && ((reg>=0x88u&&reg<=0x8bu)||(reg>=0xa8u&&reg<=0xabu)||
                      (reg>=0x98u&&reg<=0x9bu)||(reg>=0xb8u&&reg<=0xbbu)))
      g_tlut_binding_scan_dirty=true;
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
    if (reg == 0x55u || reg == 0x56u) {
      consume_intercepted_bp();
      return GekkoAOT::GX::WriteNativeBoundingBox(reg, effective_word & 0x00ffffffu);
    }
    if(reg==0x65u) {
      consume_intercepted_bp();
      MapTlut();
      return !g_fifo_fault;
    }
    if(reg>=0x49u && reg<=0x4Eu) TrackRawCopyRegister(reg,effective_word);
    if(reg==0x52u && ((effective_word>>14)&1u)!=0u) {
      g_display_insets.EndFrame(g_display_copy_count);
      {
        // v363: a pre-rendered backdrop switches the next frames to 4:3 at
        // once; three frames without one switch back.
        const bool was_43=g_frame_force_43.load(std::memory_order_relaxed);
        g_recorded_force_43=was_43;
        bool force_43=was_43;
        if (g_frame_prerendered) { force_43=true; g_prerendered_idle=0; }
        else if (++g_prerendered_idle>=3u) force_43=false;
        g_frame_prerendered=false;
        if (force_43!=was_43) {
          g_frame_force_43.store(force_43,std::memory_order_relaxed);
          aurora::gx::g_gxState.dirty|=aurora::gx::DirtyUniform;
          static unsigned prerendered_logs=0;
          if (prerendered_logs++<16u)
            std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_PRERENDERED_43_V363 copy=%llu mode=%s\n",
                         static_cast<unsigned long long>(g_display_copy_count),force_43 ? "4:3" : "widescreen");
        }
        const bool was_flat=g_flat_frames.Flat();
        g_flat_frames.EndFrame(g_frame_depth_draws);
        g_frame_depth_draws=0;
        static unsigned flat_logs=0;
        if (g_wide_perspective_ui && g_flat_frames.Flat()!=was_flat && flat_logs++<16u)
          std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_PERSPECTIVE_UI_V362 copy=%llu screen=%s\n",
                       static_cast<unsigned long long>(g_display_copy_count),
                       g_flat_frames.Flat() ? "2d-cards-ui-scissor" : "3d");
      }
      if(g_interp_probe_enabled) {
        auto& probe=g_interp_probe;
        std::unordered_multimap<std::uint64_t,std::uint64_t> previous;
        previous.reserve(probe.previous.size());
        for(const auto& [sig,mtx] : probe.previous) previous.emplace(sig,mtx);
        for(const auto& [sig,mtx] : probe.current) {
          ++probe.draws;
          const auto it=previous.find(sig);
          if(it==previous.end()) continue;
          ++probe.matched;
          if(it->second!=mtx) ++probe.moved;
          previous.erase(it);
        }
        probe.previous.swap(probe.current);
        probe.current.clear();
        if(++probe.frames%120u==0u) {
          std::fprintf(stderr,"GEKKOAOT_INTERP_PROBE frames=%llu draws_per_frame=%.0f matched=%.1f%% moved_of_matched=%.1f%%\n",
                       static_cast<unsigned long long>(probe.frames),double(probe.draws)/120.0,
                       probe.draws ? 100.0*double(probe.matched)/double(probe.draws) : 0.0,
                       probe.matched ? 100.0*double(probe.moved)/double(probe.matched) : 0.0);
          probe.draws=probe.matched=probe.moved=0;
        }
      }
      g_copy_provenance.BeginFrame();
      ++g_display_copy_count;
      g_camera_copy_epoch.store(g_display_copy_count,std::memory_order_release);
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
    if(texture_copy) ForgetTextureSourceVersions();  // v324: EFB copies rewrite guest RAM
    if(texture_copy && !PrepareRawTextureCopy(effective_word)) {
      // Bad raw copy state is preferable to poisoning Aurora's copy-texture
      // cache with a null/stale destination. Continue guest execution and let
      // the next complete copy state recover naturally.
      consume_intercepted_bp();
      return true;
    }
    // v300: Aurora records a GXCopyTex as a sealed pass whose FrameOp captures
    // its pass/upload pointers and staging high-water marks; passes, ops and
    // uploads live in std::deque, and encode_op touches only worker-owned
    // state. Later draws bind the copy through copyTextures, ordered on the
    // same frame encoder. Waiting for the render worker on both sides of every
    // copy (v78) only serialized the GX and render threads; keep it as an
    // opt-in diagnostic.
    static const bool copy_fence = EnvBool("GEKKOAOT_GX_COPY_FENCE", false);
    if(texture_copy) {
      if(copy_fence && !aurora::gfx::render_worker::is_worker_thread())
        aurora::gfx::render_worker::synchronize();
      static unsigned trace_before=0;
      if(trace_before++<64u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_EFB_COPY_FENCE_V78=1 phase=before dest_phys=%08x fmt=%u\n",
                     g_copy_dest_physical,unsigned(aurora::gx::g_gxState.texCopyFmt));
    }
    if(direct_state && len==5u) aurora::gx::fifo::handle_bp(word);
    else (void)aurora::gx::fifo::process(p,static_cast<std::uint32_t>(len));
    if(reg==0x66u) InvalidateTextureCacheV87();
    if(texture_copy) {
      if(copy_fence && !aurora::gfx::render_worker::is_worker_thread())
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
    NoteDraw(Be16(p+1));
    const auto fmt=static_cast<GXVtxFmt>(cmd&GX_VAT_MASK);
    const auto prim=static_cast<GXPrimitive>(cmd&GX_OPCODE_MASK);
    const std::uint16_t count=Be16(p+1);
    if(g_texture_mapping_scan_dirty) {
      RefreshTextureMappings();
      g_texture_mapping_scan_dirty=false;
    }
    if(g_tlut_binding_scan_dirty) {
      BindRawTluts();
      g_tlut_binding_scan_dirty=false;
    }
    using aurora::gx::g_gxState;
    if(g_texture_source_invalidation_pending && count!=0u) {
      for(unsigned map=0;map<aurora::gx::MaxTextures;++map) {
        auto& texture=g_gxState.loadedTextures[map];
        if(!texture.data) continue;
        texture.texDataVersion=static_cast<std::uint32_t>(++g_texture_generation);
        const auto span=aurora::gx::texture::texture_source_size(
            texture.format(),texture.width(),texture.height(),texture.mip_count());
        if((g_texture_image3_valid&(1u<<map))!=0u && span<=0xffffffffu)
          RememberTextureSourceVersion((g_texture_image3_word[map]&0x00ffffffu)<<5u,texture.image0,
                                       static_cast<std::uint8_t>(texture.mip_count()),
                                       static_cast<std::uint32_t>(span),texture.texDataVersion);
        if(g_draw_census) { ++g_tex_census_forced; g_tex_census_forced_bytes+=span; }
      }
      g_texture_source_invalidation_pending=false;
    }
    if(g_trace_font) {
      static std::array<std::uint64_t,128> font_draw_keys{};
      static unsigned font_draw_samples=0;
      const auto& tex=g_gxState.loadedTextures[0];
      const std::uint64_t key=(std::uint64_t(g_gxState.bpRegCache[0x94])<<32u)|
          (std::uint64_t(tex.image0)<<8u)|g_gxState.numTexGens;
      bool seen=false;
      for(unsigned i=0;i<font_draw_samples;++i) seen|=font_draw_keys[i]==key;
      if(!seen && font_draw_samples<font_draw_keys.size()) {
        font_draw_keys[font_draw_samples++]=key;
        std::fprintf(stderr,
          "GEKKOAOT_T3_FONT_DIAG prim=%u vertices=%u format=%u size=%ux%u image0=%06x image3=%06x tlut0=%06x tlut_src=%06x tlut_load=%06x tev_color=%06x tev_alpha=%06x alpha_compare=%06x blend=%06x texgens=%u\n",
          unsigned(prim),unsigned(count),unsigned(tex.format()),tex.width(),tex.height(),
          g_gxState.bpRegCache[0x88]&0xffffffu,
          g_gxState.bpRegCache[0x94]&0xffffffu,g_gxState.bpRegCache[0x98]&0xffffffu,
          g_gxState.bpRegCache[0x64]&0xffffffu,g_gxState.bpRegCache[0x65]&0xffffffu,
          g_gxState.bpRegCache[0xc0]&0xffffffu,g_gxState.bpRegCache[0xc1]&0xffffffu,
          g_gxState.bpRegCache[0xf3]&0xffffffu,g_gxState.bpRegCache[0x41]&0xffffffu,
          unsigned(g_gxState.numTexGens));
      }
    }
    if(trace_xf) {
      using namespace aurora::gx;
      static unsigned draw_logs=0;
      static std::uint64_t previous=~std::uint64_t{0};
      std::uint64_t signature=1469598103934665603ull;
      for(const auto reg : {0x00,0x28,0x41,0xc0,0xc1,0xf3})
        signature=(signature^g_gxState.bpRegCache[reg])*1099511628211ull;
      signature=(signature^g_gxState.loadedTextures[0].image0)*1099511628211ull;
      if(signature!=previous && draw_logs++<128u) {
        const auto& tcg=g_gxState.tcgs[0];
        std::fprintf(stderr,"GEKKOAOT_TRACE_GX_STATE event=draw vertices=%u pn=%u texgens=%u tev=%u tex0=%ux%u:%u tref=%06x color=%06x alpha=%06x compare=%06x blend=%06x texgen=%u:%u mtx=%u post=%u normalize=%u dual=%u viewport=%.2f,%.2f,%.2f,%.2f\n",
                     count,g_gxState.currentPnMtx,g_gxState.numTexGens,g_gxState.numTevStages,
                     g_gxState.loadedTextures[0].width(),g_gxState.loadedTextures[0].height(),unsigned(g_gxState.loadedTextures[0].format()),
                     g_gxState.bpRegCache[0x28]&0xffffffu,g_gxState.bpRegCache[0xc0]&0xffffffu,g_gxState.bpRegCache[0xc1]&0xffffffu,
                     g_gxState.bpRegCache[0xf3]&0xffffffu,g_gxState.bpRegCache[0x41]&0xffffffu,
                     unsigned(tcg.type),unsigned(tcg.src),unsigned(tcg.mtx),unsigned(tcg.postMtx),unsigned(tcg.normalize),
                     g_gxState.xfRegCache[0x12],g_gxState.logicalViewport.left,g_gxState.logicalViewport.top,
                     g_gxState.logicalViewport.width,g_gxState.logicalViewport.height);
        previous=signature;
      }
    }
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

    // GX_CULL_ALL is a legal state (used by Luigi's Mansion). WebGPU has no
    // equivalent and Aurora rejects it when constructing a triangle pipeline.
    // Consume the framed vertices without submitting culled polygons.
    if(g_gxState.cullMode == GX_CULL_ALL && prim != GX_POINTS &&
       prim != GX_LINES && prim != GX_LINESTRIP) return true;

    GatheredArrayDraw gathered_draw;
    const bool gathered=gathered_draw.Prepare(p,len,fmt,count);
    CompactArrayPlan compact_plan{};
    const GekkoAOT::GX::NativeAttributePlan* bindings=nullptr;
    if(!gathered && !PrepareIndexedArrayWindows(p,len,g_compact_array_windows ? &compact_plan : nullptr,&bindings)) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_FIFO_ERROR opcode=0x%02x action=reject-array-window\n",
                   unsigned(cmd));
      return false;
    }
    CompactArrayDraw compact_draw;
    if(!gathered) {
      if(g_compact_array_windows || g_native_array_snapshots) compact_draw.Prepare(compact_plan,*bindings);
      else PrimePersistentIndexedArraysV161H(0,*bindings);
    }
    const bool screen_fill=ClassifyScreenFill(p,len,prim,fmt,count);
    if(g_gxState.projType==GX_PERSPECTIVE) {
      if(g_gxState.depthCompare && g_gxState.depthUpdate) ++g_frame_depth_draws;
      g_draw_perspective_ui=g_wide_perspective_ui && !g_gxState.depthUpdate && g_flat_frames.Flat();
    } else {
      g_draw_perspective_ui=false;
    }
    if(g_interp_probe_enabled) {
      std::uint64_t sig=1469598103934665603ull, mtx=1469598103934665603ull;
      const auto mix=[](std::uint64_t& h,std::uint64_t v){ h=(h^v)*1099511628211ull; };
      mix(sig,unsigned(prim)); mix(sig,count); mix(sig,unsigned(fmt)); mix(sig,unsigned(g_gxState.projType));
      mix(sig,g_gxState.numTevStages); mix(sig,g_gxState.numTexGens);
      for(unsigned stage=0;stage<g_gxState.numTevStages && stage<aurora::gx::MaxTevStages;++stage) {
        const auto map=g_gxState.tevStages[stage].texMapId;
        mix(sig,map==GX_TEXMAP_NULL ? 0u : reinterpret_cast<std::uintptr_t>(g_gxState.loadedTextures[unsigned(map)&7u].data));
      }
      mix(sig,reinterpret_cast<std::uintptr_t>(g_gxState.arrays[GX_VA_POS].data));
      const unsigned pn=g_gxState.currentPnMtx;
      for(unsigned r=0;r<3u && pn+r<g_gxState.xfPosRows.size();++r)
        for(unsigned c=0;c<4u;++c) { std::uint32_t b; std::memcpy(&b,&g_gxState.xfPosRows[pn+r][c],4); mix(mtx,b); }
      for(unsigned c=0;c<4u;++c) { std::uint32_t b; std::memcpy(&b,&g_gxState.proj.m0[c],4); mix(mtx,b); }
      g_interp_probe.current.emplace_back(sig,mtx);
    }
    if(g_replay_capture_on.load(std::memory_order_relaxed)) {
      // Stable across frames: what is drawn and with which textures. Vertex
      // array addresses are left out (CPU-skinned meshes double-buffer them).
      std::uint64_t sig=1469598103934665603ull;
      const auto mix=[](std::uint64_t& h,std::uint64_t v){ h=(h^v)*1099511628211ull; };
      mix(sig,unsigned(prim)); mix(sig,count); mix(sig,unsigned(fmt)); mix(sig,unsigned(g_gxState.projType));
      mix(sig,g_gxState.numTevStages); mix(sig,g_gxState.numTexGens); mix(sig,g_gxState.currentPnMtx);
      for(unsigned stage=0;stage<g_gxState.numTevStages && stage<aurora::gx::MaxTevStages;++stage) {
        const auto map=g_gxState.tevStages[stage].texMapId;
        mix(sig,map==GX_TEXMAP_NULL ? 0u : reinterpret_cast<std::uintptr_t>(g_gxState.loadedTextures[unsigned(map)&7u].data));
      }
      g_replay_draw_sig=sig;
      g_replay_draw_pnmtx=g_gxState.currentPnMtx;
      if(GekkoAOT::GX::NativeBoundingBoxActive() && GekkoAOT::GX::NativeBoundingBoxRead()) g_replay_frame_bbox=true;
    }
    if(g_ui_insets && RecordAspectMode()==1u && !aurora::gfx::is_offscreen()) {
      const float canvas=DisplayCanvasWidth();
      const auto& view=g_gxState.logicalViewport;
      const auto& clip=g_gxState.logicalScissor;
      if(GekkoAOT::GX::IsInsetSpan(view.left,view.width,canvas))
        g_display_insets.NoteDraw(view.left,view.width,g_display_copy_count);
      if(GekkoAOT::GX::IsInsetSpan(float(clip.x),float(clip.width),canvas))
        g_display_insets.NoteDraw(float(clip.x),float(clip.width),g_display_copy_count);
    }
    if(g_wide_span_trace && g_gxState.projType==GX_ORTHOGRAPHIC) {
      // Diagnostic: textures sampled by orthographic draws and their EFB-copy provenance.
      static std::vector<std::uint64_t> seen_ortho;
      for(unsigned stage=0;stage<g_gxState.numTevStages && stage<aurora::gx::MaxTevStages;++stage) {
        const auto map=g_gxState.tevStages[stage].texMapId;
        if(map==GX_TEXMAP_NULL) continue;
        const auto& slot=g_gxState.loadedTextures[unsigned(map)&7u];
        const auto copy=g_gxState.copyTextures.find(slot.data);
        const bool is_copy=slot.data && copy!=g_gxState.copyTextures.end();
        const int sw=is_copy?copy->second.guestSource.width:0, sh=is_copy?copy->second.guestSource.height:0;
        const std::uint64_t key=(std::uint64_t(slot.width())<<40)^(std::uint64_t(slot.height())<<28)^
            (std::uint64_t(is_copy)<<27)^(std::uint64_t(sw)<<14)^std::uint64_t(sh);
        if(seen_ortho.size()<64u && std::find(seen_ortho.begin(),seen_ortho.end(),key)==seen_ortho.end()) {
          seen_ortho.push_back(key);
          std::fprintf(stderr,"GEKKOAOT_WIDESCREEN_ORTHO_TEX_TRACE tex=%ux%u fmt=%u copy=%u source=%dx%d view=%.0fx%.0f count=%u\n",
                       slot.width(),slot.height(),slot.raw_format(),is_copy?1u:0u,sw,sh,
                       g_gxState.logicalViewport.width,g_gxState.logicalViewport.height,unsigned(count));
        }
      }
    }
    if (screen_fill!=g_screen_fill_draw) {
      g_screen_fill_draw=screen_fill;
      g_gxState.dirty|=aurora::gx::DirtyUniform;
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
    ObservePerspectiveCamera();
    if(aurora::gx::fifo::submit_raw_draw(prim,fmt,count,gathered ? gathered_draw.vertices : p+3,
                                         static_cast<std::uint32_t>(len-3u)))
      return true;

    // Accepted layouts have the exact same byte count as the raw draw. Keep
    // the original semantic path if the renderer rejects a gathered payload.
    if(gathered) {
      for(unsigned f=0;f<GetIndexScanLayout(fmt)->field_count;++f)
        g_gxState.arrays[GetIndexScanLayout(fmt)->fields[f].attr].cachedRange={};
      const GekkoAOT::GX::NativeAttributePlan* original_bindings=nullptr;
      if(!PrepareIndexedArrayWindows(p,len,&compact_plan,&original_bindings)) return false;
      compact_draw.Prepare(compact_plan,*original_bindings);
    }

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

  if(direct_state && op==GX_LOAD_CP_REG && len==6u) {
    aurora::gx::fifo::handle_cp(p[1],Be32(p+2));
  } else if(direct_state && op==GX_LOAD_XF_REG && len>=9u &&
            len==5u+std::size_t((Be32(p+1)>>16u)+1u)*4u) {
    aurora::gx::fifo::handle_xf(static_cast<std::uint16_t>(Be32(p+1)),
                              std::span<const std::uint8_t>(p+5,len-5u));
  } else {
    (void)aurora::gx::fifo::process(p,static_cast<std::uint32_t>(len));
  }
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
    std::size_t consumed=len;
    if(!ProcessCommandOrRun(data+off,bytes-off,len,depth,&consumed)) return false;
    off+=consumed;
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

bool CanRecoverSingleByteFifoDesync(const std::uint8_t* p, std::size_t available,
                                    std::size_t* validated_bytes) {
  if(!p || available<2u || !validated_bytes) return false;

  // 0x18..0x1f are the reserved hole between LOAD_XF (0x10 family) and the
  // indexed-load commands (0x20 family). Do not turn arbitrary unknown GX
  // opcodes into NOPs. Rising Sun can occasionally expose one stray byte from
  // this reserved family while the following byte is already a complete BP/CP
  // control command. Recover only that one-byte loss of framing and only when
  // the complete currently-buffered suffix validates from the next byte.
  if((p[0]&GX_OPCODE_MASK)!=0x18u) return false;
  const std::uint8_t next_op=p[1]&GX_OPCODE_MASK;
  const bool strong_anchor = next_op==kLoadBpOpcode || next_op==GX_LOAD_CP_REG ||
      next_op==GX_LOAD_XF_REG || next_op==GX_LOAD_INDX_A ||
      next_op==GX_LOAD_INDX_B || next_op==GX_LOAD_INDX_C ||
      next_op==GX_LOAD_INDX_D || next_op==GX_CMD_CALL_DL;
  if(!strong_anchor) return false;

  std::size_t off=1u;
  std::size_t commands=0u;
  while(off<available) {
    std::size_t len=0u,need=1u;
    const auto status=CommandLength(p+off,available-off,&len,&need);
    if(status==DecodeResult::Invalid) return false;
    if(status==DecodeResult::Incomplete) {
      // A command split by the host burst boundary is still a valid suffix;
      // DrainFifo() will wait for its remaining bytes normally.
      *validated_bytes=off;
      return commands!=0u;
    }
    off+=len;
    ++commands;
  }
  *validated_bytes=off;
  return commands!=0u;
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
      std::size_t validated=0u;
      if(CanRecoverSingleByteFifoDesync(g_fifo.data()+g_fifo_read,available,&validated)) {
        const std::uint8_t bad=g_fifo[g_fifo_read];
        const std::uint8_t anchor=g_fifo[g_fifo_read+1u];
        static std::uint64_t recoveries=0u;
        ++recoveries;
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_GX_FIFO_RESYNC_V201=1 offset=%zu absolute=%llu "
                     "dropped=0x%02x anchor=0x%02x validated=%zu count=%llu action=skip-one-reserved-byte\n",
                     g_fifo_read,
                     static_cast<unsigned long long>(g_fifo_stream_base+g_fifo_read),
                     unsigned(bad),unsigned(anchor),validated,
                     static_cast<unsigned long long>(recoveries));
        ++g_fifo_read;
        g_fifo_need=1u;
        continue;
      }
      DumpFifoFault(g_fifo_read);
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_FIFO_ERROR offset=%zu opcode=0x%02x action=fail-closed\n",
                   g_fifo_read,unsigned(g_fifo[g_fifo_read]));
      g_fifo_fault=true;
      g_quit=true;
      break;
    }
    RememberDecodedCommand(g_fifo.data()+g_fifo_read,len,g_fifo_stream_base+g_fifo_read);
    std::size_t consumed=len;
    if(!ProcessCommandOrRun(g_fifo.data()+g_fifo_read,g_fifo.size()-g_fifo_read,len,0u,&consumed)) {
      DumpFifoFault(g_fifo_read);
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_FIFO_ERROR offset=%zu opcode=0x%02x action=fail-closed-process\n",
                   g_fifo_read,unsigned(g_fifo[g_fifo_read]));
      g_fifo_fault=true;
      g_quit=true;
      break;
    }
    g_fifo_read+=consumed;
    g_fifo_need=1u;
  }
  CompactFifo();
}

bool RuntimeWindowIsEmbedded() {
  const char* parent = std::getenv("GEKKOAOT_EMBED_WINDOW");
  return parent && *parent;
}

#ifdef _WIN32
// The Qt host owns fullscreen and lifetime. The SDL renderer keeps its HWND
// and Vulkan surface, but becomes a child of the persistent game viewport.
HWND g_embed_parent = nullptr;
int g_embed_width = 0, g_embed_height = 0;
void ResizeWindowsEmbed() {
  if (!g_embed_parent || !g_info.window || !IsWindow(g_embed_parent)) return;
  RECT bounds{};
  if (!GetClientRect(g_embed_parent, &bounds)) return;
  const int w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
  if (w <= 0 || h <= 0 || (w == g_embed_width && h == g_embed_height)) return;
  g_embed_width = w; g_embed_height = h;
  (void)SDL_SetWindowSize(g_info.window, w, h);
}
bool EmbedWindows() {
  const char* text = std::getenv("GEKKOAOT_EMBED_WINDOW");
  if (!text || !*text) return true;
  char* end = nullptr;
  const auto value = std::strtoull(text, &end, 0);
  const HWND parent = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(value));
  if (end == text || *end || !IsWindow(parent) || !g_info.window) return false;
  const HWND child = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(g_info.window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  if (!child || child == parent) return false;
  const auto style = GetWindowLongPtrW(child, GWL_STYLE);
  SetWindowLongPtrW(child, GWL_STYLE, (style & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_BORDER)) | WS_CHILD | WS_CLIPSIBLINGS);
  SetLastError(0);
  if (!SetParent(child, parent) && GetLastError() != 0) return false;
  g_embed_parent = parent; g_embed_width = 0; g_embed_height = 0;
  ResizeWindowsEmbed();
  SetWindowPos(child, nullptr, 0, 0, g_embed_width, g_embed_height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  std::fprintf(stderr, "GEKKOAOT_NATIVE_GX_SINGLE_WINDOW=1 backend=win32 parent=%llu\n", value);
  return true;
}
#endif
void ApplyWindowIdentity() {
  if (!g_info.window) return;
  if (const char* path = std::getenv("GEKKOAOT_WINDOW_ICON"); path && *path) {
    if (SDL_Surface* icon = SDL_LoadBMP(path)) {
      (void)SDL_SetWindowIcon(g_info.window, icon); SDL_DestroySurface(icon);
    }
  }
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
#ifdef _WIN32
  ResizeWindowsEmbed();
#endif
  const AuroraEvent* ev=aurora_update();
  while(ev && ev->type!=AURORA_NONE) {
    if(ev->type==AURORA_EXIT) g_quit=true;
    if (ev->type == AURORA_WINDOW_RESIZED || ev->type == AURORA_UNPAUSED ||
        ev->type == AURORA_DISPLAY_SCALE_CHANGED ||
        (ev->type == AURORA_SDL_EVENT && ev->sdl.type == SDL_EVENT_WINDOW_EXPOSED))
      aurora::gfx::render_worker::enqueue_work([] { g_cached_scanout.Reset(); });
    ++ev;
  }
  PollFullscreenShortcut();
  if (g_info.window) RefreshHostWindowAspect();
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
  // A previous renderer set quit at shutdown. A new device owns a fresh
  // producer lifetime; retaining that latch silently discards every command.
  g_quit=false;
  g_frame_open=false;
  g_alt_enter_latched=false;
  g_resolve=resolve; g_resolve_user=user; g_fifo.reserve(1u<<20);
  g_fifo_read=0; g_fifo_need=1; g_fifo_fault=false; g_fifo_fault_reported=false;
  g_fifo_forensic_trace=EnvBool("GEKKOAOT_NATIVE_GX_FIFO_FORENSIC",false);
  g_native_commands.Reset();
  g_native_commands.SetDirectSpansEnabled(EnvBool("GEKKOAOT_NATIVE_GX_DIRECT_COMMAND_SPANS",true));
  g_typed_arrays=EnvBool("GEKKOAOT_NATIVE_GX_TYPED_ARRAYS",false) && !g_fifo_forensic_trace &&
      !std::getenv("GEKKOAOT_GX_DIAG_FIFO_CAPTURE");
  g_direct_matrices=EnvBool("GEKKOAOT_NATIVE_GX_DIRECT_MATRICES",true) && !g_fifo_forensic_trace &&
      !std::getenv("GEKKOAOT_GX_DIAG_FIFO_CAPTURE");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_DIRECT_MATRICES_V317=1 enabled=%u renderer=matrix-state\n",g_direct_matrices?1u:0u);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_TYPED_COMMANDS_V313=1 enabled=%u arrays=host-parameters fallback=exact-stream\n",g_typed_arrays?1u:0u);
  g_fifo_streaming=EnvBool("GEKKOAOT_NATIVE_GX_STREAM_FIFO",true);
  g_fifo_stream_stats=EnvBool("GEKKOAOT_NATIVE_GX_STREAM_FIFO_STATS",false);
  g_fifo_submitted_bytes=0; g_fifo_copied_bytes=0; g_fifo_direct_bytes=0;
  g_fifo_write_trace.fill({}); g_fifo_write_trace_head=0; g_fifo_write_trace_count=0;
  g_fifo_last_cmd=0; g_fifo_last_cmd_len=0; g_fifo_last_draw_fmt=0xffu;
  g_fifo_last_draw_count=0; g_fifo_last_draw_stride=0;
  g_vertex_size_cache.fill(0); g_vertex_size_valid=0;
  g_index_scan_layout.fill({}); g_index_scan_valid=0;
  g_texture_image3_word.fill(0); g_texture_image3_valid=0; g_texture_image3_written=0;
  g_texture_ui_source_hash.fill(0); g_texture_ui_source_hash_valid=0;
  g_tlut_memory.Reset(); g_tlut_revisions.fill(0); g_tlut_bindings.fill(0);
  g_array_available.fill(0);
  g_array_guest_base.fill(0);
  g_cached_scanout.Reset();
  for(auto& slot : g_persistent_array_slots) slot = {};
  for(auto& policy : g_array_upload_policy) policy = {};
  g_adaptive_array_upload=EnvBool("GEKKOAOT_NATIVE_GX_ADAPTIVE_ARRAY_UPLOAD",true);
  g_content_array_cache=EnvBool("GEKKOAOT_NATIVE_GX_CONTENT_ARRAY_CACHE",true);
  g_content_array_hits=0;
  g_persistent_array_cursor=static_cast<std::uint32_t>(aurora::gfx::StorageBufferSize);
  g_persistent_array_frame=UINT32_MAX;
  g_persistent_array_reset_pending=false;
  g_persistent_array_cache=EnvBool("GEKKOAOT_NATIVE_GX_PERSISTENT_ARRAY_CACHE",true);
  g_persistent_array_min_bytes=EnvUnsigned("GEKKOAOT_NATIVE_GX_PERSISTENT_ARRAY_MIN_BYTES",8u*1024u);
  g_persistent_array_uploads=0;
  g_persistent_array_upload_bytes=0;
  g_persistent_array_hits=0;
  g_persistent_array_fallbacks=0;
  g_adaptive_array_fallbacks=0;
  g_compact_array_windows=EnvBool("GEKKOAOT_NATIVE_GX_COMPACT_ARRAY_WINDOWS",true);
  g_wide_solid_spans=EnvBool("GEKKOAOT_WIDESCREEN_SOLID_SPANS",true);
  g_ui_insets=EnvBool("GEKKOAOT_WIDESCREEN_UI_INSETS",true);
  g_copy_provenance_enabled=EnvBool("GEKKOAOT_WIDESCREEN_COPY_PROVENANCE",true);
  g_wide_perspective_fills=EnvBool("GEKKOAOT_WIDESCREEN_PERSPECTIVE_FILLS",true);
  g_wide_perspective_ui=EnvBool("GEKKOAOT_WIDESCREEN_PERSPECTIVE_UI",true);
  g_wide_prerendered_43=EnvBool("GEKKOAOT_WIDESCREEN_PRERENDERED_43",true);
  g_interp_probe_enabled=EnvBool("GEKKOAOT_INTERP_PROBE",false);
  g_copy_provenance.Reset();
  g_display_insets.Reset();
  g_flat_frames.Reset(); g_frame_depth_draws=0; g_draw_perspective_ui=false;
  g_frame_prerendered=false; g_prerendered_idle=0; g_frame_force_43.store(false,std::memory_order_relaxed);
  g_wide_span_trace=EnvBool("GEKKOAOT_WIDESCREEN_SPAN_TRACE",false);
  g_native_index_range_cache=EnvBool("GEKKOAOT_NATIVE_GX_INDEX_RANGE_CACHE",false);
  g_native_index_ranges.Clear();
  std::fprintf(stderr,"GEKKOAOT_NATIVE_INDEX_RANGES_V307=%u payload_limit=512 exact=layout+draw fallback=rescan\n",
               g_native_index_range_cache?1u:0u);
  g_native_array_tiles=EnvBool("GEKKOAOT_NATIVE_GX_ARRAY_TILES",true) &&
                       aurora::gfx::native_snapshot_atlas_enabled();
  g_native_array_snapshots=EnvBool("GEKKOAOT_NATIVE_GX_ARRAY_SNAPSHOTS",true) &&
                           aurora::gfx::native_snapshot_atlas_enabled();
  g_native_attribute_plan=EnvBool("GEKKOAOT_NATIVE_GX_ATTRIBUTE_PLAN",true);
  g_native_vertex_program=EnvBool("GEKKOAOT_NATIVE_GX_VERTEX_PROGRAM",true);
  g_native_vertex_gather=EnvBool("GEKKOAOT_NATIVE_GX_VERTEX_GATHER",false);
  g_native_gather_draws=g_native_gather_bytes=g_native_gather_windows=0;
  std::fprintf(stderr,"GEKKOAOT_NATIVE_VERTEX_GATHER_V318=%u indexed=16 fields=1..4 vertices=1..64 resources=host-packed fallback=original\n",g_native_vertex_gather?1u:0u);
  g_native_vertex_profile=EnvBool("GEKKOAOT_RUNTIME_PROFILE",false);
  g_native_vertex_draws=0;g_native_vertex_vertices=0;
  std::fprintf(stderr,"GEKKOAOT_NATIVE_VERTEX_V315=%u layout=compiled-index16 fields=2..4 equality=exact fallback=original env=GEKKOAOT_NATIVE_GX_VERTEX_PROGRAM\n",g_native_vertex_program?1u:0u);
  const auto* snapshot_recycling=std::getenv("GEKKOAOT_NATIVE_GX_SNAPSHOT_RECYCLING");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_SNAPSHOT_RECYCLING_V312=%u scope=cpu-shadow gpu-ranges=immutable env=GEKKOAOT_NATIVE_GX_SNAPSHOT_RECYCLING\n",
               snapshot_recycling && snapshot_recycling[0]=='1' ? 1u : 0u);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_ATTRIBUTE_PLAN_V310=%u source=validated-vcd-vat cache=layout-only resources=live env=GEKKOAOT_NATIVE_GX_ATTRIBUTE_PLAN\n",
               g_native_attribute_plan?1u:0u);
  g_native_array_snapshot_stages=0;
  g_compact_array_uploads=0;
  g_compact_array_bytes=0;
  g_compact_array_prefix_bytes=0;
  g_compact_array_rollovers=0;
  g_compact_array_fallbacks=0;
  g_array_cache_control_events=0;
  g_array_cache_invalidations=0;
  g_array_cache_control_logs=0;
  g_copy_dest_physical=0; g_copy_stride_bytes=0; g_copy_yscale_raw=0x100u; g_copy_src_valid=false; g_copy_dest_valid=false;
  g_frame_ready_xfb.store(0, std::memory_order_relaxed);
  g_texture_viewport_roles.Reset();
  g_screen_fill_draw=false;
  g_pending_efb_pokes={};
  g_efb_poke_batch=EnvBool("GEKKOAOT_NATIVE_GX_EFB_POKE_BATCH",true);
  g_efb_poke_stats=EnvBool("GEKKOAOT_NATIVE_GX_EFB_POKE_STATS",false);
  g_efb_poke_first_flush_logged=false;
  g_efb_poke_writes=0;
  g_efb_poke_draws=0;
  g_alt_enter_latched=false;
  g_interpolation_requested=EnvBool("GEKKOAOT_FRAME_INTERPOLATION",false);
  g_frame_interpolation_enabled=g_interpolation_requested;
  {
    // v358: frame interpolation re-renders geometry; the image blends stay
    // available as GEKKOAOT_INTERPOLATION_MODE=motion|blend|repeat.
    const char* mode=std::getenv("GEKKOAOT_INTERPOLATION_MODE");
    g_replay_mode=!mode || !*mode || EqualNoCase(mode,"replay");
  }
  g_video=GekkoAOT::GX::GraphicsSettingsFromEnvironment();
  GekkoAOT::GX::Hud::Configure(g_video.hud!=0,g_video.hud_items);
  g_replay_interpolate=g_video.unlock_predict==0;
  g_render_percent.store(g_video.render_percent);
  g_render_width.store(g_video.render_width); g_render_height.store(g_video.render_height);
  g_interp_previous={}; g_interp_current={};
  g_interp_previous_valid=false; g_interp_current_valid=false; g_interp_snapshot_count=0;
  g_texture_bind_cache=EnvBool("GEKKOAOT_NATIVE_GX_FIFO_TEXTURE_CACHE",true);
  g_texture_version_reuse=EnvBool("GEKKOAOT_NATIVE_GX_TEXTURE_VERSION_REUSE",true);
  ForgetTextureSourceVersions();
  g_array_base_trace=EnvBool("GEKKOAOT_NATIVE_GX_ARRAY_BASE_TRACE",false);
  g_z16r_copy_trace=EnvBool("GEKKOAOT_NATIVE_GX_Z16R_TRACE",false);
  g_ztexture_trace=EnvBool("GEKKOAOT_NATIVE_GX_ZTEXTURE_TRACE",false);
  g_partial_clear_copy_continuation=
      EnvBool("GEKKOAOT_NATIVE_GX_PARTIAL_CLEAR_CONTINUATION",false);
  g_masked_clear_efb_continuity=
      EnvBool("GEKKOAOT_NATIVE_GX_MASKED_CLEAR_CONTINUITY",true);
  g_efb_boundary_continuity=
      EnvBool("GEKKOAOT_NATIVE_GX_EFB_CONTINUITY",false);
  g_draw_census=EnvBool("GEKKOAOT_RUNTIME_PROFILE",false);
  g_draw_runs=EnvBool("GEKKOAOT_NATIVE_GX_DRAW_RUNS",true);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_DRAW_RUNS_V323=%u list_member_vertices=%u expand_member_vertices=%u "
               "max_run_vertices=%u key=vat mixed=triangles order=aurora-exact\n",
               g_draw_runs?1u:0u,unsigned(kDrawRunListMax),unsigned(kDrawRunExpandMax),
               unsigned(kDrawRunMaxVertices));
  g_display_copy_state_logs=0;
  g_partial_clear_copy_logs=0;
  g_trace_gx_copy=EnvBool("GEKKOAOT_TRACE_GX_COPY",false);
  g_trace_video=EnvBool("GEKKOAOT_TRACE_VIDEO",false);
  g_invalidate_texture_sources=EnvBool("GEKKOAOT_GX_INVALIDATE_TEXTURE_SOURCES",true);
  g_texture_source_invalidation_pending=false;
  g_trace_font=EnvBool("GEKKOAOT_TRACE_FONT",false);
  g_paranoid_texture_hash=EnvBool("GEKKOAOT_GX_PARANOID_TEXTURE_HASH",false);
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
  g_post_processing_enabled=EnvBool("GEKKOAOT_POST_PROCESSING",true);
  g_post_preset=std::getenv("GEKKOAOT_POSTFX_PRESET") ? std::getenv("GEKKOAOT_POSTFX_PRESET") : "balanced";
  if(EqualNoCase(g_post_preset.c_str(),"cinematic")) g_post_cinematic=0.20f;
  else g_post_cinematic=0.0f;
  g_post_sharpen=EnvFloat("GEKKOAOT_POSTFX_SHARPEN",0.28f,0.0f,1.0f);
  g_post_bloom=EnvFloat("GEKKOAOT_POSTFX_BLOOM",0.10f,0.0f,1.0f);
  g_post_vignette=EnvFloat("GEKKOAOT_POSTFX_VIGNETTE",0.06f,0.0f,1.0f);
  g_post_grain=EnvFloat("GEKKOAOT_POSTFX_GRAIN",0.0f,0.0f,0.25f);
  g_post_tone=EnvFloat("GEKKOAOT_POSTFX_TONE",0.15f,0.0f,1.0f);
  g_post_local_contrast=EnvFloat("GEKKOAOT_POSTFX_LOCAL_CONTRAST",0.08f,0.0f,1.0f);
  g_post_frame=0;
  AuroraConfig config{};
  config.appName="GekkoAOT " GEKKOAOT_VERSION;
  config.desiredBackend=BackendFromEnv();
#if defined(__linux__)
  // Mesa may otherwise infer Wayland from WAYLAND_DISPLAY even while SDL/Qt
  // supplies an Xlib surface. An XID then reaches the Wayland EGL path as a
  // wl_surface pointer and crashes inside eglCreateWindowSurface.
  if (config.desiredBackend == BACKEND_OPENGL || config.desiredBackend == BACKEND_OPENGLES) {
    const char* driver = std::getenv("SDL_VIDEODRIVER");
    if (driver && EqualNoCase(driver, "x11")) ::setenv("EGL_PLATFORM", "x11", 1);
    else if (driver && EqualNoCase(driver, "wayland")) ::setenv("EGL_PLATFORM", "wayland", 1);
  }
#endif
  config.vsync=EnvBool("GEKKOAOT_NATIVE_GX_VSYNC",false);
  g_vsync_default=config.vsync;
  config.startFullscreen=EnvBool("GEKKOAOT_NATIVE_GX_FULLSCREEN",false);
  // Aurora reports framebuffer pixels after DPI scaling. A 1280x720 logical
  // default becomes 1920x1080 at Windows 150% scaling, which is unnecessarily
  // large for the standalone window. Keep env overrides, but start compact.
  config.windowWidth=EnvUnsigned("GEKKOAOT_NATIVE_GX_WIDTH",960);
  config.windowHeight=EnvUnsigned("GEKKOAOT_NATIVE_GX_HEIGHT",540);
  config.pauseOnFocusLost=false;
  config.allowJoystickBackgroundEvents=true;
  config.allowCpuAdapter=EnvBool("GEKKOAOT_NATIVE_GX_ALLOW_CPU",false);
  const unsigned msaa=EnvUnsigned("GEKKOAOT_NATIVE_GX_MSAA",1);
  const unsigned anisotropy=EnvUnsigned("GEKKOAOT_NATIVE_GX_ANISOTROPY",16);
  config.msaa=(msaa==4)?4:1;
  config.maxTextureAnisotropy=(anisotropy==1 || anisotropy==2 || anisotropy==4 ||
                               anisotropy==8 || anisotropy==16)?anisotropy:16;
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_QUALITY msaa=%u anisotropy=%u interpolation=%u\n",
               config.msaa, config.maxTextureAnisotropy,
               g_frame_interpolation_enabled ? 1u : 0u);
  std::fprintf(stderr,"GEKKOAOT_POST_PROCESSING_V196=%u preset=%s sharpen=%.3f bloom=%.3f vignette=%.3f grain=%.3f tone=%.3f local=%.3f cinematic=%.3f stage=final-xfb\n",
               g_post_processing_enabled?1u:0u,g_post_preset.c_str(),g_post_sharpen,g_post_bloom,
               g_post_vignette,g_post_grain,g_post_tone,g_post_local_contrast,g_post_cinematic);
  std::fprintf(stderr,"GEKKOAOT_AURORA_HOT_ENV_CACHE_V36=1 trace_copy=%u trace_video=%u trace_font=%u paranoid_hash=%u\n",
               g_trace_gx_copy?1u:0u,g_trace_video?1u:0u,g_trace_font?1u:0u,
               g_paranoid_texture_hash?1u:0u);
  config.mem1Size=0; config.mem2Size=0;
#ifdef GEKKOAOT_HAVE_X11
  if (!PrepareX11Embed()) {
    std::fprintf(stderr,"GEKKOAOT_AURORA_GX_V001=0 reason=x11-prep-failed\n");
    return false;
  }
#endif
  int argc=1; char arg0[]="gekkoaot"; char* argv[]={arg0,nullptr};
  g_info=aurora_initialize(argc,argv,&config);
  ApplyWindowIdentity();
#ifdef _WIN32
  if (!EmbedWindows()) {
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_SINGLE_WINDOW=0 reason=win32-embed-failed\n");
    aurora_shutdown(); return false;
  }
#endif
  std::fprintf(stderr,"GEKKOAOT_GRAPHICS_API requested=%u actual=%u match=%u\n",
               unsigned(config.desiredBackend),unsigned(g_info.backend),
               config.desiredBackend==BACKEND_AUTO || config.desiredBackend==g_info.backend);
  if (config.desiredBackend != BACKEND_AUTO && config.desiredBackend != g_info.backend) {
    std::fprintf(stderr,"GEKKOAOT_GRAPHICS_API_ERROR requested API unavailable; refusing silent fallback\n");
    aurora_shutdown(); return false;
  }
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
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_XFB_PARTIAL_CLEAR_V176_LEGACY=%u default=off policy=keep-recording-open env=GEKKOAOT_NATIVE_GX_PARTIAL_CLEAR_CONTINUATION\n",
               g_partial_clear_copy_continuation?1u:0u);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_XFB_MASKED_CLEAR_CONTINUITY_V1=%u default=on policy=gxcopydisp-boundary+selective-color-depth-load env=GEKKOAOT_NATIVE_GX_MASKED_CLEAR_CONTINUITY\n",
               g_masked_clear_efb_continuity?1u:0u);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_XFB_EFB_CONTINUITY_V177_SAFE=%u default=off policy=diagnostic-all-attachment-load env=GEKKOAOT_NATIVE_GX_EFB_CONTINUITY regression=hpcos-ghse69\n",
               g_efb_boundary_continuity?1u:0u);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_PERF_RECOVERY_V115=1 array-frame-refresh-default=off indexed-draw-coherency-default=off ztexture=aurora-v1 xfb-boundary=v110\n");
  std::fprintf(stderr,
               "GEKKOAOT_AURORA_ARRAY_CACHE_V35=%u region_mib=64 base_mib=%llu min_bytes=%u small=frame-staging coherency=guest-write+dcache-publication fallback=transient-storage\n",
               g_persistent_array_cache?1u:0u,
               static_cast<unsigned long long>(aurora::gfx::StorageBufferSize/(1024ull*1024ull)),
               g_persistent_array_min_bytes);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_ADAPTIVE_ARRAY_UPLOAD_V190=%u recent_frames=4 retry_frames=64 dynamic=mapped-frame-staging stable=persistent env=GEKKOAOT_NATIVE_GX_ADAPTIVE_ARRAY_UPLOAD\n",
               g_adaptive_array_upload?1u:0u);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_CONTENT_ARRAY_CACHE_V191=%u equality=exact-bytes snapshot=immutable same-frame-ranges=distinct env=GEKKOAOT_NATIVE_GX_CONTENT_ARRAY_CACHE\n",
               g_content_array_cache?1u:0u);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_COMPACT_ARRAY_WINDOWS_V293=%u indices=raw-u16 source=used-suffix shader-bias=u32 one-draw=1 preflight=arrays+fog env=GEKKOAOT_NATIVE_GX_COMPACT_ARRAY_WINDOWS\n",
               g_compact_array_windows?1u:0u);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_RESOURCES_V303 atlas=%u tiles=%u small_arrays=%u equality=cacheable-cpu-shadow raw_indices=preserved fallback=original\n",
               aurora::gfx::native_snapshot_atlas_enabled()?1u:0u,
               g_native_array_tiles?1u:0u,g_native_array_snapshots?1u:0u);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_INDEXED_ARRAY_COHERENCY_V114=1 policy=exact-used-windows+no-cross-draw-merge default=off env=GEKKOAOT_NATIVE_GX_INDEXED_DRAW_COHERENCY\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_ZTEXTURE_V115=1 backend=aurora-bp-f4-f5 trace=%u env=GEKKOAOT_NATIVE_GX_ZTEXTURE_TRACE\n",g_ztexture_trace?1u:0u);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_INDEX_SCAN_V2=1 layout=cached-fields nbt3=xyz-element\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_XF_ARRAY_WINDOW_V86=1 arrays=pos+normal+texture+light sizing=per-indexed-load bounds=guest-mapping\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_COHERENCY_V87=1 texture-invalidate=bp66 stale-array=fail-closed stale-texture=fail-closed dl-depth=1\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_TEXTURE_ID_V10=1 identity=stable-per-slot version=per-load\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_FONT_COHERENCY_V174=1 formats=i4+i8+ia4+ia8+c4+c8+c14x2 policy=content-hash-on-stable-rebind\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_ARRAY_FRAME_REFRESH_V3=%u policy=%s\n",
               g_array_frame_refresh?1u:0u,
               g_array_frame_refresh?"gx-copy+host-frame-boundary":"gx-invalidate-driven");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_INDEXED_DRAW_COHERENCY_V109=%u policy=%s\n",
               g_indexed_draw_coherency?1u:0u,
               g_indexed_draw_coherency?"refresh-used-arrays+forbid-cross-draw-merge":"legacy-cache-reuse");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_DYNAMIC_VERTEX_V141=1 source=dcbst+dcbf+dcbi line=32 target=indexed-arrays policy=invalidate-overlap-no-per-draw-hash\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_TEXTURE_COHERENCY_V159=1 mutation=cache-control+host-dma bp66=tmem-only draw-scans=dirty-driven\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_EFB_COPY_V2=1 mode=retail-bp-state\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_EFB_COPY_FENCE_V78=1 ordering=render-worker-before+after trace=64\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_FIFO_CAP_V11=1 bytes=%u policy=fail-closed\n",
               16u*1024u*1024u);
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_FIFO_BURST_V13=1 abi=optional max=4096\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_FIFO_FASTPATH_V159=1 forensic=%u env=GEKKOAOT_NATIVE_GX_FIFO_FORENSIC texture-scans=dirty-only\n",g_fifo_forensic_trace?1u:0u);
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
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_WIDESCREEN_SOLID_SPANS_V343=%u ortho=untextured-full-width "
               "action=host-width env=GEKKOAOT_WIDESCREEN_SOLID_SPANS\n",
               g_wide_solid_spans ? 1u : 0u);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_WIDESCREEN_UI_INSETS_V348=%u span=inner+narrow confirm=displayed-uncopied "
               "action=ui-viewport+scissor env=GEKKOAOT_WIDESCREEN_UI_INSETS\n",
               g_ui_insets ? 1u : 0u);
  return true;
}

namespace {

bool AppendNativeFifoBytes(const std::uint8_t* bytes, std::size_t size,
                           std::uint32_t guest_pc) {
  if(!g_ready || g_fifo_fault || g_quit || !bytes || size==0u) return false;
  FlushEfbPokes();
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
  // Preserve the exact submitted stream for diagnosing command framing;
  // unlike draw captures this also retains CP state and split gather lines.
  static std::FILE* capture = [] {
    const char* path = std::getenv("GEKKOAOT_GX_DIAG_FIFO_CAPTURE");
    return path && *path ? std::fopen(path, "wb") : nullptr;
  }();
  if (capture) {
    (void)std::fwrite(bytes, 1u, size, capture);
    (void)std::fflush(capture);
  }
  // One forensic record per host burst is enough to retain the producer PC
  // while keeping the fast path independent from guest-store granularity.
  RememberFifoWrite(0u, static_cast<std::uint8_t>(std::min<std::size_t>(size,255u)),
                    g_fifo_stream_base+old, guest_pc);
  if(g_fifo_stream_stats) g_fifo_submitted_bytes += size;
  // v239: ProcessOne consumes control words immediately and copies vertex
  // payloads into owned staging before returning. The submitted span is never
  // retained by the renderer. Forensic mode keeps the original buffered path.
  if(!g_fifo_streaming || g_fifo_forensic_trace) {
    g_fifo.insert(g_fifo.end(), bytes, bytes+size);
    if(g_fifo_stream_stats) g_fifo_copied_bytes += size;
    if(g_fifo.size()-g_fifo_read>=g_fifo_need) DrainFifo();
    return !g_fifo_fault;
  }
  std::size_t offset=0;
  while(offset<size && !g_fifo_fault) {
    if(g_fifo_read<g_fifo.size()) {
      const std::size_t unread=g_fifo.size()-g_fifo_read;
      if(unread>=g_fifo_need) {
        DrainFifo();
        continue;
      }
      // Complete only the pending header/payload. A large draw may need more
      // than one burst; keep its complete bytes until the existing decoder can
      // consume it, then parse the remaining incoming commands directly.
      const std::size_t copied=std::min(size-offset,g_fifo_need-unread);
      g_fifo.insert(g_fifo.end(),bytes+offset,bytes+offset+copied);
      if(g_fifo_stream_stats) g_fifo_copied_bytes += copied;
      offset+=copied;
      if(g_fifo.size()-g_fifo_read>=g_fifo_need) DrainFifo();
      continue;
    }
    std::size_t length=0,need=1;
    const auto status=CommandLength(bytes+offset,size-offset,&length,&need);
    if(status!=DecodeResult::Complete) {
      // Preserve exact framing/error recovery and the old bounds for an
      // incomplete or invalid suffix. No command bytes are discarded here.
      g_fifo.insert(g_fifo.end(),bytes+offset,bytes+size);
      if(g_fifo_stream_stats) g_fifo_copied_bytes += size-offset;
      DrainFifo();
      break;
    }
    RememberDecodedCommand(bytes+offset,length,g_fifo_stream_base);
    if(!ProcessCommandOrRun(bytes+offset,size-offset,length,0u,&length)) {
      // Retain the failing command and suffix for the normal diagnostic dump;
      // do not replay a command that may already have changed GX state.
      g_fifo.insert(g_fifo.end(),bytes+offset,bytes+size);
      if(g_fifo_stream_stats) g_fifo_copied_bytes += size-offset;
      DumpFifoFault(0u);
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_FIFO_ERROR offset=0 opcode=0x%02x action=fail-closed-process\n",
                   unsigned(bytes[offset]));
      g_fifo_fault=true; g_quit=true;
      break;
    }
    g_fifo_stream_base+=length;
    g_fifo_need=1u;
    offset+=length;
    if(g_fifo_stream_stats) g_fifo_direct_bytes += length;
  }
  return !g_fifo_fault;
}

bool AppendNativeCommands(const std::uint8_t* bytes,std::size_t size,std::uint32_t pc) {
  if(!g_typed_arrays&&!g_direct_matrices) return AppendNativeFifoBytes(bytes,size,pc);
  return g_native_commands.Consume(bytes,size,
    [pc](const std::uint8_t* raw,std::size_t n){return AppendNativeFifoBytes(raw,n,pc);},
    []{return !g_fifo_fault && g_fifo_read==g_fifo.size();},
    [](GekkoAOT::GX::NativeCommandStream::ArrayBinding binding){
      if(!g_ready || g_fifo_fault || g_quit)return false;
      FlushEfbPokes();
      MapArrayBase(std::uint8_t(0xa0u+binding.index),binding.base);
      aurora::gx::fifo::handle_cp(std::uint8_t(0xb0u+binding.index),binding.stride);
      g_fifo_stream_base+=12u;
      if(g_fifo_stream_stats){g_fifo_submitted_bytes+=12u;g_fifo_direct_bytes+=12u;}
      return true;
    },
    [](std::uint32_t address,std::uint32_t words,const std::uint8_t* data){
      if(!g_ready || g_fifo_fault || g_quit || !GekkoAOT::GX::NativeMatrixRange(address,words))return false;
      FlushEfbPokes();
      // Native SDK upload: write the host renderer's matrix rows directly.
      // No opcode/header decoding, XF register dispatch or CPUState access.
      bool changed=false;
      auto store=[&](float& value,unsigned i) {
        const auto bits=Be32(data+4u*i);
        if(std::bit_cast<std::uint32_t>(value)!=bits) {
          value=std::bit_cast<float>(bits);changed=true;
        }
      };
      auto& state=aurora::gx::g_gxState;
      if(address<0x100u) {
        const auto row=address/4u;
        for(unsigned i=0;i<words;++i)store(state.xfPosRows[row+i/4u][i%4u],i);
      } else if(address<0x500u) {
        const auto row=(address-0x400u)/3u;
        for(unsigned i=0;i<9u;++i)store(state.xfNrmRows[row+i/3u][i%3u],i);
      } else {
        auto* matrix=reinterpret_cast<float*>(&state.ptTexMtxs[(address-0x500u)/12u]);
        for(unsigned i=0;i<12u;++i)store(matrix[i],i);
      }
      if(changed)state.dirty|=aurora::gx::DirtyUniform;
      const auto bytes=5u+4u*words;
      g_fifo_stream_base+=bytes;
      if(g_fifo_stream_stats){g_fifo_submitted_bytes+=bytes;g_fifo_direct_bytes+=bytes;}
      return true;
    });
}
} // namespace

// Aurora shader/presentation overlays query these live values. Keeping aspect
// outside ShaderConfig means resizing/switching modes only rebuilds uniforms,
// not GX pipelines or shaders.
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_render_extent(int* width, int* height) noexcept {
  const auto w = g_render_width.load(std::memory_order_relaxed);
  const auto h = g_render_height.load(std::memory_order_relaxed);
  if (w && h) { *width = int(w); *height = int(h); }
}
GEKKOAOT_GX_EXPORT float gekkoaot_native_gx_render_scale() noexcept {
  return float(g_render_percent.load(std::memory_order_relaxed))/100.f;
}
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_get_graphics_state(
    GekkoAOT::GX::GraphicsSettings* value, std::uint32_t bytes,
    std::uint32_t* extent) {
  if (!g_ready || !value || bytes != sizeof(*value) || !extent) return false;
  *value = g_video;
  extent[0] = aurora::webgpu::g_frameBuffer.size.width;
  extent[1] = aurora::webgpu::g_frameBuffer.size.height;
  extent[2] = aurora::webgpu::g_frameBuffer.texture.GetSampleCount();
  extent[3] = aurora::webgpu::g_graphicsConfig.textureAnisotropy;
  return true;
}
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_apply_graphics_settings(
    const GekkoAOT::GX::GraphicsSettings* value, std::uint32_t bytes) {
  // v359: older runtimes send the layout without the HUD fields.
  if(!g_ready || !value || (bytes!=sizeof(*value) && bytes!=GekkoAOT::GX::GraphicsSettingsPreHudSize &&
                            bytes!=GekkoAOT::GX::GraphicsSettingsPreUnlockSize))
    return false;
  auto next=g_video;
  std::memcpy(&next,value,bytes);
  if(next.render_percent<25 || next.render_percent>400 ||
     (next.msaa!=1 && next.msaa!=4) || next.anisotropy<1 || next.anisotropy>16 ||
     (next.anisotropy & (next.anisotropy-1))) return false;
  const bool quality=next.render_percent!=g_video.render_percent || next.render_width!=g_video.render_width ||
                     next.render_height!=g_video.render_height || next.msaa!=g_video.msaa ||
                     next.anisotropy!=g_video.anisotropy;
  const bool resume_frame = g_frame_open && quality;
  if(g_frame_open && quality) { aurora_end_frame_no_present(); g_frame_open=false; }
  // HostBridge has drained the parser queue. Retire render-worker packets before
  // changing their attachment/sample or sampler contracts. Never on draw paths.
  aurora::gfx::render_worker::synchronize();
  g_render_percent.store(next.render_percent,std::memory_order_relaxed);
  g_render_width.store(next.render_width); g_render_height.store(next.render_height);
  if(quality) {
    aurora::webgpu::g_graphicsConfig.msaaSamples=next.msaa;
    aurora::webgpu::g_graphicsConfig.textureAnisotropy=next.anisotropy;
    aurora::gfx::clear_caches();
    aurora::webgpu::refresh_surface(false);
    aurora::gx::g_gxState.dirty=aurora::gx::DirtyAll;
  }
  if(g_interpolation_requested!=bool(next.interpolation) || quality) {
    g_interp_previous={}; g_interp_current={};
    g_interp_previous_valid=g_interp_current_valid=false;
    g_interp_snapshot_count=0;
  }
  g_interpolation_requested=next.interpolation!=0;
  g_frame_interpolation_enabled=g_interpolation_requested;
  const bool fullscreen_changed=next.fullscreen!=g_video.fullscreen;
  const bool output_changed=next.output_width!=g_video.output_width || next.output_height!=g_video.output_height;
  g_video=next;
  GekkoAOT::GX::Hud::Configure(next.hud!=0,next.hud_items);
  g_replay_interpolate=next.unlock_predict==0;
  g_post_processing_enabled=next.post!=0;
  g_post_sharpen=next.sharpen;g_post_bloom=next.bloom;
  g_post_vignette=next.vignette;g_post_grain=next.grain;
  g_post_tone=next.tone;g_post_local_contrast=next.local_contrast;g_post_cinematic=next.cinematic;
  const char* embed=std::getenv("GEKKOAOT_EMBED_WINDOW");
  if((!embed || !*embed) && g_info.window) {
    if(output_changed && next.output_width && next.output_height &&
       (next.output_width!=g_info.windowSize.width || next.output_height!=g_info.windowSize.height)) {
      aurora::window::set_window_size(next.output_width,next.output_height);
      g_info.windowSize=aurora::window::get_window_size();
    }
    if(fullscreen_changed && aurora::window::get_fullscreen()!=bool(next.fullscreen))
      aurora::window::set_fullscreen(next.fullscreen!=0);
  }
  // The FIFO may immediately deliver XF viewport/scissor or draw commands.
  // Their GX helpers require a live recorder, even before the next GXCopyDisp.
  if (resume_frame && !g_quit && aurora_begin_frame()) g_frame_open = true;
  std::fprintf(stderr,"GEKKOAOT_GRAPHICS_LIVE render=%u msaa=%u aniso=%u post=%u fxaa=%u generation=%u efb=%ux%u\n",
      next.render_percent,next.msaa,next.anisotropy,next.post,next.fxaa,next.interpolation,
      aurora::webgpu::g_frameBuffer.size.width,aurora::webgpu::g_frameBuffer.size.height);
  return true;
}

GEKKOAOT_GX_EXPORT float gekkoaot_native_gx_widescreen_x_scale() noexcept {
  return ProjectionXScaleForMode(RecordAspectMode());
}
GEKKOAOT_GX_EXPORT float gekkoaot_native_gx_widescreen_target_aspect() noexcept {
  return TargetAspectForMode(g_present_force_43 ? 0u : g_aspect_mode.load(std::memory_order_acquire));
}

GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_camera_signature(
    float* fov_y_degrees, float* guest_aspect) noexcept {
  if (!fov_y_degrees || !guest_aspect) return false;
  const auto observed = g_camera_observed_copy.load(std::memory_order_acquire);
  const auto current = g_camera_copy_epoch.load(std::memory_order_acquire);
  const auto signature = g_camera_signature.load(std::memory_order_acquire);
  if (!signature || current < observed || current - observed > 2u) return false;
  const auto fov_bits = static_cast<std::uint32_t>(signature >> 32u);
  const auto aspect_bits = static_cast<std::uint32_t>(signature);
  std::memcpy(fov_y_degrees,&fov_bits,sizeof(float));
  std::memcpy(guest_aspect,&aspect_bits,sizeof(float));
  return true;
}

GEKKOAOT_GX_EXPORT float gekkoaot_native_gx_widescreen_projection_scale(
    float horizontal, float vertical, float width, float height) noexcept {
  if (RecordAspectMode() != 1u) return 1.f;
  return GekkoAOT::GX::PerspectiveXScale(horizontal, vertical, DisplayViewWidth(width),
                                         height, HostWindowAspect());
}

GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_texture_viewport(
    float left,float top,float width,float height) noexcept {
  return g_texture_viewport_roles.IsProducer(left,top,width,height,
      g_camera_copy_epoch.load(std::memory_order_relaxed));
}

GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_screen_fill() noexcept {
  return g_screen_fill_draw;
}

GEKKOAOT_GX_EXPORT float gekkoaot_native_gx_ui_clip_x(
    float x, float left, float width, float scale) noexcept {
  return GekkoAOT::GX::UiClipX(x,left,width,scale);
}

// v348: Aurora asks per draw whether a viewport/scissor span is a confirmed
// HUD inset, then maps its edges about the canvas centre like the UI.
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_ui_inset(float left, float width) noexcept {
  return g_ui_insets && RecordAspectMode() == 1u &&
         GekkoAOT::GX::IsInsetSpan(left, width, DisplayCanvasWidth()) &&
         g_display_insets.IsInset(left, width, g_display_copy_count);
}
// v362: the current perspective draw is a card of a 2D screen (UI scissor).
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_perspective_ui() noexcept {
  return g_draw_perspective_ui && RecordAspectMode() == 1u;
}
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_ui_full_view(float left, float width) noexcept {
  const float canvas = DisplayCanvasWidth();
  return canvas > 0.f && left <= canvas * .02f && left + width >= canvas * .98f;
}
GEKKOAOT_GX_EXPORT float gekkoaot_native_gx_ui_canvas_x(float x) noexcept {
  const float canvas = DisplayCanvasWidth();
  return GekkoAOT::GX::UiClipX(x, 0.f, canvas,
                               ProjectionXScaleForMode(RecordAspectMode()));
}

GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_screen_copy(
    int width, int height, float view_width, float view_height) noexcept {
  return GekkoAOT::GX::IsScreenCopy(width, height, view_width, view_height);
}
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_widescreen_diag(
    int projection_type, float view_left, float view_top,
    float view_width, float view_height, int scissor_x, int scissor_y,
    int scissor_width, int scissor_height, bool offscreen,
    bool samples_screen_copy, float correction) noexcept {
  // Called for every uniform build: resolve the diagnostic switch once.
  static const bool enabled = EnvBool("GEKKOAOT_DIAG_WIDESCREEN", false);
  if (!enabled) return;
  char state[320]{};
  const auto& gx=aurora::gx::g_gxState;
  const auto& tex0=gx.loadedTextures[0];
  std::snprintf(state, sizeof(state),
                "projection=%d guest=1.333333 host=%.6f viewport=%.1f,%.1f,%.1f,%.1f scissor=%d,%d,%d,%d offscreen=%u efb-sample=%u correction=%.6f fill=%u ztex=%u tev=%u blend=%u tex0=%ux%u:%u",
                projection_type, HostWindowAspect(), view_left, view_top,
                view_width, view_height, scissor_x, scissor_y,
                scissor_width, scissor_height, offscreen ? 1u : 0u,
                samples_screen_copy ? 1u : 0u, correction, g_screen_fill_draw ? 1u : 0u,
                unsigned(gx.zTexOp), unsigned(gx.numTevStages), unsigned(gx.blendMode),
                tex0.width(), tex0.height(), unsigned(tex0.raw_format()));
  static std::vector<std::string> seen;
  if (seen.size() < 4096u && std::find(seen.begin(), seen.end(), std::string(state)) == seen.end()) {
    seen.emplace_back(state);
    std::fprintf(stderr, "GEKKOAOT_DIAG_WIDESCREEN %s\n", state);
  }
}
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_set_aspect_mode(std::uint32_t mode) {
  aurora::gfx::render_worker::synchronize();
  ApplyAspectMode(mode, true);
}

GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_set_pe_callback(HostPeEventFn fn, void* user) { g_pe=fn; g_pe_user=user; }
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_sdk_array(std::uint32_t index,std::uint32_t base,
                                                   std::uint32_t stride,std::uint32_t before) {
  if(g_ready && !g_quit && !g_fifo_fault && g_typed_arrays)
    (void)g_native_commands.Stage({index,base,stride},before);
}
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_write_commands(const std::uint8_t* bytes,
    std::uint32_t size,std::uint32_t pc,const void* metadata,std::uint32_t count) {
  if(!bytes || !size || size>4096u || count>64u || (!metadata&&count))return;
  if(!g_typed_arrays){(void)AppendNativeCommands(bytes,size,pc);return;}
  const auto* commands=static_cast<const GekkoAOT::GX::NativeArrayCommand*>(metadata);
  // Validate metadata before applying any operation. Older/invalid producers
  // still submit the complete transport through the compatibility decoder.
  for(unsigned i=0;i<count;++i) {
    if(commands[i].offset>=size || commands[i].index>=16 ||
       (i && commands[i].offset<commands[i-1].offset+12u)) {
      (void)AppendNativeCommands(bytes,size,pc);return;
    }
  }
  unsigned offset=0;
  for(unsigned i=0;i<count;++i) {
    const auto& c=commands[i];
    if(c.offset>offset && !AppendNativeCommands(bytes+offset,c.offset-offset,pc))return;
    (void)g_native_commands.Stage({c.index,c.base,c.stride},0);
    offset=c.offset;
  }
  if(offset<size)(void)AppendNativeCommands(bytes+offset,size-offset,pc);
}
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_write_sdk_commands_v317(const std::uint8_t* bytes,
    std::uint32_t size,std::uint32_t pc,const void* metadata,std::uint32_t count) {
  using namespace GekkoAOT::GX;
  if(!bytes || !size || size>4096u || count>64u || (!metadata&&count))return;
  const auto* commands=static_cast<const NativeSdkCommand*>(metadata);
  for(unsigned i=0;i<count;++i) {
    const auto& c=commands[i];
    const bool valid=c.kind==NativeSdkKind::Array ? c.a<16u :
      c.kind==NativeSdkKind::Matrix && c.c==0u && NativeMatrixRange(c.a,c.b);
    if(!valid || c.offset>=size ||
       (i && c.offset<commands[i-1].offset+NativeSdkWireBytes(commands[i-1]))) {
      (void)AppendNativeCommands(bytes,size,pc);return;
    }
  }
  unsigned offset=0;
  for(unsigned i=0;i<count;++i) {
    const auto& c=commands[i];
    if(c.offset>offset && !AppendNativeCommands(bytes+offset,c.offset-offset,pc))return;
    if(c.kind==NativeSdkKind::Array && g_typed_arrays)
      (void)g_native_commands.Stage({c.a,c.b,c.c},0);
    else if(c.kind==NativeSdkKind::Matrix && g_direct_matrices)
      (void)g_native_commands.StageMatrix(c.a,c.b,0);
    offset=c.offset;
  }
  if(offset<size)(void)AppendNativeCommands(bytes+offset,size-offset,pc);
}
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_write(std::uint64_t value, std::uint8_t size,
                                                  std::uint32_t guest_pc) {
  if(size!=1&&size!=2&&size!=4&&size!=8) return;
  std::uint8_t bytes[8]{};
  for(std::uint8_t i=0;i<size;++i)
    bytes[i]=static_cast<std::uint8_t>(value>>((size-1u-i)*8u));
  (void)AppendNativeCommands(bytes,size,guest_pc);
}
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_write_burst(const std::uint8_t* bytes,
                                                        std::uint32_t size,
                                                        std::uint32_t guest_pc) {
  if(size==0u || size>4096u) return;
  (void)AppendNativeCommands(bytes,size,guest_pc);
}
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_cache_control(std::uint8_t operation,
                                                         std::uint32_t address) {
  if(!g_ready || g_fifo_fault || g_quit) return;
  NotifyGuestVertexCacheControl(operation,address);
}
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_memory_write(std::uint32_t address,
                                                        std::uint32_t size) {
  if(!g_ready || g_fifo_fault || g_quit || size==0u) return;
  NotifyGuestMemoryWriteRange(address,size);
}
// v162: CPU-visible EFB Z aperture for retail GXPeekZ loads. Aurora's own
// GXCpu2Efb implementation is intentionally asynchronous: use the newest
// completed depth snapshot, return 0 before the first snapshot exists, and
// request another snapshot for the next observation. Returning true means the
// CPU access was handled even when the bootstrap value is zero.
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_peek_z(std::uint16_t x, std::uint16_t y,
                                                   std::uint32_t* z) {
  if(!g_ready || g_fifo_fault || g_quit || z==nullptr) return false;
  FlushEfbPokes();
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
  FlushEfbPokes();
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
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_read_bbox(std::uint16_t* bounds) {
  if (!g_ready || g_fifo_fault || g_quit || !bounds) return false;
  const bool active = GekkoAOT::GX::NativeBoundingBoxActive();
  if (active) {
    FlushEfbPokes();
    if (g_frame_open) {
      // PE reads complete prior raster work without performing a guest EFB
      // clear or display copy. Continue with both attachments loaded.
      aurora::gfx::gekkoaot_preserve_efb_on_next_begin(true, true);
      aurora_end_frame_no_present();
      g_frame_open = false;
    }
    aurora::gfx::render_worker::synchronize();
  }
  const bool result = GekkoAOT::GX::ReadNativeBoundingBox(bounds);
  if (active && !g_quit && aurora_begin_frame()) g_frame_open = true;
  return result;
}
// CPU EFB pokes are ordered after the pending WGPIPE batch and before the next
// guest-visible draw/copy. Reuse Aurora's partial clear draw so MSAA, resource
// lifetime and pass continuation follow the ordinary EFB command stream.
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_poke_efb(std::uint16_t x, std::uint16_t y,
                                                     std::uint32_t value, bool depth) {
  if (!g_ready || !g_frame_open || g_fifo_fault || g_quit || x >= 640u || y >= 528u)
    return false;
  QueueEfbPoke(x,y,value,depth);
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
// v300 optional ABI: same VI scanout as present_xfb_ex, executed by the host's
// GX worker in FIFO order. It never pumps window events.
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_present_xfb_async(
    std::uint32_t top, std::uint32_t bottom, std::uint32_t width,
    std::uint32_t stride_bytes, std::uint32_t field_height) {
  if (g_fifo_fault || g_quit) return;
  PresentViXfb(top,bottom,true,width,stride_bytes,field_height,false);
}
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_consume_frame_ready(std::uint32_t* xfb_address) {
  if(!g_ready) return false;
  const auto xfb = g_frame_ready_xfb.exchange(0, std::memory_order_acq_rel);
  if (xfb_address) *xfb_address = xfb;
  return xfb != 0;
}
// Optional ABI: this poll only touches the atomic mailbox and is safe while
// the parser worker is running. Renderer state still requires a host fence
// after a non-empty result, and all MMIO/FIFO fences retain their old ordering.
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_consume_frame_ready_async(std::uint32_t* xfb_address) {
  const auto xfb = g_frame_ready_xfb.exchange(0, std::memory_order_acq_rel);
  if (xfb_address) *xfb_address = xfb;
  return xfb != 0;
}
// v358: extrapolate the current frame's uniform blocks from the previous
// frame (alpha = elapsed time over the last frame interval) and re-render it.
// Runs on the render worker, after the frame it replays was submitted.
void ReplayFrame(const ReplayCapture& current, const ReplayCapture& previous, float alpha) {
  std::vector<std::uint8_t> bytes(current.bytes);
  std::vector<aurora::gfx::detail::GekkoaotReplayWrite> writes;
  writes.reserve(current.blocks.size());
  std::uint64_t matched=0, moved=0, cuts=0;
  for (const auto& block : current.blocks) {
    writes.push_back({block.offset,block.size,block.data});
    const auto it=previous.index.find(block.key);
    if (it==previous.index.end()) continue;
    const auto& prior=previous.blocks[it->second];
    if (prior.size!=block.size || prior.proj!=block.proj) continue;
    ++matched;
    const auto motion=GekkoAOT::GX::ExtrapolateUniformBlock(
        current.bytes.data()+block.data,previous.bytes.data()+prior.data,block.size,block.proj,block.pnmtx,
        aurora::gx::MaxXfPosUniformRows,alpha,bytes.data()+block.data);
    if (motion!=GekkoAOT::GX::ReplayMotion::Unchanged) ++moved;
    if (motion==GekkoAOT::GX::ReplayMotion::Cut) ++cuts;
  }
  auto& stats=g_replay_stats;
  stats.blocks+=current.blocks.size(); stats.matched+=matched; stats.moved+=moved; stats.cuts+=cuts;
  // Mostly unmatched or mostly cut: a different scene, not the next pose.
  const bool frame_cut=matched*2u<current.blocks.size() || (moved>=8u && cuts*2u>moved);
  if (frame_cut) ++stats.frame_cuts;
  bool presented=false;
  if (!frame_cut && aurora::gfx::detail::gekkoaot_replay_submit(current.frame_id,bytes.data(),writes.data(),writes.size())) {
    PresentFinalTexture(current.present);
    presented=true;
    ++stats.replays; stats.alpha+=alpha;
  } else {
    ++stats.skipped;
  }
  const auto total=stats.replays+stats.skipped;
  if (presented && (total==30u || total%600u==0u))
    std::fprintf(stderr,"GEKKOAOT_FRAME_REPLAY_V358 replays=%llu skipped=%llu frame_cuts=%llu blocks/frame=%.0f matched=%.1f%% moved=%.1f%% cuts=%.1f%% alpha=%.2f\n",
                 static_cast<unsigned long long>(stats.replays),static_cast<unsigned long long>(stats.skipped),
                 static_cast<unsigned long long>(stats.frame_cuts),double(stats.blocks)/double(total),
                 stats.blocks ? 100.0*double(stats.matched)/double(stats.blocks) : 0.0,
                 stats.matched ? 100.0*double(stats.moved)/double(stats.matched) : 0.0,
                 stats.moved ? 100.0*double(stats.cuts)/double(stats.moved) : 0.0,
                 stats.replays ? stats.alpha/double(stats.replays) : 0.0);
}

bool ReplayReady() {
  std::lock_guard lock{g_replay_mutex};
  return g_replay_current && g_replay_previous && g_replay_current->eligible;
}

bool PresentReplay() {
  std::shared_ptr<const ReplayCapture> current, previous;
  {
    std::lock_guard lock{g_replay_mutex};
    current=g_replay_current; previous=g_replay_previous;
  }
  g_replay_requests.fetch_add(1,std::memory_order_relaxed);
  if (!current || !previous || !current->eligible) return false;
  const auto now=std::chrono::steady_clock::now();
  const double period=std::chrono::duration<double>(current->produced-previous->produced).count();
  if (!(period>0.004 && period<0.25)) {
    g_replay_declined_period.fetch_add(1,std::memory_order_relaxed);
    return false;
  }
  // Never predict past the next real frame: a late frame holds the last pose.
  double alpha=std::min(std::chrono::duration<double>(now-current->produced).count()/period,1.0);
  // Interpolation shows the previous pose when the newest frame arrives, so
  // a direct present of that frame would jump ahead and back: while replays
  // run, a declined tick still counts as presented.
  const bool hold=g_replay_interpolate;
  if (g_replay_interpolate) alpha-=1.0;
  else if (alpha<0.05) {
    g_replay_declined_early.fetch_add(1,std::memory_order_relaxed);
    return false;
  }
  // At most one present per display refresh.
  const auto now_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
  if (double(now_ns-g_last_present_ns.load(std::memory_order_relaxed))<0.9e9/DisplayRefreshHz()) {
    g_replay_declined_period.fetch_add(1,std::memory_order_relaxed);
    return hold;
  }
  if (g_replay_busy.exchange(true,std::memory_order_acq_rel)) {
    g_replay_declined_busy.fetch_add(1,std::memory_order_relaxed);
    return hold;
  }
  g_replay_queued.fetch_add(1,std::memory_order_relaxed);
  aurora::gfx::render_worker::enqueue_work([current=std::move(current),previous=std::move(previous),alpha] {
    ReplayFrame(*current,*previous,static_cast<float>(alpha));
    g_replay_busy.store(false,std::memory_order_release);
  });
  return true;
}

// v359: live runtime values for the performance HUD (Hud::RuntimeValue order).
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_publish_perf(const float* values, std::uint32_t count) {
  GekkoAOT::GX::Hud::PublishRuntime(values,count);
}
// Aurora's own scanout/blend presents draw the HUD through this hook.
void gekkoaot_native_gx_draw_overlay(const wgpu::CommandEncoder& encoder, const wgpu::TextureView& view,
                                     std::uint32_t width, std::uint32_t height) noexcept {
  GekkoAOT::GX::Hud::Draw(encoder,view,aurora::webgpu::g_graphicsConfig.surfaceConfiguration.format,width,height);
}

GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_replay_capture() noexcept {
  return g_replay_capture_on.load(std::memory_order_relaxed);
}
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_replay_uniform(std::uint32_t offset, const std::uint8_t* data,
                                                         std::uint32_t size, std::uint32_t proj) noexcept {
  auto& capture=g_replay_building;
  if (!data || proj+64u>size || capture.bytes.size()+size>(64u<<20)) return;
  // The n-th block a draw signature builds in a frame pairs with its n-th
  // block in the previous frame.
  std::uint64_t key=g_replay_draw_sig;
  key=(key^g_replay_occurrences[g_replay_draw_sig]++)*1099511628211ull;
  capture.blocks.push_back({key,offset,size,proj,static_cast<std::uint32_t>(capture.bytes.size()),
                            std::min<std::uint32_t>(g_replay_draw_pnmtx,aurora::gx::MaxXfPosRows-3u)});
  capture.bytes.insert(capture.bytes.end(),data,data+size);
}

GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_frame_interpolation_ready() {
  if (g_ready && g_frame_interpolation_enabled && g_replay_mode) return ReplayReady();
  return g_ready && g_frame_interpolation_enabled && g_interp_previous_valid && g_interp_current_valid;
}
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_present_interpolated(float alpha) {
  if (g_ready && g_frame_interpolation_enabled && g_replay_mode) {
    const bool presented=PresentReplay();
    PollEvents();
    return presented;
  }
  if(!g_ready || !g_frame_interpolation_enabled || !g_interp_previous_valid || !g_interp_current_valid)
    return false;
  const bool presented =
      PresentInterpolationBlend(g_interp_previous, g_interp_current, std::clamp(alpha,0.0f,1.0f));
  PollEvents();
  return presented;
}
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_should_quit(){ PollEvents(); return g_quit; }
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_set_paused(bool paused) {
  if (!g_ready) return;
  // Retire the current recorder before processing resize/surface events while
  // paused. Resume with a fresh recorder, preserving the guest GX state/EFB.
  if (paused && g_frame_open) {
    aurora_end_frame_no_present();
    g_frame_open = false;
    aurora::gfx::render_worker::synchronize();
  } else if (!paused && !g_frame_open && !g_quit && aurora_begin_frame()) {
    g_frame_open = true;
  }
}
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_shutdown(){
  if(!g_ready) return;
  if(g_native_vertex_gather)
    std::fprintf(stderr,"GEKKOAOT_NATIVE_VERTEX_GATHER_V318_STATS draws=%llu packed_bytes=%llu source_window_bytes=%llu\n",
      static_cast<unsigned long long>(g_native_gather_draws),
      static_cast<unsigned long long>(g_native_gather_bytes),
      static_cast<unsigned long long>(g_native_gather_windows));
  if(g_typed_arrays)
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_TYPED_COMMANDS_V313_STATS arrays=%llu fallback=%llu declined=%llu\n",
      static_cast<unsigned long long>(g_native_commands.NativeCalls()-g_native_commands.MatrixCalls()),
      static_cast<unsigned long long>(g_native_commands.FallbackCalls()),
      static_cast<unsigned long long>(g_native_commands.DeclinedHints()));
  if(g_direct_matrices)
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_DIRECT_MATRICES_V317_STATS matrices=%llu fallback=%llu\n",
      static_cast<unsigned long long>(g_native_commands.MatrixCalls()),
      static_cast<unsigned long long>(g_native_commands.FallbackCalls()));
  if(g_fifo_stream_stats)
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_STREAM_FIFO_V239_STATS submitted=%llu copied=%llu direct=%llu forensic=%u\n",
                 static_cast<unsigned long long>(g_fifo_submitted_bytes),
                 static_cast<unsigned long long>(g_fifo_copied_bytes),
                 static_cast<unsigned long long>(g_fifo_direct_bytes),
                 g_fifo_forensic_trace?1u:0u);
  FlushEfbPokes();
  if (g_efb_poke_stats)
    std::fprintf(stderr,"GEKKOAOT_EFB_POKE_V164_STATS writes=%llu draws=%llu\n",
                 static_cast<unsigned long long>(g_efb_poke_writes),
                 static_cast<unsigned long long>(g_efb_poke_draws));
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
  aurora::gfx::render_worker::synchronize();
  GekkoAOT::GX::ShutdownNativeBoundingBox();
  // Release optional host presentation resources while Dawn's device and
  // window/WSI are alive; function-static handles must not defer this to exit.
  UniversalPostFxResources()={};
  BlendResources()={};
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
               "GEKKOAOT_NATIVE_GX_ARRAY_UPLOAD_V190_STATS persistent_uploads=%llu persistent_bytes=%llu adaptive_fallbacks=%llu content_hits=%llu\n",
               static_cast<unsigned long long>(g_persistent_array_uploads),
               static_cast<unsigned long long>(g_persistent_array_upload_bytes),
               static_cast<unsigned long long>(g_adaptive_array_fallbacks),
               static_cast<unsigned long long>(g_content_array_hits));
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_COMPACT_ARRAY_WINDOWS_V293_STATS uploads=%llu bytes=%llu original_prefix_bytes=%llu rollovers=%llu fallbacks=%llu\n",
               static_cast<unsigned long long>(g_compact_array_uploads),
               static_cast<unsigned long long>(g_compact_array_bytes),
               static_cast<unsigned long long>(g_compact_array_prefix_bytes),
               static_cast<unsigned long long>(g_compact_array_rollovers),
               static_cast<unsigned long long>(g_compact_array_fallbacks));
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_SHUTDOWN_V12=1 order=drain-endframe-events-aurora dso=pinned\n");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_ARRAY_SNAPSHOTS_V303_STATS staged=%llu\n",
               static_cast<unsigned long long>(g_native_array_snapshot_stages));
  const auto index_stats=g_native_index_ranges.Statistics();
  std::fprintf(stderr,"GEKKOAOT_NATIVE_INDEX_RANGES_V307_STATS hits=%llu misses=%llu admitted=%llu rejected=%llu\n",
      static_cast<unsigned long long>(index_stats.hits),static_cast<unsigned long long>(index_stats.misses),
      static_cast<unsigned long long>(index_stats.admitted),static_cast<unsigned long long>(index_stats.rejected));
  g_fifo.clear();
  g_native_commands.Reset();
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
  g_texture_image3_written=0;
  g_texture_ui_source_hash.fill(0);
  g_texture_ui_source_hash_valid=0;
  g_array_frame_refresh=false;
  g_array_available.fill(0);
  g_array_guest_base.fill(0);
  for(auto& slot : g_persistent_array_slots) slot = {};
  g_persistent_array_cursor=0;
  g_persistent_array_frame=UINT32_MAX;
  g_persistent_array_reset_pending=false;
  g_persistent_array_cache=false;
  g_persistent_array_min_bytes=8u*1024u;
  g_persistent_array_uploads=0;
  g_persistent_array_upload_bytes=0;
  g_persistent_array_hits=0;
  g_persistent_array_fallbacks=0;
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
  g_display_insets.Reset();
  g_flat_frames.Reset(); g_frame_depth_draws=0; g_draw_perspective_ui=false;
  g_frame_prerendered=false; g_prerendered_idle=0; g_frame_force_43.store(false,std::memory_order_relaxed);
  g_copy_provenance.Reset();
  g_camera_signature.store(0,std::memory_order_relaxed);
  g_camera_observed_copy.store(0,std::memory_order_relaxed);
  g_camera_copy_epoch.store(0,std::memory_order_relaxed);
  g_camera_area_epoch=~std::uint64_t{0};g_camera_best_area=0.f;
  g_camera_coefficients={};g_camera_cached_signature=0;
  g_camera_published_signature=0;
  g_camera_published_copy=~std::uint64_t{0};
  g_interp_previous={}; g_interp_current={};
  g_interp_previous_valid=false; g_interp_current_valid=false; g_interp_snapshot_count=0;
  g_fifo_fault=false;
  g_resolve=nullptr;
  g_resolve_user=nullptr;
  g_pe=nullptr;
  g_pe_user=nullptr;
}
