// SPDX-License-Identifier: GPL-3.0-or-later
// GekkoAOT direct retail GX -> Aurora bridge. No emulator runtime.
#include <aurora/aurora.h>
#include "../native_index_range_cache.h"
#include "../native_attribute_plan.h"
#include "../native_vertex_program.h"
#include "../native_index_bounds.h"
#include "../native_vertex_gather.h"
#include "../native_command_stream.h"
#include "../native_display_list_cache.h"
#include "../native_draw_ir.h"
#include "../render_graph.h"
#include "../scene_evidence.h"
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx/GXCommandList.h>
#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXAurora.h>
#include "gx/command_processor.hpp"
#include "gx/regs.hpp"
#include "gx/gx.hpp"
#include <aurora/texture.hpp>
#include "../../perf/capture.h"
#include "gx/texture.hpp"
#include "gfx/render_worker.hpp"
#include "gfx/clear.hpp"
#include "gfx/recording.hpp"
#include "gfx/color_peek.hpp"
#include "gfx/depth_peek.hpp"
#include "gfx/gekkoaot_enhanced_renderer.hpp"
#include "gfx/gekkoaot_hd_resources.hpp"
#include "gfx/gekkoaot_shadow_replay.hpp"
#include "../scene_camera_selector.h"
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
#include "../projection_fov.h"

#include <algorithm>
#include "../widescreen_policy.h"
#include "dynamic_texture_shaders.h"
#include "../frame_replay_policy.h"
#include "../frame_replay_clock.h"
#include <xxhash.h>
#include <unordered_set>
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
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <optional>

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

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

// GEKKOAOT_GX_PHASE_PROFILE=1: exclusive TSC cycles per bridge phase on the
// calling thread, reported with the runtime profile (diagnostic only).
namespace {
enum GxPhase : unsigned {
  kPhOutside, kPhAppend, kPhState, kPhListExec, kPhListCompile, kPhDraw, kPhDrawPrep,
  kPhDrawSubmit, kPhOperation, kPhPresent,
  kPhAObserve, kPhAGeometry, kPhAVerts, kPhAPipeline, kPhAStorage, kPhABind, kPhAUniform, kPhAEmit,
  kPhBInval, kPhBStatic, kPhBMaterial, kPhBGroups, kPhBCopy,
  kPhPGather, kPhPWindows, kPhPCompact,
  kPhCount
};
constexpr const char* kGxPhaseNames[kPhCount] = {
  "outside", "append", "state", "list_exec", "list_compile", "draw", "draw_prep",
  "draw_submit", "operation", "present",
  "a_observe", "a_geometry", "a_verts", "a_pipeline", "a_storage", "a_bind", "a_uniform", "a_emit",
  "b_inval", "b_static", "b_material", "b_groups", "b_copy",
  "p_gather", "p_windows", "p_compact"
};
bool g_phase_profile = false;
std::array<std::uint64_t, 512> g_fifo_census{};  // FIFO op / BP reg / XF range counts
std::uint64_t g_replay_census_calls = 0, g_replay_census_same = 0;
std::array<std::uint64_t, 5> g_replay_census_diff{};
thread_local unsigned g_phase_cur = kPhOutside;
thread_local std::uint64_t g_phase_start = 0;
thread_local std::array<std::uint64_t, kPhCount> g_phase_cycles{}, g_phase_calls{};
struct PhaseScope {
  unsigned saved = kPhOutside;
  bool on;
  explicit PhaseScope(unsigned phase) : on(g_phase_profile) {
    if (!on) return;
    const std::uint64_t now = __rdtsc();
    g_phase_cycles[g_phase_cur] += now - g_phase_start;
    saved = g_phase_cur;
    g_phase_cur = phase;
    g_phase_start = now;
    ++g_phase_calls[phase];
  }
  ~PhaseScope() {
    if (!on) return;
    const std::uint64_t now = __rdtsc();
    g_phase_cycles[g_phase_cur] += now - g_phase_start;
    g_phase_cur = saved;
    g_phase_start = now;
  }
};
}  // namespace
extern "C" void gekkoaot_texture_totals_v389(std::uint64_t* out);
extern "C" unsigned gekkoaot_gx_phase_enter(unsigned phase) {
  if (!g_phase_profile) return 0u;
  const std::uint64_t now = __rdtsc();
  g_phase_cycles[g_phase_cur] += now - g_phase_start;
  const unsigned saved = g_phase_cur;
  g_phase_cur = phase;
  g_phase_start = now;
  ++g_phase_calls[phase];
  return saved;
}
extern "C" void gekkoaot_gx_phase_leave(unsigned saved) {
  if (!g_phase_profile) return;
  const std::uint64_t now = __rdtsc();
  g_phase_cycles[g_phase_cur] += now - g_phase_start;
  g_phase_cur = saved;
  g_phase_start = now;
}
namespace {
void ReportGxPhases() {
  if (!g_phase_profile) return;
  const std::uint64_t now = __rdtsc();
  g_phase_cycles[g_phase_cur] += now - g_phase_start;
  g_phase_start = now;
  std::uint64_t total = 0;
  for (unsigned i = 1; i < kPhCount; ++i) total += g_phase_cycles[i];
  std::string line;
  for (unsigned i = 1; i < kPhCount; ++i) {
    char item[96];
    std::snprintf(item, sizeof(item), " %s=%.1fMc/%llu", kGxPhaseNames[i], double(g_phase_cycles[i]) / 1e6,
                  static_cast<unsigned long long>(g_phase_calls[i]));
    line += item;
  }
  std::fprintf(stderr, "GEKKOAOT_GX_PHASES_V389 inside=%.1fMc%s\n", double(total) / 1e6, line.c_str());
  {
    static std::uint64_t last[4]{};
    std::uint64_t now_stats[4]{};
    gekkoaot_texture_totals_v389(now_stats);
    std::fprintf(stderr, "GEKKOAOT_GX_TEXTURE_CACHE_V389 object_hits=%llu content_hits=%llu misses=%llu hashed_kb=%llu\n",
                 static_cast<unsigned long long>(now_stats[0] - last[0]),
                 static_cast<unsigned long long>(now_stats[1] - last[1]),
                 static_cast<unsigned long long>(now_stats[2] - last[2]),
                 static_cast<unsigned long long>((now_stats[3] - last[3]) >> 10u));
    std::copy(now_stats, now_stats + 4, last);
  }
  g_phase_cycles.fill(0u);
  g_phase_calls.fill(0u);
  std::vector<std::pair<std::uint64_t, unsigned>> top;
  for (unsigned i = 0; i < g_fifo_census.size(); ++i) if (g_fifo_census[i]) top.emplace_back(g_fifo_census[i], i);
  std::sort(top.begin(), top.end(), std::greater<>());
  std::string census;
  for (std::size_t i = 0; i < top.size() && i < 16u; ++i) {
    char item[48];
    const unsigned k = top[i].second;
    if (k >= 256u) std::snprintf(item, sizeof(item), " bp%02x=%llu", k - 256u, static_cast<unsigned long long>(top[i].first));
    else if (k >= 128u) std::snprintf(item, sizeof(item), " xf%02x00=%llu", k - 128u, static_cast<unsigned long long>(top[i].first));
    else std::snprintf(item, sizeof(item), " op%02x=%llu", k << 3u, static_cast<unsigned long long>(top[i].first));
    census += item;
  }
  std::fprintf(stderr, "GEKKOAOT_GX_FIFO_CENSUS_V389%s\n", census.c_str());
  std::fprintf(stderr, "GEKKOAOT_GX_REPLAY_CENSUS_V389 list_calls=%llu same_entry_state=%llu diff(bp,cp,xf,tex,arrays)=%llu,%llu,%llu,%llu,%llu\n",
               static_cast<unsigned long long>(std::exchange(g_replay_census_calls, 0u)),
               static_cast<unsigned long long>(std::exchange(g_replay_census_same, 0u)),
               static_cast<unsigned long long>(g_replay_census_diff[0]), static_cast<unsigned long long>(g_replay_census_diff[1]),
               static_cast<unsigned long long>(g_replay_census_diff[2]), static_cast<unsigned long long>(g_replay_census_diff[3]),
               static_cast<unsigned long long>(g_replay_census_diff[4]));
  g_replay_census_diff.fill(0u);
  g_fifo_census.fill(0u);
}
}  // namespace

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
aurora::texture::ReplacementGroup g_texture_pack;
bool g_quit = false;
bool g_frame_open = false;
// v128b renderer-level aspect mode. Keep only the two user-facing policies:
// exact 4:3, or a live window-aspect mode with projection compensation.
std::atomic<std::uint32_t> g_aspect_mode{0u};
// Pre-rendered scenes (a full-screen backdrop writing depth from a
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
// Logical width of the last GXCopyDisp (the frame the VI scans out).
std::atomic<std::uint32_t> g_display_copy_width{640u};
// ProjectionAspect normalizes a viewport against a 640-wide 4:3 display.
// Titles with a narrower EFB (Nemo: 512x480, stretched to the full 4:3 screen
// by the VI scaler) render a 4:3 camera into it; measure the viewport against
// the width of the frame the VI actually scans out (the last GXCopyDisp).
float DisplayViewWidth(float width) noexcept {
  const float display = static_cast<float>(g_display_copy_width.load(std::memory_order_relaxed));
  return display >= 320.f && display < 636.f && width <= display * 1.02f ? width * 640.f / display : width;
}

// HUD insets (viewport/scissor spans clear of both canvas edges) follow
// the UI transform once a frame shows them without copying them to a texture.
bool g_ui_insets = true;
GekkoAOT::GX::DisplayInsets g_display_insets;
// Screen-derived EFB copies (bloom/blur pyramids) keep the retail width.
bool g_copy_provenance_enabled = true;
bool g_wide_perspective_fills = true;
// Perspective cards of a 2D screen (no depth-writing 3D for a few
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
  std::uint64_t sig=0; // the draw signature alone (key adds the occurrence)
  float view[4]{};      // logical viewport (EFB pixels) when it was built
  bool direct=false;    // positions come with the vertices (immediate mode)
  bool producer=false;  // renders a small texture copy (shadow map, mask)
};
// A texture copy of the frame: the blocks built before it and its EFB rect.
struct ReplayCopy { std::uint32_t blocks=0; float rect[4]{}; };
// A position/normal array staged for a frame. Arrays the game rewrote since
// the previous frame (CPU skinning, deformation) keep a copy of their bytes:
// replays blend them toward the previous frame's copy of the same array.
struct ReplayArray {
  std::uint64_t key=0, hash=0;
  std::uint32_t storage=0, size=0, stride=0, copy=0;
  std::uint8_t comp=0, count=0;
  bool little_endian=false, copied=false;
};
struct ReplayCapture {
  std::uint64_t frame_id=0;
  std::vector<ReplayBlock> blocks;
  std::vector<std::uint8_t> bytes;
  std::unordered_map<std::uint64_t,std::uint32_t> index;
  // Blocks of one draw signature, in drawing order.
  std::unordered_map<std::uint64_t,std::vector<std::uint32_t>> groups;
  std::vector<ReplayArray> arrays;
  std::vector<ReplayCopy> copies;
  std::vector<std::uint8_t> vertex_bytes;
  std::unordered_map<std::uint64_t,std::uint32_t> array_index;
  std::chrono::steady_clock::time_point produced{};
  aurora::webgpu::TextureWithSampler present;
  bool eligible=false;
  // Per block: view-space motion since its partner one frame earlier (set
  // when this frame is paired with its predecessor, render worker only).
  mutable std::vector<std::array<float,3>> motion;
};
bool g_replay_mode = true;
// Default: show poses between the previous and the newest guest frame (no
// overshoot, one guest frame of latency). GEKKOAOT_REPLAY_PREDICT=1 instead
// predicts past the newest frame (no added latency, overshoots on sudden
// direction changes).
bool g_replay_interpolate = true;
bool g_vsync_default = false;
bool g_vsync_applied = false; // the swapchain's present mode (GX thread)
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
// GX thread: content hash of each array key in the last captured frame, the
// storage ranges already noted in the frame being built, and the suffix
// prefix of compact array windows staged for the current draw.
std::unordered_map<std::uint64_t,std::uint64_t> g_replay_array_last;
std::unordered_set<std::uint64_t> g_replay_array_seen;
std::array<std::uint32_t, GX_VA_MAX_ATTR> g_replay_compact_prefix{};
std::uint32_t g_replay_compact_mask = 0;
struct ReplayVertexStats { std::uint64_t arrays=0, animated=0, animated_bytes=0, blended=0, refused=0; };
ReplayVertexStats g_replay_vertex_stats; // render worker
// FPS metrics kept apart (never one blended "FPS"): replays presented, and
// those showing a pose that differs from the previous present (a held clock
// re-presents the same pose).
std::atomic<std::uint64_t> g_metric_replays{0}, g_metric_unique_replays{0};
// Game timeline of the captured frames and the shown game time of replays
// (guarded by g_replay_mutex).
GekkoAOT::GX::ReplayClock g_replay_clock;
double ReplaySeconds(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double>(t.time_since_epoch()).count();
}
std::shared_ptr<const ReplayCapture> g_replay_current, g_replay_previous;
// Replays queued on the render worker and not yet started: at most one waits
// behind the real frame (or the running replay); later ticks coalesce into it.
std::atomic<bool> g_replay_waiting{false};
// Real frames come first. A replay starts only once the previous one has
// left the GPU and its measured cost still fits before the next guest frame.
// Unpaced, a saturated GPU backs the render worker up into the GX thread and the
// guest falls to the 30 fps VI lock while the display shows hundreds of replays.
std::atomic<std::int64_t> g_replay_gpu_start_ns{0}, g_replay_cost_ns{0};
std::atomic<bool> g_replay_poll_waiting{false};
std::atomic<std::int64_t> g_replay_tick_ns{0}, g_replay_last_tick_ns{0}, g_replay_frame_cost_ns{0}, g_replay_period_ns{0};
// Guest frames that already have an image on its way, and the pacing state: a
// frame that leaves no room for a replay is simply shown as it is.
std::atomic<std::uint64_t> g_replay_shown_frame{0};
std::atomic<bool> g_replay_direct{false};
std::atomic<std::uint64_t> g_replay_direct_ticks{0};
// Newest frame whose replay was refused: it has been shown directly, and
// further attempts on it would only burn the render worker.
std::atomic<std::uint64_t> g_replay_refused_frame{0};
std::atomic<double> g_replay_last_frame_s{0.0};
std::atomic<std::uint64_t> g_replay_declined_deadline{0};
// Diagnostics: captures/eligible (GX thread), requests and declines (host thread).
std::atomic<std::uint64_t> g_replay_captures{0}, g_replay_eligible{0}, g_replay_requests{0},
    g_replay_declined_early{0}, g_replay_declined_period{0}, g_replay_declined_busy{0}, g_replay_queued{0};
struct ReplayStats {
  std::uint64_t replays=0, skipped=0, blocks=0, matched=0, moved=0, cuts=0, frame_cuts=0, followed=0, camera=0, producers=0;
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

// Untextured full-width orthographic spans also bypass the UI x-scale.
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
bool g_fifo_native_ir = false, g_native_ir_profile = false;
GekkoAOT::GX::NativeDisplayListCache g_native_display_lists;
GekkoAOT::GX::NativeDrawListCache g_native_draw_lists;
std::uint64_t g_native_draw_list_executed=0;
struct NativeIrStats {
  std::uint64_t sdk = 0, fifo = 0, arrays = 0, matrices = 0, unchanged_matrices = 0;
  std::uint64_t sdk_ns = 0, fifo_ns = 0, compatibility = 0, compatibility_ns = 0;
  std::uint64_t compatibility_transitions = 0, decoder_units = 0, graph_copies = 0;
  std::uint64_t sdk_command_units = 0;
  std::uint64_t vertex_states=0, projections=0, viewports=0, transform_states=0, pixel_states=0;
  std::uint64_t dl_operations_executed=0, dl_commands_reused=0;
  bool in_compatibility = false;
} g_native_ir_stats;
GekkoAOT::GX::SceneEvidence g_scene_evidence;
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
struct IndexRangeCacheField {std::uint16_t offset;std::uint8_t width,count,attr,reserved;};
struct IndexRangeCacheLayout {
  std::uint16_t stride,count;std::uint8_t minimumRequired,reserved[3];
  std::array<IndexRangeCacheField,12> fields;
};
static_assert(sizeof(IndexRangeCacheLayout)<=GekkoAOT::GX::NativeIndexRangeCache::LayoutBytes);
struct IndexScanLayout {
  std::uint16_t stride = 0;
  std::uint8_t field_count = 0;
  std::uint8_t valid = 0;
  std::uint8_t has_index8 = 0;  // v336: skip the eager INDEX8 pass when none
  std::array<IndexScanField, 12> fields{};
  GekkoAOT::GX::NativeAttributePlan bindings{};
  GekkoAOT::GX::NativeVertexProgram native{};
  GekkoAOT::GX::NativeIndexBounds index_bounds{};
  GekkoAOT::GX::NativeVertexGather gather{};
  std::array<IndexRangeCacheLayout,2> range_keys{};
};
std::array<IndexScanLayout, 8> g_index_scan_layout{};
std::uint8_t g_index_scan_valid = 0;
GekkoAOT::GX::NativeIndexRangeCache g_native_index_ranges;
bool g_native_index_range_cache=false;
bool g_native_attribute_plan=true;
bool g_native_vertex_program=false;
bool g_native_index_bounds=true;
bool g_native_vertex_gather=false;
std::uint64_t g_native_gather_draws=0,g_native_gather_bytes=0,g_native_gather_windows=0;
GekkoAOT::GX::NativeVertexGather::Workspace g_native_gather_workspace;
bool g_native_vertex_profile=false;
std::uint64_t g_native_vertex_draws=0, g_native_vertex_vertices=0;
std::array<std::uint32_t, 8> g_texture_image3_word{};
// Avoid recalculating the same GX texture footprint for every guest cache-line
// publication. IMAGE0/size/mips may change without an IMAGE3 write, so all
// four descriptor fields are checked at the read site (no stale watch spans).
struct TextureWriteFootprint {
  std::uint32_t format=0, width=0, height=0, mips=0;
  std::uint64_t bytes=0;
  bool valid=false;
};
std::array<TextureWriteFootprint, 8> g_texture_write_footprints{};
std::uint64_t g_texture_write_span_hits=0, g_texture_write_span_misses=0;
std::uint64_t g_guest_memory_write_notifications=0, g_guest_memory_write_noops=0;
std::array<std::uint32_t, 8> g_texture_mapped_image0{};
std::array<std::uint32_t, 8> g_texture_mapped_mips{};
std::uint8_t g_texture_image3_written = 0;
std::array<std::uint32_t, GX_VA_MAX_ATTR> g_array_available{};
// Retain the authoritative guest base separately from Aurora's host
// pointer/visible byte window. CPU D-cache maintenance publishes dynamic
// vertex data to GX; use that boundary to invalidate only overlapping GPU
// snapshots instead of hashing vertex arrays on every draw.
std::array<std::uint32_t, GX_VA_MAX_ATTR> g_array_guest_base{};

// Persistent GPU-side snapshots for indexed GX arrays.
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
// Terminator 3 needs in-place font/UI texture changes to invalidate a
// stable IMAGE3 binding, but forcing a new texture version on every low-bit
// format rebind also hits the bitmap-heavy Monkey Ball render path. Remember
// the source bytes instead and only break the stable-bind fast path on a real
// content change.
std::array<std::uint64_t, 8> g_texture_ui_source_hash{};
std::uint8_t g_texture_ui_source_hash_valid = 0;
// Retail games issue many draws between texture-state changes. Avoid
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
std::mutex g_camera_projection_mutex;
std::array<float,6> g_camera_projection{};
std::array<float,6> g_camera_producer_projection{};
std::atomic<std::uint64_t> g_camera_observed_copy{0};
std::atomic<std::uint64_t> g_camera_copy_epoch{0};
std::atomic<std::uint64_t> g_camera_vi_epoch{0};
GekkoAOT::GX::SceneCameraSelector g_scene_camera_selector;
struct RecentCamera {std::array<float,8> lens{};std::uint64_t epoch=0;bool valid=false;};
std::array<RecentCamera,32> g_camera_candidates{},g_camera_candidates_producer{};
// Consecutive perspective draws normally share one lens: derive its FOV/aspect
// and refresh the candidate table only when the lens (or VI epoch) changes.
struct CameraObservationMemo {
  std::array<float,4> key{};
  std::array<float,8> lens{};
  float fov=0.f,aspect=0.f;
  std::uint64_t epoch=0;
  bool derived=false,published=false;
} g_camera_memo;
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
std::atomic<float> g_fov_y{0};
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
  // Match GX's actual logical EFB/target mapping. 528 is the hardware address
  // limit, not the configured EFB height (commonly 480); scaling by it moved
  // CPU pokes to the wrong row even at native render resolution.
  const auto mapped=aurora::gx::map_logical_scissor({poke.x,poke.y,poke.width,poke.height});
  const auto left=std::clamp(mapped.x,0,static_cast<std::int32_t>(size.x));
  const auto top=std::clamp(mapped.y,0,static_cast<std::int32_t>(size.y));
  const auto right=std::clamp(mapped.x+mapped.width,left,static_cast<std::int32_t>(size.x));
  const auto bottom=std::clamp(mapped.y+mapped.height,top,static_cast<std::int32_t>(size.y));
  if(left==right || top==bottom)return;
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
             .width=right-left,.height=bottom-top}});
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
  std::uint64_t capture_frame = 0;
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
std::uint64_t g_trace_native_draws = 0;  // GEKKOAOT_XFB_COPY_TRACE diagnostic

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
std::uint64_t g_tex_census_font = 0, g_tex_census_font_bytes = 0;

// Texture source versions between TMEM invalidations. Every non-stable
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
std::array<TextureSourceVersion, 2048> g_texture_source_versions{};
std::uint32_t g_texture_source_lo = 0xffffffffu, g_texture_source_hi = 0u;
bool g_texture_version_reuse = true;
bool g_texture_source_ids = true;
bool g_copy_range_forget = true;
std::uint64_t g_tex_census_reused = 0;

std::uint64_t g_tex_new_empty = 0, g_tex_new_other = 0, g_tex_new_span = 0;
std::uint64_t g_tex_forget_all = 0, g_tex_forget_range = 0, g_tex_forget_entries = 0, g_tex_forget_bytes = 0;
void ForgetTextureSourceVersions() {
  ++g_tex_forget_all;
  if (g_texture_source_hi == 0u) return;
  g_texture_source_versions.fill({});
  g_texture_source_lo = 0xffffffffu;
  g_texture_source_hi = 0u;
}

void ForgetTextureSourceRange(std::uint32_t address, std::uint32_t size) {
  const std::uint64_t end = std::uint64_t(address) + size;
  if (size == 0u || address >= g_texture_source_hi || end <= g_texture_source_lo) return;
  ++g_tex_forget_range; g_tex_forget_bytes += size;
  for (auto& entry : g_texture_source_versions)
    if (entry.version != 0u && address < std::uint64_t(entry.guest) + entry.span && entry.guest < end) {
      entry = {};
      ++g_tex_forget_entries;
    }
}

TextureSourceVersion& TextureSourceSlot(std::uint32_t guest, std::uint32_t image0, std::uint8_t mips) {
  const std::uint32_t h = (guest >> 5u) * 0x9E3779B1u ^ image0 * 0x85EBCA77u ^ mips;
  return g_texture_source_versions[(h >> 7u) & (g_texture_source_versions.size() - 1u)];
}

// Host write watch over texture source pages (HostBridge). Armed when a
// source is versioned, collected at every TMEM invalidation.
using TextureWatchArmFn = void (*)(void* user, std::uint32_t physical, std::uint32_t size);
using TextureWatchCollectFn = std::int32_t (*)(void* user, std::uint32_t* pages, std::uint32_t capacity);
TextureWatchArmFn g_texture_watch_arm = nullptr;
TextureWatchCollectFn g_texture_watch_collect = nullptr;
void* g_texture_watch_user = nullptr;
std::uint64_t g_tex_watch_collects = 0, g_tex_watch_pages = 0, g_tex_watch_fallbacks = 0;

void RememberTextureSourceVersion(std::uint32_t guest, std::uint32_t image0, std::uint8_t mips,
                                  std::uint32_t span, std::uint32_t version) {
  if (!g_texture_version_reuse || span == 0u) return;
  // Arm before Aurora reads the source: a later write is always reported.
  if (g_texture_watch_arm) g_texture_watch_arm(g_texture_watch_user, guest, span);
  TextureSourceSlot(guest, image0, mips) = {guest, image0, span, version, mips};
  g_texture_source_lo = std::min(g_texture_source_lo, guest);
  g_texture_source_hi = static_cast<std::uint32_t>(
      std::min<std::uint64_t>(0xffffffffu, std::max<std::uint64_t>(g_texture_source_hi, std::uint64_t(guest) + span)));
}

// Aurora's texture object cache is keyed by texObjId. One id per
// hardware unit made every texture switch on a unit miss it and take the
// content-key path (source hash memo + content map, ~1.9k cycles per bind,
// HPCOS 64k binds/s). A source owns the id of its version-table slot, so a
// rebind of a known source is an object-cache hit; the cache still compares
// address, size, format, mips and version before reusing a texture, and a
// slot taken over by another source simply replaces the entry. Ids 1..8 stay
// the per-unit fallback.
constexpr std::uint32_t kTextureSourceObjectIdBase = 9u;
std::uint32_t TextureSourceObjectId(std::uint32_t guest, std::uint32_t image0, std::uint8_t mips) {
  return kTextureSourceObjectIdBase +
         static_cast<std::uint32_t>(&TextureSourceSlot(guest, image0, mips) - g_texture_source_versions.data());
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
    if (g_draw_census) {
      if (entry.version == 0u) ++g_tex_new_empty;
      else if (entry.guest != guest || entry.image0 != image0 || entry.mips != mips) ++g_tex_new_other;
      else ++g_tex_new_span;
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
  std::fprintf(stderr, "GEKKOAOT_TEXTURE_WATCH_STATS_V389 collects=%llu pages=%llu fallbacks=%llu\n",
               static_cast<unsigned long long>(std::exchange(g_tex_watch_collects, 0u)),
               static_cast<unsigned long long>(std::exchange(g_tex_watch_pages, 0u)),
               static_cast<unsigned long long>(std::exchange(g_tex_watch_fallbacks, 0u)));
  std::fprintf(stderr, "GEKKOAOT_TEXTURE_NEW_V389 empty=%llu other=%llu span=%llu\n",
               static_cast<unsigned long long>(std::exchange(g_tex_new_empty, 0u)),
               static_cast<unsigned long long>(std::exchange(g_tex_new_other, 0u)),
               static_cast<unsigned long long>(std::exchange(g_tex_new_span, 0u)));
  std::fprintf(stderr, "GEKKOAOT_TEXTURE_FORGET_V389 all=%llu range=%llu entries=%llu kb=%llu\n",
               static_cast<unsigned long long>(std::exchange(g_tex_forget_all, 0u)),
               static_cast<unsigned long long>(std::exchange(g_tex_forget_range, 0u)),
               static_cast<unsigned long long>(std::exchange(g_tex_forget_entries, 0u)),
               static_cast<unsigned long long>(std::exchange(g_tex_forget_bytes, 0u) >> 10u));
  std::fprintf(stderr, "GEKKOAOT_TEXTURE_FONT_BINDS_V389 binds=%llu kb=%llu\n",
               static_cast<unsigned long long>(std::exchange(g_tex_census_font, 0u)),
               static_cast<unsigned long long>(std::exchange(g_tex_census_font_bytes, 0u) >> 10u));
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


// Some retail titles (notably FMV/video paths) write YUVY XFBs directly
// into MEM1 and point VI at them without ever issuing GXCopyDisp. Keep the
// existing zero-copy cache as the fast path; this texture is only the RAM-XFB
// compatibility path used after an address-keyed cache miss.
aurora::gfx::TextureHandle g_ram_xfb_texture;
aurora::webgpu::TextureWithSampler g_ram_xfb_present;
std::vector<std::uint8_t> g_ram_xfb_rgba;
struct RamXfbUploadSlot { std::vector<std::uint8_t> bytes; bool busy=false; };
std::array<RamXfbUploadSlot,3> g_ram_xfb_uploads;
std::mutex g_ram_xfb_upload_mutex;
std::condition_variable g_ram_xfb_upload_ready;
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
// Environment variables are immutable for one renderer instance.  Never
// call getenv() from FIFO/texture hot paths; cache diagnostic/coherency toggles
// once during gekkoaot_native_gx_init().
bool g_trace_gx_copy = false;
bool g_trace_video = false;
bool g_invalidate_texture_sources = true;
bool g_texture_source_invalidation_pending = false;
// The pending invalidation was resolved by the write watch: bound textures
// whose source version survived it keep their version.
bool g_texture_source_watched_invalidation = false;
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

// The widescreen scissor/projection helpers run for every GX draw. Each
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
  // The direct VI scanout already uses the live GekkoAOT aspect, but the
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

  // Keep producer-side GX/EFB geometry in the GameCube coordinate
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
#ifdef __APPLE__
  if(EqualNoCase(v,"metal")) return BACKEND_METAL;
#endif
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

#include "bridge_vertex_arrays.cpp"
#include "bridge_copies.cpp"
#include "bridge_present.cpp"
#include "bridge_xfb_textures.cpp"
#include "bridge_fifo.cpp"


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
  // ProcessOne consumes control words immediately and copies vertex
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
  PhaseScope phase_scope(kPhAppend);
  if(!g_typed_arrays&&!g_direct_matrices) return AppendNativeFifoBytes(bytes,size,pc);
  return g_native_commands.ConsumeOperations(bytes,size,
    [pc](const std::uint8_t* raw,std::size_t n){return AppendNativeFifoBytes(raw,n,pc);},
    []{return !g_fifo_fault && g_fifo_read==g_fifo.size();},
    [](const GekkoAOT::GX::NativeOperation& operation){
      if (!SubmitNativeOperation(operation, GekkoAOT::GX::NativeOrigin::Sdk)) return false;
      const auto* matrix = std::get_if<GekkoAOT::GX::NativeMatrixUpload>(&operation);
      const auto bytes = matrix ? 5u + 4u * matrix->words : 12u;
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
// Explicit view-space depth-replay validation, not a retail shadow toggle.
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_shadow_depth_validation(const float* view_to_light_clip,
                                                                 std::uint32_t resolution) {
  if(!g_ready || g_quit || g_fifo_fault)return false;
  DrainFifo();
  aurora::gfx::render_worker::synchronize();
  return aurora::gfx::shadow::validation_view(view_to_light_clip,resolution);
}
GEKKOAOT_GX_EXPORT std::uint64_t gekkoaot_native_gx_pending_texture_bytes(){return aurora::gfx::hd::pending_bytes();}

GEKKOAOT_GX_EXPORT std::uint32_t gekkoaot_native_gx_graphics_settings_size() {
  return sizeof(GekkoAOT::GX::GraphicsSettings);
}
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_get_graphics_state(
    GekkoAOT::GX::GraphicsSettings* value, std::uint32_t bytes,
    std::uint32_t* extent) {
  if (!g_ready || !value || !extent || (bytes != sizeof(*value) &&
      bytes != GekkoAOT::GX::GraphicsSettingsPreHudSize &&
      bytes != GekkoAOT::GX::GraphicsSettingsPreUnlockSize &&
      bytes != GekkoAOT::GX::GraphicsSettingsPreEnhancedSize &&
      bytes != GekkoAOT::GX::GraphicsSettingsPreFovSize &&
      bytes != GekkoAOT::GX::GraphicsSettingsPreFragmentSize &&
      bytes != GekkoAOT::GX::GraphicsSettingsPreEnvironmentSize &&
      bytes != GekkoAOT::GX::GraphicsSettingsPreSunShadowsSize &&
      bytes != GekkoAOT::GX::GraphicsSettingsPreGiSize &&
      bytes != GekkoAOT::GX::GraphicsSettingsPreVsyncSize &&
      bytes != GekkoAOT::GX::GraphicsSettingsPreHdSize &&
      bytes != GekkoAOT::GX::GraphicsSettingsPreMaterialSize &&
      bytes != GekkoAOT::GX::GraphicsSettingsPreCompareSize &&
      bytes != GekkoAOT::GX::GraphicsSettingsPreGeometrySize &&
      bytes != GekkoAOT::GX::GraphicsSettingsPreTextureBackendSize && bytes != GekkoAOT::GX::GraphicsSettingsPreDisplacementSize && bytes != GekkoAOT::GX::GraphicsSettingsPreDofSize && bytes != GekkoAOT::GX::GraphicsSettingsPreParticleLightsSize && bytes != GekkoAOT::GX::GraphicsSettingsPreParticleRadiusSize && bytes != GekkoAOT::GX::GraphicsSettingsPreSurfaceSize)) return false;
  // Match the size-negotiated setter. Old clients own only their ABI prefix;
  // assigning a full current struct would overwrite the following memory.
  std::memcpy(value, &g_video, bytes);
  extent[0] = aurora::webgpu::g_frameBuffer.size.width;
  extent[1] = aurora::webgpu::g_frameBuffer.size.height;
  extent[2] = aurora::webgpu::g_frameBuffer.texture.GetSampleCount();
  extent[3] = aurora::webgpu::g_graphicsConfig.textureAnisotropy;
  return true;
}
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_apply_graphics_settings(
    const GekkoAOT::GX::GraphicsSettings* value, std::uint32_t bytes) {
  // Older runtimes send the layout without the HUD fields.
  if(!g_ready || !value || (bytes!=sizeof(*value) && bytes!=GekkoAOT::GX::GraphicsSettingsPreHudSize &&
                            bytes!=GekkoAOT::GX::GraphicsSettingsPreUnlockSize && bytes!=GekkoAOT::GX::GraphicsSettingsPreEnhancedSize && bytes!=GekkoAOT::GX::GraphicsSettingsPreFovSize && bytes!=GekkoAOT::GX::GraphicsSettingsPreFragmentSize &&
                            bytes!=GekkoAOT::GX::GraphicsSettingsPreEnvironmentSize &&
                            bytes!=GekkoAOT::GX::GraphicsSettingsPreSunShadowsSize &&
                            bytes!=GekkoAOT::GX::GraphicsSettingsPreGiSize &&
                            bytes!=GekkoAOT::GX::GraphicsSettingsPreVsyncSize &&
                            bytes!=GekkoAOT::GX::GraphicsSettingsPreHdSize &&
                            bytes!=GekkoAOT::GX::GraphicsSettingsPreMaterialSize &&
                            bytes!=GekkoAOT::GX::GraphicsSettingsPreCompareSize &&
                            bytes!=GekkoAOT::GX::GraphicsSettingsPreGeometrySize &&
                            bytes!=GekkoAOT::GX::GraphicsSettingsPreTextureBackendSize && bytes!=GekkoAOT::GX::GraphicsSettingsPreDisplacementSize && bytes!=GekkoAOT::GX::GraphicsSettingsPreDofSize && bytes!=GekkoAOT::GX::GraphicsSettingsPreParticleLightsSize && bytes!=GekkoAOT::GX::GraphicsSettingsPreParticleRadiusSize && bytes!=GekkoAOT::GX::GraphicsSettingsPreSurfaceSize))
    return false;
  auto next=g_video;
  std::memcpy(&next,value,bytes);
  if(next.render_percent<25 || next.render_percent>400 ||
     (next.msaa!=1 && next.msaa!=4) || next.anisotropy<1 || next.anisotropy>16 ||
     (next.anisotropy & (next.anisotropy-1))) return false;
  if(!std::isfinite(next.fov_y)||(next.fov_y!=0&&(next.fov_y<20||next.fov_y>150)))return false;
  if(next.enhanced>1||next.ao_quality>3||next.contact_shadows>1||next.fragment_lighting>1)return false;
  if(next.particle_lights>2 || !std::isfinite(next.particle_light_strength) || next.particle_light_strength<0 || next.particle_light_strength>4)return false;
  if(!std::isfinite(next.particle_light_radius) || next.particle_light_radius<.5f || next.particle_light_radius>6.f)return false;
  if(next.dof_quality>2 || !std::isfinite(next.dof_strength) || next.dof_strength<0 || next.dof_strength>1 ||
     !std::isfinite(next.dof_focus) || next.dof_focus<1 || next.dof_focus>500)return false;
  if(next.displacement>1 || next.texture_backend>2 || next.geometry_refinement>2 || next.material_compare>1 || next.material_enhancement>1 || next.pom_quality>2 || !std::isfinite(next.material_normal_strength) ||
     next.material_normal_strength<0 || next.material_normal_strength>2 || !std::isfinite(next.material_relief) ||
     next.material_relief<0 || next.material_relief>.05f ||
     !std::isfinite(next.surface_detail) || next.surface_detail<0 || next.surface_detail>1 ||
     !std::isfinite(next.surface_specular) || next.surface_specular<0 || next.surface_specular>1)return false;
  if((next.texture_scale!=1 && next.texture_scale!=2 && next.texture_scale!=4) || next.texture_quality>1 ||
     next.hd_budget_mib<64 || next.hd_budget_mib>65536 || next.hd_cache_mib<64 || next.hd_cache_mib>65536)return false;
  if(next.environment>2||next.sun_preset>=unsigned(GekkoAOT::GX::SunPreset::Count)||next.sun_shadows>2||
     next.gi>1||next.ssr>1||!std::isfinite(next.gi_strength)||!std::isfinite(next.ssr_strength)||
     next.gi_strength<0||next.gi_strength>2||next.ssr_strength<0||next.ssr_strength>4)return false;
  const float environmentValues[]{next.environment_strength,next.sun_intensity,next.sun_temperature,
    next.sun_azimuth,next.sun_elevation,next.sky_intensity};
  for(float f:environmentValues)if(!std::isfinite(f))return false;
  if(next.environment_strength<0||next.environment_strength>1||next.sun_intensity<0||next.sun_intensity>4||
     next.sun_temperature<1500||next.sun_temperature>12000||std::abs(next.sun_azimuth)>360||
     next.sun_elevation<1||next.sun_elevation>90||next.sky_intensity<0||next.sky_intensity>2)return false;
  const float enhancedValues[]{next.ao_strength,next.ao_radius,next.shadow_strength,next.shadow_distance,
    next.shadow_thickness,next.shadow_softness,next.light_x,next.light_y,next.light_z};
  for(float f:enhancedValues)if(!std::isfinite(f))return false;
  if(next.ao_strength<0||next.ao_strength>2||next.ao_radius<.01f||next.ao_radius>1000||
     next.shadow_strength<0||next.shadow_strength>1||next.shadow_distance<.01f||next.shadow_distance>10000||
     next.shadow_thickness<.001f||next.shadow_thickness>100||next.shadow_softness<0||next.shadow_softness>1||
     std::abs(next.light_x)>1||std::abs(next.light_y)>1||std::abs(next.light_z)>1||
     (next.contact_shadows && next.light_x*next.light_x+next.light_y*next.light_y+next.light_z*next.light_z<1e-6f))return false;
  const bool quality=next.enhanced!=g_video.enhanced || next.render_percent!=g_video.render_percent || next.render_width!=g_video.render_width ||
                     next.render_height!=g_video.render_height || next.msaa!=g_video.msaa ||
                     next.anisotropy!=g_video.anisotropy;
  const bool projection_change=next.fov_y!=g_video.fov_y;
  const bool resume_frame = g_frame_open && (quality||projection_change);
  if(g_frame_open && (quality||projection_change)) { aurora_end_frame_no_present(); g_frame_open=false; }
  // HostBridge has drained the parser queue. Retire render-worker packets before
  // changing their attachment/sample or sampler contracts. Never on draw paths.
  aurora::gfx::render_worker::synchronize();
  g_render_percent.store(next.render_percent,std::memory_order_relaxed);
  g_render_width.store(next.render_width); g_render_height.store(next.render_height);
  if(next.fov_y!=g_video.fov_y) {
    g_fov_y.store(next.fov_y,std::memory_order_relaxed);
    aurora::gx::g_gxState.dirty|=aurora::gx::DirtyUniform;
    std::lock_guard lock{g_replay_mutex};
    g_replay_current.reset();g_replay_previous.reset();
    g_interp_previous_valid=g_interp_current_valid=false;
  }
  aurora::gfx::enhanced::configure(next);
  if(quality) {
    aurora::gfx::enhanced::reset();
    aurora::webgpu::g_graphicsConfig.enhancedCapture=next.enhanced!=0;
    aurora::webgpu::g_graphicsConfig.normalBuffer=next.enhanced!=0;
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
  g_vsync_default=next.vsync!=0; // applied at the next frame boundary
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

GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_camera_projection(float* output,unsigned count) noexcept {
  float fov=0,aspect=0;
  if(!output || count!=6 || !gekkoaot_native_gx_camera_signature(&fov,&aspect))return false;
  std::lock_guard lock(g_camera_projection_mutex);
  std::copy(g_camera_projection.begin(),g_camera_projection.end(),output);
  return true;
}
GEKKOAOT_GX_EXPORT unsigned gekkoaot_native_gx_camera_projections(float* output,unsigned capacity) noexcept {
  if(!output||capacity==0)return 0;
  const auto epoch=g_camera_vi_epoch.load(std::memory_order_acquire);
  std::lock_guard lock(g_camera_projection_mutex);unsigned count=0;
  for(const auto& camera:g_camera_candidates)
    if(camera.valid && epoch>=camera.epoch && epoch-camera.epoch<=6 && count<std::min(capacity,32u)){
      std::copy(camera.lens.begin(),camera.lens.end(),output+8*count);++count;
    }
  return count;
}

GEKKOAOT_GX_EXPORT float gekkoaot_native_gx_fov_scale(float vertical) noexcept {
  if(g_draw_perspective_ui)return 1.f;
  return GekkoAOT::GX::PerspectiveFovScale(vertical,g_fov_y.load(std::memory_order_relaxed));
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

// Aurora asks per draw whether a viewport/scissor span is a confirmed
// HUD inset, then maps its edges about the canvas centre like the UI.
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_ui_inset(float left, float width) noexcept {
  return g_ui_insets && RecordAspectMode() == 1u &&
         GekkoAOT::GX::IsInsetSpan(left, width, DisplayCanvasWidth()) &&
         g_display_insets.IsInset(left, width, g_display_copy_count);
}
// The current perspective draw is a card of a 2D screen (UI scissor).
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
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_set_texture_watch_v389(TextureWatchArmFn arm,
    TextureWatchCollectFn collect, void* user) {
  // Sources versioned before the watch was armed were never protected.
  ForgetTextureSourceVersions();
  g_texture_watch_arm=arm && collect ? arm : nullptr;
  g_texture_watch_collect=arm && collect ? collect : nullptr;
  g_texture_watch_user=arm && collect ? user : nullptr;
}
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
// Feature negotiation prevents a new host from sending pixel metadata to an
// older DSO whose v317 ABI only accepts arrays/matrices.
GEKKOAOT_GX_EXPORT std::uint32_t gekkoaot_native_gx_sdk_features(){return 7u;}
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_write_sdk_commands_v317(const std::uint8_t* bytes,
    std::uint32_t size,std::uint32_t pc,const void* metadata,std::uint32_t count) {
  using namespace GekkoAOT::GX;
  if(!bytes || !size || size>4096u || count>64u || (!metadata&&count))return;
  const auto* commands=static_cast<const NativeSdkCommand*>(metadata);
  for(unsigned i=0;i<count;++i) {
    const auto& c=commands[i];
    const bool valid=c.kind==NativeSdkKind::Array ? c.a<16u :
      (c.kind==NativeSdkKind::Matrix && c.c==0u && NativeMatrixRange(c.a,c.b)) ||
      (c.kind==NativeSdkKind::Pixel && c.c==0u && c.a<=255u && c.b<=0xffffffu && [&]{
        NativePixelState pixel;return LowerNativeBp(c.a,c.b,pixel);}());
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
    else if(c.kind==NativeSdkKind::Pixel)
      (void)g_native_commands.StagePixel(c.a,c.b,0);
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
// CPU-visible EFB Z aperture for retail GXPeekZ loads. Aurora's own
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
// Matching CPU-visible EFB color aperture for GXPeekARGB. This mirrors
// the asynchronous depth path: consume the newest completed logical-frame
// snapshot, bootstrap with zero, then request a fresher snapshot.
GEKKOAOT_GX_EXPORT bool gekkoaot_native_gx_peek_argb(std::uint16_t x, std::uint16_t y,
                                                      std::uint32_t* argb) {
  if(!g_ready || g_fifo_fault || g_quit || argb==nullptr) return false;
  FlushEfbPokes();
  aurora::gfx::enhanced::note_color_peek();
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
// Debug (GEKKOAOT_REPLAY_DUMP=<dir>): while <dir>/trigger exists, the next 24
// replays write their output as f<guest frame>_t<blend position x1000>.ppm and
// the trigger is removed: one guest frame pair seen at several positions.
#pragma push_macro("Success")
#undef Success // X11
void DumpReplay(const ReplayCapture& current, double position) {
  static const char* dir=std::getenv("GEKKOAOT_REPLAY_DUMP");
  static std::chrono::steady_clock::time_point checked{};
  static bool armed=false;
  static unsigned dumped=0;
  if (!dir || !current.present.texture) return;
  const auto now=std::chrono::steady_clock::now();
  if (!armed) {
    if (now-checked<std::chrono::milliseconds(250)) return;
    checked=now;
    armed=std::filesystem::exists(std::filesystem::path(dir)/"trigger");
    dumped=0;
    if (!armed) return;
  }
  const auto& texture=current.present.texture;
  const auto format=texture.GetFormat();
  const bool bgra=format==wgpu::TextureFormat::BGRA8Unorm;
  if (!bgra && format!=wgpu::TextureFormat::RGBA8Unorm) { armed=false; return; }
  const std::uint32_t width=texture.GetWidth(), height=texture.GetHeight(), row=(width*4u+255u)&~255u;
  const wgpu::BufferDescriptor descriptor{.label="GekkoAOT replay dump",
                                          .usage=wgpu::BufferUsage::CopyDst|wgpu::BufferUsage::MapRead,
                                          .size=std::uint64_t(row)*height};
  auto readback=aurora::webgpu::g_device.CreateBuffer(&descriptor);
  auto encoder=aurora::webgpu::g_device.CreateCommandEncoder();
  const wgpu::TexelCopyTextureInfo from{.texture=texture};
  const wgpu::TexelCopyBufferInfo to{.layout={.bytesPerRow=row,.rowsPerImage=height},.buffer=readback};
  const wgpu::Extent3D extent{width,height,1};
  encoder.CopyTextureToBuffer(&from,&to,&extent);
  const auto command=encoder.Finish();
  aurora::webgpu::g_queue.Submit(1,&command);
  bool mapped=false;
  const auto future=readback.MapAsync(wgpu::MapMode::Read,0,descriptor.size,wgpu::CallbackMode::WaitAnyOnly,
      [&mapped](wgpu::MapAsyncStatus status,wgpu::StringView){ mapped=status==wgpu::MapAsyncStatus::Success; });
  if (aurora::webgpu::g_instance.WaitAny(future,2000000000ull)!=wgpu::WaitStatus::Success || !mapped) return;
  const auto* pixels=static_cast<const std::uint8_t*>(readback.GetConstMappedRange());
  char name[64];
  std::snprintf(name,sizeof(name),"f%06llu_t%05d.ppm",static_cast<unsigned long long>(current.frame_id),
                int(std::lround(position*1000.0)));
  if (std::FILE* f=std::fopen((std::filesystem::path(dir)/name).string().c_str(),"wb")) {
    std::fprintf(f,"P6\n%u %u\n255\n",width,height);
    std::vector<std::uint8_t> line(width*3u);
    for (std::uint32_t y=0;y<height;++y) {
      const auto* p=pixels+std::size_t(y)*row;
      for (std::uint32_t x=0;x<width;++x) {
        line[x*3u]=p[x*4u+(bgra ? 2u : 0u)]; line[x*3u+1u]=p[x*4u+1u]; line[x*3u+2u]=p[x*4u+(bgra ? 0u : 2u)];
      }
      std::fwrite(line.data(),1,line.size(),f);
    }
    std::fclose(f);
  }
  readback.Unmap();
  if (++dumped>=24u) {
    armed=false;
    std::error_code ignored;
    std::filesystem::remove(std::filesystem::path(dir)/"trigger",ignored);
  }
}
#pragma pop_macro("Success")

// Extrapolate the current frame's uniform blocks from the previous
// frame (alpha = elapsed time over the last frame interval) and re-render it.
// Runs on the render worker, after the frame it replays was submitted.
void ReplayFrame(const ReplayCapture& current, const ReplayCapture& previous, float alpha) {
  std::vector<std::uint8_t> bytes(current.bytes);
  std::vector<aurora::gfx::detail::GekkoaotReplayWrite> writes;
  writes.reserve(current.blocks.size());
  std::uint64_t matched=0, moved=0, cuts=0, followed=0, producers=0;
  // Rigid matrix decompositions depend only on the two guest frames: cached
  // by matrix content (many blocks share the camera/world matrices) until the
  // pair of frames changes; each replay only slerps.
  static std::uint64_t cached_current=0, cached_previous=0;
  static std::unordered_map<std::uint64_t,GekkoAOT::GX::RigidPair> rigid_cache;
  static std::vector<std::int32_t> partner;
  bool pairing_cached=true;
  if (cached_current!=current.frame_id || cached_previous!=previous.frame_id) {
    rigid_cache.clear();
    cached_current=current.frame_id; cached_previous=previous.frame_id;
    pairing_cached=false;
  }
  const auto rigid=[](const std::uint8_t* c,const std::uint8_t* p,bool translation) -> const GekkoAOT::GX::RigidPair& {
    std::uint8_t key_bytes[97];
    std::memcpy(key_bytes,c,48); std::memcpy(key_bytes+48,p,48); key_bytes[96]=translation ? 1u : 0u;
    const auto key=XXH3_64bits(key_bytes,sizeof(key_bytes));
    auto it=rigid_cache.find(key);
    if (it==rigid_cache.end()) it=rigid_cache.emplace(key,GekkoAOT::GX::PrepareRigidPair(c,p,translation)).first;
    return it->second;
  };
  // Pairing (once per frame pair, like the source tags of a decompiled port
  // but from the command stream alone):
  // - the camera motion: the transform most draws share between the frames
  //   (static scenery moves only with the view);
  // - each draw signature's blocks align in drawing order (games traverse
  //   their scene in a stable order), with gaps for draws culled or added; a
  //   pair is allowed only if the current block's origin is within 25% of its
  //   view distance of where the camera motion puts its partner's, so a
  //   mismatch never slides or teleports across the screen;
  // - perspective blocks left unpaired (new, culled, retextured: flipbook
  //   flames) follow the camera alone: their real pose seen from the
  //   interpolated viewpoint, never frozen on screen.
  using GekkoAOT::GX::ReplayAffine;
  static bool camera_known=false;
  static ReplayAffine camera{};
  static std::vector<std::uint8_t> follow_bytes;
  static std::vector<std::int64_t> follow_at;
  const auto own=[](const ReplayCapture& c,const ReplayBlock& b,ReplayAffine& m) {
    if (b.proj+64u+(b.pnmtx+3u)*16u>b.size) return false;
    m=GekkoAOT::GX::LoadReplayAffine(c.bytes.data()+b.data+b.proj+64u+b.pnmtx*16u);
    for (const auto& row : m) for (const double v : row) if (!std::isfinite(v)) return false;
    return true;
  };
  const auto perspective=[](const ReplayCapture& c,const ReplayBlock& b) {
    float w[4];
    std::memcpy(w,c.bytes.data()+b.data+b.proj+48u,16);
    return std::abs(w[2])>0.5f && w[3]==0.f;
  };
  if (!pairing_cached) {
    partner.assign(current.blocks.size(),-1);
    std::vector<ReplayAffine> deltas;
    for (const auto& [sig,group] : current.groups) {
      const auto found=previous.groups.find(sig);
      if (found==previous.groups.end()) continue;
      for (std::size_t k=0;k<group.size() && k<found->second.size() && deltas.size()<256u;++k) {
        const auto& a=current.blocks[group[k]];
        const auto& b=previous.blocks[found->second[k]];
        ReplayAffine ma, mb, inverse;
        if (!perspective(current,a) || !own(current,a,ma) || !own(previous,b,mb) ||
            !GekkoAOT::GX::InvertReplayAffine(mb,inverse)) continue;
        deltas.push_back(GekkoAOT::GX::MulReplayAffine(ma,inverse));
      }
    }
    std::size_t camera_support=0;
    camera_known=GekkoAOT::GX::EstimateReplayCamera(deltas,camera,&camera_support);
    // Each block's origin in view space; each previous block's predicted
    // origin now: moved on by its own last motion, then by the camera
    // (tracking with a constant-velocity model: a particle slot reused by a
    // new particle, or a neighbour in a moving chain, is far from the
    // prediction, the same draw is near it).
    using Point=std::array<double,3>;
    std::vector<Point> here(current.blocks.size()), predicted(previous.blocks.size());
    std::vector<std::uint8_t> here_ok(current.blocks.size()), predicted_ok(previous.blocks.size());
    for (std::size_t i=0;i<current.blocks.size();++i) {
      ReplayAffine m;
      if ((here_ok[i]=own(current,current.blocks[i],m))) here[i]={m[0][3],m[1][3],m[2][3]};
    }
    const bool tracked=previous.motion.size()==previous.blocks.size();
    for (std::size_t j=0;j<previous.blocks.size();++j) {
      ReplayAffine m;
      if (!(predicted_ok[j]=own(previous,previous.blocks[j],m))) continue;
      Point p{m[0][3],m[1][3],m[2][3]};
      if (tracked) for (int c=0;c<3;++c) p[c]+=previous.motion[j][c];
      predicted[j]=camera_known && perspective(previous,previous.blocks[j]) ? GekkoAOT::GX::ApplyReplayAffine(camera,p) : p;
    }
    std::vector<std::int32_t> aligned;
    for (const auto& [sig,group] : current.groups) {
      const auto found=previous.groups.find(sig);
      if (found==previous.groups.end()) continue;
      const auto& prior=found->second;
      const auto distance=[&](std::size_t i,std::size_t j) {
        const auto a=group[i], b=prior[j];
        if (current.blocks[a].size!=previous.blocks[b].size || current.blocks[a].proj!=previous.blocks[b].proj)
          return std::numeric_limits<double>::infinity();
        if (!here_ok[a] || !predicted_ok[b]) return here_ok[a]==predicted_ok[b] ? 0.0 : std::numeric_limits<double>::infinity();
        const auto& pa=here[a]; const auto& pb=predicted[b];
        const double dx=pa[0]-pb[0], dy=pa[1]-pb[1], dz=pa[2]-pb[2];
        const double scale=std::max({std::sqrt(pa[0]*pa[0]+pa[1]*pa[1]+pa[2]*pa[2]),
                                     std::sqrt(pb[0]*pb[0]+pb[1]*pb[1]+pb[2]*pb[2]),1e-6});
        return std::sqrt(dx*dx+dy*dy+dz*dz)/scale;
      };
      // Immediate-mode groups are particles (billboards): a tight gate. With
      // motion known, a gap costs a quarter of the gate (a particle that died
      // and one born are two gaps, not every survivor shifted onto its
      // neighbour); without it drawing order decides.
      std::size_t direct=0;
      for (const auto i : group) direct+=current.blocks[i].direct;
      const double gate=direct*2u>group.size() ? 0.05 : 0.25;
      if (group.size()>256u || prior.size()>256u) {
        // Particle-sized groups: drawing order, each pair still checked.
        for (std::size_t k=0;k<group.size() && k<prior.size();++k)
          if (distance(k,k)<=gate) partner[group[k]]=std::int32_t(prior[k]);
        continue;
      }
      // Still look-alikes (where they were, up to float error) lock; the rest
      // align in drawing order within the gate.
      GekkoAOT::GX::PairReplayBlocks(group.size(),prior.size(),distance,1e-4,gate,aligned,tracked ? 0.25*gate : 1e6);
      for (std::size_t i=0;i<group.size();++i)
        if (aligned[i]>=0) partner[group[i]]=std::int32_t(prior[std::size_t(aligned[i])]);
    }
    // This frame's motion (for the next pair): where each block is now
    // against where its partner was, moved by the camera.
    current.motion.assign(current.blocks.size(),{0.f,0.f,0.f});
    for (std::size_t i=0;i<current.blocks.size();++i) {
      const auto j=partner[i];
      if (j<0 || !here_ok[i] || !predicted_ok[std::size_t(j)]) continue;
      ReplayAffine m;
      own(previous,previous.blocks[std::size_t(j)],m);
      Point p{m[0][3],m[1][3],m[2][3]};
      if (camera_known && perspective(previous,previous.blocks[std::size_t(j)])) p=GekkoAOT::GX::ApplyReplayAffine(camera,p);
      for (int c=0;c<3;++c) current.motion[i][c]=float(here[i][c]-p[c]);
    }
    // GEKKOAOT_REPLAY_TRACE=1: every 60th frame pair, the largest groups
    // (size now/before, paired, immediate-mode positions, producers).
    static const bool trace=EnvBool("GEKKOAOT_REPLAY_TRACE",false);
    static std::uint64_t traced=0;
    if (trace && traced++%60u==0u) {
      std::vector<std::pair<std::size_t,std::uint64_t>> sizes;
      for (const auto& [sig,group] : current.groups) sizes.emplace_back(group.size(),sig);
      std::sort(sizes.rbegin(),sizes.rend());
      std::fprintf(stderr,"GEKKOAOT_REPLAY_TRACE frame=%llu blocks=%zu groups=%zu camera=%u support=%zu/%zu\n",
                   static_cast<unsigned long long>(current.frame_id),current.blocks.size(),sizes.size(),camera_known?1u:0u,
                   camera_support,deltas.size());
      for (std::size_t k=0;k<sizes.size() && k<12u;++k) {
        const auto& group=current.groups.at(sizes[k].second);
        const auto found=previous.groups.find(sizes[k].second);
        std::size_t paired=0, direct=0, producer=0;
        for (const auto i : group) {
          paired+=partner[i]>=0; direct+=current.blocks[i].direct; producer+=current.blocks[i].producer;
        }
        const auto& first=current.blocks[group.front()];
        std::fprintf(stderr,"  sig=%016llx now=%zu before=%zu paired=%zu direct=%zu producer=%zu persp=%u view=%.0f,%.0f,%.0fx%.0f\n",
                     static_cast<unsigned long long>(sizes[k].second),group.size(),
                     found==previous.groups.end() ? 0u : found->second.size(),paired,direct,producer,
                     perspective(current,first)?1u:0u,first.view[0],first.view[1],first.view[2],first.view[3]);
      }
      for (const auto& copy : current.copies)
        std::fprintf(stderr,"  copy blocks=%u rect=%.0f,%.0f,%.0fx%.0f\n",copy.blocks,copy.rect[0],copy.rect[1],copy.rect[2],copy.rect[3]);
    }
    // The camera-only "previous" of unpaired perspective blocks: every
    // position matrix moved back by the camera motion (normals by its
    // rotation).
    follow_bytes.clear();
    follow_at.assign(current.blocks.size(),-1);
    ReplayAffine back;
    if (camera_known && GekkoAOT::GX::InvertReplayAffine(camera,back)) {
      for (std::uint32_t index=0;index<current.blocks.size();++index) {
        const auto& block=current.blocks[index];
        if (partner[index]>=0 || !perspective(current,block)) continue;
        const std::uint32_t rows=std::min(GekkoAOT::GX::ReplayMatrixRows,aurora::gx::MaxXfPosUniformRows);
        const std::uint32_t pos=block.proj+64u, nrm=pos+aurora::gx::MaxXfPosUniformRows*16u;
        if (nrm+rows*16u>block.size) continue;
        follow_at[index]=std::int64_t(follow_bytes.size());
        follow_bytes.insert(follow_bytes.end(),current.bytes.begin()+block.data,current.bytes.begin()+block.data+block.size);
        auto* bytes=follow_bytes.data()+follow_at[index];
        for (std::uint32_t m=0;m+3u<=rows;m+=3u) {
          GekkoAOT::GX::StoreReplayAffine(GekkoAOT::GX::MulReplayAffine(back,GekkoAOT::GX::LoadReplayAffine(bytes+pos+m*16u)),
                                          bytes+pos+m*16u);
          auto normal=GekkoAOT::GX::LoadReplayAffine(bytes+nrm+m*16u);
          for (auto& row : normal) row[3]=0.0;
          auto rotation=back;
          for (auto& row : rotation) row[3]=0.0;
          GekkoAOT::GX::StoreReplayAffine(GekkoAOT::GX::MulReplayAffine(rotation,normal),bytes+nrm+m*16u,false);
        }
      }
    }
  }
  for (std::uint32_t index=0;index<current.blocks.size();++index) {
    const auto& block=current.blocks[index];
    writes.push_back({block.offset,block.size,block.data});
    if (block.producer) { ++producers; continue; }
    if (partner[index]<0) {
      if (follow_at[index]>=0) {
        ++followed;
        GekkoAOT::GX::ExtrapolateUniformBlock(current.bytes.data()+block.data,follow_bytes.data()+follow_at[index],
            block.size,block.proj,block.pnmtx,aurora::gx::MaxXfPosUniformRows,alpha,bytes.data()+block.data,rigid);
      }
      continue;
    }
    const auto& prior=previous.blocks[std::uint32_t(partner[index])];
    if (prior.size!=block.size || prior.proj!=block.proj) continue;
    ++matched;
    const auto motion=GekkoAOT::GX::ExtrapolateUniformBlock(
        current.bytes.data()+block.data,previous.bytes.data()+prior.data,block.size,block.proj,block.pnmtx,
        aurora::gx::MaxXfPosUniformRows,alpha,bytes.data()+block.data,rigid);
    if (motion!=GekkoAOT::GX::ReplayMotion::Unchanged) ++moved;
    if (motion==GekkoAOT::GX::ReplayMotion::Cut) ++cuts;
  }
  auto& stats=g_replay_stats;
  stats.blocks+=current.blocks.size(); stats.matched+=matched; stats.moved+=moved; stats.cuts+=cuts;
  stats.followed+=followed; stats.camera+=camera_known ? 1u : 0u; stats.producers+=producers;
  // Mostly unmatched or mostly cut: a different scene, not the next pose.
  const bool frame_cut=(matched+followed+producers)*2u<current.blocks.size() || (moved>=8u && cuts*2u>moved);
  if (frame_cut) ++stats.frame_cuts;
  // CPU-animated arrays: both frames' bytes of the same array, blended into
  // the retained storage for this replay and restored right after it.
  struct StorageWrite { std::uint32_t offset, size, source; };
  std::vector<std::uint8_t> blended, originals;
  std::vector<StorageWrite> vertex_writes;
  auto& vstats=g_replay_vertex_stats;
  // Vertices only move between the two real poses: extrapolated meshes
  // (prediction, or a late frame) overshoot into spikes.
  const double vertex_t=std::clamp(1.0+double(alpha),0.0,1.0);
  if (!frame_cut && vertex_t<1.0) {
    vstats.arrays+=current.arrays.size();
    for (const auto& array : current.arrays) {
      if (!array.copied) continue;
      ++vstats.animated; vstats.animated_bytes+=array.size;
      const auto it=previous.array_index.find(array.key);
      if (it==previous.array_index.end()) continue;
      const auto& prior=previous.arrays[it->second];
      if (!prior.copied || prior.size!=array.size) continue;
      const std::uint32_t rounded=(array.size+3u)&~3u;
      const std::uint32_t at=static_cast<std::uint32_t>(blended.size());
      blended.resize(at+rounded,0);
      std::memcpy(blended.data()+at,current.vertex_bytes.data()+array.copy,array.size);
      originals.resize(at+rounded,0);
      std::memcpy(originals.data()+at,current.vertex_bytes.data()+array.copy,array.size);
      if (!GekkoAOT::GX::BlendVertexArray(current.vertex_bytes.data()+array.copy,previous.vertex_bytes.data()+prior.copy,
                                         array.size,array.stride,array.comp,array.count,array.little_endian,
                                         vertex_t,blended.data()+at)) {
        ++vstats.refused; blended.resize(at); originals.resize(at); continue;
      }
      ++vstats.blended;
      vertex_writes.push_back({array.storage,rounded,at});
    }
  }
  for (const auto& w : vertex_writes)
    aurora::gfx::detail::gekkoaot_replay_write_storage(current.frame_id,w.offset,blended.data()+w.source,w.size);
  // Queued after the replay's command buffer: the frame's own bytes are back
  // before any later frame can reuse a persistent array snapshot.
  const auto restore_vertices=[&] {
    for (const auto& w : vertex_writes)
      aurora::gfx::detail::gekkoaot_replay_write_storage(current.frame_id,w.offset,originals.data()+w.source,w.size);
  };
  bool presented=false;
  if (!frame_cut && aurora::gfx::detail::gekkoaot_replay_submit(current.frame_id,bytes.data(),writes.data(),writes.size())) {
    restore_vertices();
    DumpReplay(current,1.0+double(alpha));
    PresentFinalTexture(current.present);
    presented=true;
    ++stats.replays; stats.alpha+=alpha;
    static std::uint64_t last_frame=0; static float last_alpha=0;
    g_metric_replays.fetch_add(1,std::memory_order_relaxed);
    if (current.frame_id!=last_frame || std::abs(alpha-last_alpha)>1e-3f)
      g_metric_unique_replays.fetch_add(1,std::memory_order_relaxed);
    last_frame=current.frame_id; last_alpha=alpha;
  } else {
    restore_vertices();
    ++stats.skipped;
    // PresentReplay held the real frame back for this replay: without a replay
    // nothing else shows it, so it goes out once as is and the display never
    // freezes on frames that cannot be replayed (cuts, frames not retained).
    g_replay_refused_frame.store(current.frame_id,std::memory_order_relaxed);
    static std::uint64_t last_direct=0;
    if (g_replay_interpolate && last_direct!=current.frame_id) {
      last_direct=current.frame_id;
      PresentFinalTexture(current.present);
    }
    static unsigned refused_logs=0;
    if (refused_logs++<8u || stats.skipped%600u==0u)
      std::fprintf(stderr,"GEKKOAOT_FRAME_REPLAY_REFUSED_V393 frame_cut=%u blocks=%zu matched=%llu followed=%llu producers=%llu moved=%llu cuts=%llu\n",
                   frame_cut ? 1u : 0u,current.blocks.size(),
                   static_cast<unsigned long long>(matched),static_cast<unsigned long long>(followed),
                   static_cast<unsigned long long>(producers),static_cast<unsigned long long>(moved),
                   static_cast<unsigned long long>(cuts));
  }
  const auto total=stats.replays+stats.skipped;
  if (presented && (total==30u || total%600u==0u))
    std::fprintf(stderr,"GEKKOAOT_FRAME_REPLAY_V358 replays=%llu skipped=%llu frame_cuts=%llu blocks/frame=%.0f matched=%.1f%% moved=%.1f%% cuts=%.1f%% alpha=%.2f followed=%.1f%% camera=%.0f%% producers=%.1f%%\n",
                 static_cast<unsigned long long>(stats.replays),static_cast<unsigned long long>(stats.skipped),
                 static_cast<unsigned long long>(stats.frame_cuts),double(stats.blocks)/double(total),
                 stats.blocks ? 100.0*double(stats.matched)/double(stats.blocks) : 0.0,
                 stats.matched ? 100.0*double(stats.moved)/double(stats.matched) : 0.0,
                 stats.moved ? 100.0*double(stats.cuts)/double(stats.moved) : 0.0,
                 stats.replays ? stats.alpha/double(stats.replays) : 0.0,
                 stats.blocks ? 100.0*double(stats.followed)/double(stats.blocks) : 0.0,
                 total ? 100.0*double(stats.camera)/double(total) : 0.0,
                 stats.blocks ? 100.0*double(stats.producers)/double(stats.blocks) : 0.0);
  if (presented && (total==30u || total%600u==0u))
    std::fprintf(stderr,"GEKKOAOT_FRAME_REPLAY_VERTEX_V368 arrays=%llu animated=%llu animated_kib=%llu blended=%llu refused=%llu\n",
                 static_cast<unsigned long long>(vstats.arrays),static_cast<unsigned long long>(vstats.animated),
                 static_cast<unsigned long long>(vstats.animated_bytes>>10),static_cast<unsigned long long>(vstats.blended),
                 static_cast<unsigned long long>(vstats.refused));
}

// Host tick: polls GPU completion and decides between replays and direct
// presents. True while real frames go out directly.
bool ReplayDirectTick(std::int64_t now_ns) {
  const auto last_tick=g_replay_last_tick_ns.exchange(now_ns,std::memory_order_relaxed);
  if (last_tick!=0 && now_ns-last_tick<250000000) g_replay_tick_ns.store(now_ns-last_tick,std::memory_order_relaxed);
  // Dawn is only safe from the render worker: completion is polled there.
  if (g_replay_gpu_start_ns.load(std::memory_order_acquire)!=0 && !g_replay_poll_waiting.exchange(true,std::memory_order_acq_rel))
    aurora::gfx::render_worker::enqueue_work([] {
      g_replay_poll_waiting.store(false,std::memory_order_release);
      aurora::webgpu::g_instance.ProcessEvents();
    });
  bool direct=g_replay_direct.load(std::memory_order_relaxed);
  const double period=1e-9*double(g_replay_period_ns.load(std::memory_order_relaxed));
  if (period<=0.0) return direct;
  // A replay needs the time of its frame and of itself inside one guest period.
  const double needed=1e-9*double(g_replay_frame_cost_ns.load(std::memory_order_relaxed))+
                      1.2e-9*double(g_replay_cost_ns.load(std::memory_order_relaxed));
  static std::int64_t resume_after=0, resumed_at=0, hold=1000000000;
  if (!direct && needed>period) {
    direct=true;
    // Capture changes the cost, so a scene right at the limit could flip every
    // second: each quick failure doubles the wait before the next attempt.
    hold=now_ns-resumed_at>30000000000ll ? 1000000000 : std::min<std::int64_t>(hold*2,32000000000ll);
    resume_after=now_ns+hold;
  } else if (direct && needed<0.85*period && now_ns>=resume_after) {
    direct=false;
    resumed_at=now_ns;
  }
  g_replay_direct.store(direct,std::memory_order_relaxed);
  return direct;
}

bool ReplayReady() {
  if (g_replay_direct.load(std::memory_order_relaxed)) return true;
  std::lock_guard lock{g_replay_mutex};
  return g_replay_current && g_replay_previous && g_replay_current->eligible;
}

bool PresentReplay() {
  std::shared_ptr<const ReplayCapture> current, previous;
  const auto now=std::chrono::steady_clock::now();
  const auto now_ns=std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
  static const bool paced=[] {
    const char* setting=std::getenv("GEKKOAOT_REPLAY_DEADLINE");
    return !setting || setting[0]!='0';
  }();
  if (paced && ReplayDirectTick(now_ns)) {
    g_replay_requests.fetch_add(1,std::memory_order_relaxed);
    g_replay_direct_ticks.fetch_add(1,std::memory_order_relaxed);
    return false;
  }
  // Blend position from the smoothed game timeline (ReplayClock), not from
  // the frames' jittery finish times: 0 = previous frame, 1 = newest.
  double position=0;
  bool positioned=false;
  double nominal=0;
  {
    std::lock_guard lock{g_replay_mutex};
    current=g_replay_current; previous=g_replay_previous;
    nominal=g_replay_clock.Nominal();
    if (current && previous && current->eligible)
      positioned=g_replay_clock.Position(ReplaySeconds(now),g_replay_interpolate,position);
  }
  g_replay_requests.fetch_add(1,std::memory_order_relaxed);
  if (!current || !previous || !current->eligible) return false;
  if (!positioned || !(nominal>0.004 && nominal<0.25)) {
    g_replay_declined_period.fetch_add(1,std::memory_order_relaxed);
    return false;
  }
  // Uniform blocks move as current + alpha * (current - previous).
  double alpha=position-1.0;
  // Interpolation shows the previous pose when the newest frame arrives, so
  // a direct present of that frame would jump ahead and back: while replays
  // run, a declined tick still counts as presented.
  const bool hold=g_replay_interpolate;
  if (hold && current->frame_id==g_replay_refused_frame.load(std::memory_order_relaxed)) return true;
  if (!g_replay_interpolate && alpha<0.05) {
    g_replay_declined_early.fetch_add(1,std::memory_order_relaxed);
    return false;
  }
  // With VSync, at most one present per display refresh (more are never
  // seen). Without it the output frame limit alone paces presents.
  if (g_vsync_default &&
      double(now_ns-g_last_present_ns.load(std::memory_order_relaxed))<0.9e9/DisplayRefreshHz()) {
    g_replay_declined_period.fetch_add(1,std::memory_order_relaxed);
    return hold;
  }
  if (paced) {
    const auto started=g_replay_gpu_start_ns.load(std::memory_order_acquire);
    const double cost=1e-9*double(g_replay_cost_ns.load(std::memory_order_relaxed));
    const double since=ReplaySeconds(now)-g_replay_last_frame_s.load(std::memory_order_relaxed);
    if (since>=0.0 && since+1.2*cost>nominal) {
      g_replay_declined_deadline.fetch_add(1,std::memory_order_relaxed);
      if (!hold) return false;
      // No time left for a replay: this frame still goes out, right before the
      // next one starts its transition from this very pose.
      if (g_replay_shown_frame.exchange(current->frame_id,std::memory_order_relaxed)!=current->frame_id)
        aurora::gfx::render_worker::enqueue_work([current=std::move(current)] {
          PresentFinalTexture(current->present);
        });
      return true;
    }
    if (started!=0 && now_ns-started<50000000) {
      g_replay_declined_busy.fetch_add(1,std::memory_order_relaxed);
      return hold;
    }
  }
  if (g_replay_waiting.exchange(true,std::memory_order_acq_rel)) {
    g_replay_declined_busy.fetch_add(1,std::memory_order_relaxed);
    return hold;
  }
  g_replay_queued.fetch_add(1,std::memory_order_relaxed);
  g_replay_shown_frame.store(current->frame_id,std::memory_order_relaxed);
  // The pose is taken when the worker reaches the job (after the real frame
  // or the previous replay), so what is shown matches when it is shown.
  aurora::gfx::render_worker::enqueue_work([current=std::move(current),previous=std::move(previous),alpha] {
    g_replay_waiting.store(false,std::memory_order_release);
    std::shared_ptr<const ReplayCapture> newest, prior;
    double position=0;
    bool positioned=false;
    {
      std::lock_guard lock{g_replay_mutex};
      newest=g_replay_current; prior=g_replay_previous;
      if (newest && prior && newest->eligible)
        positioned=g_replay_clock.Position(ReplaySeconds(std::chrono::steady_clock::now()),g_replay_interpolate,position);
    }
    const std::int64_t start=ReplayClockNs();
    g_replay_gpu_start_ns.store(start,std::memory_order_release);
    // A guest frame that arrived meanwhile is the fresher pair.
    if (positioned && (g_replay_interpolate || position-1.0>=0.05))
      ReplayFrame(*newest,*prior,static_cast<float>(position-1.0));
    else
      ReplayFrame(*current,*previous,static_cast<float>(alpha));
    TimeSubmittedWork(start,true);
  });
  return true;
}

// Live runtime values for the performance HUD (Hud::RuntimeValue order).
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
  const auto& view=aurora::gx::g_gxState.logicalViewport;
  capture.blocks.push_back({key,offset,size,proj,static_cast<std::uint32_t>(capture.bytes.size()),
                            std::min<std::uint32_t>(g_replay_draw_pnmtx,aurora::gx::MaxXfPosRows-3u),
                            g_replay_draw_sig,{view.left,view.top,view.width,view.height},
                            aurora::gx::g_gxState.vtxDesc[GX_VA_POS]==GX_DIRECT});
  capture.bytes.insert(capture.bytes.end(),data,data+size);
}

// Aurora's push_gx_draw: an indexed position/normal array a draw reads from
// storage `offset` (the shader base: a compact window's staged start minus its
// prefix). Keyed by guest address, size, stride and format.
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_replay_array(std::uint32_t attr, const std::uint8_t* data,
                                                       std::uint32_t offset, std::uint32_t size,
                                                       std::uint32_t stride, std::uint32_t comp,
                                                       std::uint32_t count, bool little_endian) noexcept {
  if (!data || size==0u || attr>=GX_VA_MAX_ATTR || size>(1u<<20)) return;
  const std::uint32_t prefix=(g_replay_compact_mask>>attr)&1u ? g_replay_compact_prefix[attr] : 0u;
  const std::uint32_t staged=offset+prefix;
  if (!g_replay_array_seen.insert((std::uint64_t(attr)<<32)|staged).second) return;
  data+=prefix;
  const std::uint32_t guest=g_array_guest_base[attr]+prefix;
  std::uint64_t base=1469598103934665603ull;
  for (const std::uint64_t v : {std::uint64_t(attr),std::uint64_t(guest),std::uint64_t(size),std::uint64_t(stride),
                                std::uint64_t(comp),std::uint64_t(count)})
    base=(base^v)*1099511628211ull;
  auto& capture=g_replay_building;
  const std::uint64_t hash=XXH3_64bits(data,size);
  // A guest buffer the game refills between draws (one scratch buffer for
  // several objects) is one array per content: its n-th content pairs with its
  // n-th content in the previous frame. The same content staged again (another
  // draw) is the same array in another storage range.
  std::uint64_t key=base;
  for (std::uint64_t occurrence=1;;++occurrence) {
    const auto seen=capture.array_index.find(key);
    if (seen==capture.array_index.end()) break;
    if (capture.arrays[seen->second].hash==hash) {
      ReplayArray again=capture.arrays[seen->second];
      again.storage=staged;
      capture.arrays.push_back(again);
      return;
    }
    key=(base^occurrence)*1099511628211ull;
  }
  ReplayArray array{key,hash,staged,size,stride,0u,std::uint8_t(comp),std::uint8_t(count),little_endian,false};
  capture.array_index.emplace(key,static_cast<std::uint32_t>(capture.arrays.size()));
  const auto last=g_replay_array_last.find(key);
  if (last!=g_replay_array_last.end() && last->second!=array.hash && capture.vertex_bytes.size()+size<=(32u<<20)) {
    array.copied=true;
    array.copy=static_cast<std::uint32_t>(capture.vertex_bytes.size());
    capture.vertex_bytes.insert(capture.vertex_bytes.end(),data,data+size);
  }
  capture.arrays.push_back(array);
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
#if defined(__GNUC__) && !defined(__clang__)
// Present only in profile-instrumented (-fprofile-generate) renderer builds.
extern "C" void __gcov_dump(void) __attribute__((weak));
#endif
GEKKOAOT_GX_EXPORT void gekkoaot_native_gx_shutdown(){
#if defined(__GNUC__) && !defined(__clang__)
  // The runner may leave through a fast exit that skips the destructors
  // libgcov writes its counters from.
  if(__gcov_dump) __gcov_dump();
#endif
  if(!g_ready) return;
  if (g_native_ir_profile) {
    const auto& s = g_native_ir_stats;
    const auto total_units = s.decoder_units + s.sdk_command_units;
    const double sdk_percent = total_units ? 100.0 * double(s.sdk_command_units) / double(total_units) : 0.0;
    const double ir_percent = total_units ? 100.0 * double(s.sdk_command_units + s.fifo) / double(total_units) : 0.0;
    std::fprintf(stderr,
        "GEKKOAOT_NATIVE_GX_IR_STATS sdk_ops=%llu fifo_ops=%llu arrays=%llu matrices=%llu unchanged_matrices=%llu "
        "sdk_sink_ns=%llu fifo_sink_ns=%llu decoder_units=%llu aurora_calls=%llu aurora_ns=%llu "
        "compatibility_transitions=%llu graph_copies=%llu fifo_ir=%u sdk_command_units=%llu "
        "sdk_bypass_pct=%.3f fifo_decode_pct=%.3f state_ir_pct=%.3f\n",
        static_cast<unsigned long long>(s.sdk), static_cast<unsigned long long>(s.fifo),
        static_cast<unsigned long long>(s.arrays), static_cast<unsigned long long>(s.matrices),
        static_cast<unsigned long long>(s.unchanged_matrices), static_cast<unsigned long long>(s.sdk_ns),
        static_cast<unsigned long long>(s.fifo_ns), static_cast<unsigned long long>(s.decoder_units),
        static_cast<unsigned long long>(s.compatibility), static_cast<unsigned long long>(s.compatibility_ns),
        static_cast<unsigned long long>(s.compatibility_transitions), static_cast<unsigned long long>(s.graph_copies),
        g_fifo_native_ir ? 1u : 0u, static_cast<unsigned long long>(s.sdk_command_units),
        sdk_percent, total_units ? 100.0 - sdk_percent : 0.0, ir_percent);
    const auto& dl=g_native_display_lists.stats;
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_EXTENDED_STATS vertex=%llu projection=%llu viewport=%llu transform=%llu pixel=%llu "
        "dl_hits=%llu dl_misses=%llu dl_changed=%llu dl_negative_hits=%llu dl_declined=%llu dl_operations=%llu "
        "dl_operations_executed=%llu dl_commands_reused=%llu\n",
        static_cast<unsigned long long>(s.vertex_states),static_cast<unsigned long long>(s.projections),
        static_cast<unsigned long long>(s.viewports),static_cast<unsigned long long>(s.transform_states),
        static_cast<unsigned long long>(s.pixel_states),static_cast<unsigned long long>(dl.hits),
        static_cast<unsigned long long>(dl.misses),static_cast<unsigned long long>(dl.changed),
        static_cast<unsigned long long>(dl.negative_hits),static_cast<unsigned long long>(dl.declined),
        static_cast<unsigned long long>(dl.operations),static_cast<unsigned long long>(s.dl_operations_executed),
        static_cast<unsigned long long>(s.dl_commands_reused));
    const auto& draws=g_native_draw_lists.stats;
    std::fprintf(stderr,"GEKKOAOT_NATIVE_DRAW_IR_STATS hits=%llu misses=%llu changed=%llu declined=%llu compiled_draws=%llu reused_draws=%llu executed_draws=%llu\n",
      static_cast<unsigned long long>(draws.hits),static_cast<unsigned long long>(draws.misses),
      static_cast<unsigned long long>(draws.changed),static_cast<unsigned long long>(draws.declined),
      static_cast<unsigned long long>(draws.draws),static_cast<unsigned long long>(draws.reusedDraws),
      static_cast<unsigned long long>(g_native_draw_list_executed));
    std::fprintf(stderr,"GEKKOAOT_NATIVE_LIST_GOVERNOR_V387 state_bypassed=%llu state_episodes=%llu draw_bypassed=%llu draw_episodes=%llu\n",
      static_cast<unsigned long long>(g_native_display_lists.governor.bypassed),
      static_cast<unsigned long long>(g_native_display_lists.governor.episodes),
      static_cast<unsigned long long>(g_native_draw_lists.governor.bypassed),
      static_cast<unsigned long long>(g_native_draw_lists.governor.episodes));
    const auto& d = g_scene_evidence.draws;
    std::fprintf(stderr, "GEKKOAOT_SCENE_EVIDENCE unknown=%llu opaque3d_candidate=%llu transparent_candidate=%llu "
        "interface_candidate=%llu intermediate=%llu csm_supported=0\n",
        static_cast<unsigned long long>(d[0]), static_cast<unsigned long long>(d[1]),
        static_cast<unsigned long long>(d[2]), static_cast<unsigned long long>(d[3]),
        static_cast<unsigned long long>(d[4]));
  }
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
  aurora::gfx::enhanced::report();
  aurora::gfx::enhanced::reset();
  aurora::texture::unregister_replacements(g_texture_pack);
  g_texture_pack = {};
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
               "GEKKOAOT_GX_COHERENCY_FAST_V1 notifications=%llu unchanged=%llu texture_span_hits=%llu texture_span_misses=%llu\n",
               static_cast<unsigned long long>(g_guest_memory_write_notifications),
               static_cast<unsigned long long>(g_guest_memory_write_noops),
               static_cast<unsigned long long>(g_texture_write_span_hits),
               static_cast<unsigned long long>(g_texture_write_span_misses));
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
  g_texture_write_footprints.fill({});
  g_texture_write_span_hits=g_texture_write_span_misses=0;
  g_guest_memory_write_notifications=g_guest_memory_write_noops=0;
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
  {std::lock_guard lock(g_camera_projection_mutex);g_camera_projection={};g_camera_producer_projection={};g_camera_candidates={};g_camera_candidates_producer={};}
  g_camera_memo={};
  g_camera_observed_copy.store(0,std::memory_order_relaxed);
  g_camera_copy_epoch.store(0,std::memory_order_relaxed);
  g_camera_vi_epoch=0;g_scene_camera_selector.Reset();
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
