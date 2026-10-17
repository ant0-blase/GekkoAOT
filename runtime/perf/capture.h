// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace GekkoAOT::Capture {
// This ABI is also used by the optional renderer plugin. Events describe real
// boundaries, never inferred GPU completion or physical display scanout.
enum class Kind : std::uint32_t {
  AotMain, AotSecondary, Sdk, NativeOs, IdleWait, ClockPark, PollPark,
  GxQueueFull, GxQueueEmpty, GxParser, GxDrain, RenderQueueFull,
  RenderQueueEmpty, RenderWork, RenderFence, SurfaceAcquire, SurfacePresent,
  PipelineCompile, PipelineWait, PipelineThrottle, DiscRead, DiscWait, DspWork,
  GuestFrameObserved, PresentRequest, PeCommandParsed, PeGuestDelivery,
  ViFieldObserved, XfbPublish, FrameBegin, FrameEnd, GpuSubmit,
  GpuStagingReady, ParserQueueAge, RenderQueueAge, WsiSuccess, XfbSelected, ScanoutDuplicate, GpuComplete, Count
};
inline constexpr const char* Names[] = {
  "aot_main_inclusive", "aot_secondary_inclusive", "sdk_inclusive", "native_os_inclusive",
  "idle_wait", "clock_park_inclusive", "poll_park_inclusive", "gx_queue_full",
  "gx_queue_empty", "gx_parser", "gx_drain", "render_queue_full", "render_queue_empty",
  "render_work_inclusive", "render_fence", "surface_acquire", "surface_present",
  "pipeline_compile", "pipeline_wait", "pipeline_throttle", "disc_read", "disc_wait",
  "dsp_work", "guest_frame_observed", "present_request", "pe_command_parsed",
  "pe_guest_delivery", "vi_field_observed", "xfb_publish", "frame_begin", "frame_end",
  "gpu_submit", "gpu_staging_ready", "parser_queue_age", "render_queue_age", "wsi_success", "xfb_selected", "scanout_duplicate", "gpu_complete"
};
static_assert(sizeof(Names)/sizeof(*Names) == static_cast<unsigned>(Kind::Count));
using EnabledFn = bool (*)(void*);
using RecordFn = void (*)(void*, Kind, std::uint64_t, std::uint64_t,
                         std::uint64_t, std::uint64_t);
struct Sink {
  void* user = nullptr;
  EnabledFn enabled = nullptr;
  RecordFn record = nullptr;
  std::uint32_t abi = 1;
  std::uint32_t kind_count = static_cast<std::uint32_t>(Kind::Count);
};

inline bool Compatible(const Sink* sink, std::uint32_t bytes) {
  return sink && bytes == sizeof(Sink) && sink->abi == 1u &&
         sink->kind_count == static_cast<std::uint32_t>(Kind::Count);
}

// One sink per linkage image. Configure only before workers start; the owner
// keeps it alive until all producers stop. No lock/clock/allocation when off.
#if defined(__GNUC__)
__attribute__((visibility("hidden")))
#endif
inline Sink& LocalSink() { static Sink sink; return sink; }
inline void SetSink(Sink sink) { LocalSink() = sink; }
inline bool Enabled() { const auto& s=LocalSink(); return s.enabled && s.enabled(s.user); }
inline std::uint64_t Now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void Record(Kind kind, std::uint64_t start, std::uint64_t end,
                   std::uint64_t id=0, std::uint64_t value=0) {
  const auto& s=LocalSink();
  if(s.record) s.record(s.user,kind,start,end,id,value);
}
inline void Mark(Kind kind, std::uint64_t id=0, std::uint64_t value=0) {
  if(Enabled()) { const auto now=Now(); Record(kind,now,now,id,value); }
}
// Context is carried through render queue items, not read from parser-owned
// frame state on the render thread. A VI replay may reuse the same frame id.
inline thread_local std::uint64_t frame_id=0;
class FrameContext {
public:
  explicit FrameContext(std::uint64_t id): previous_(frame_id) { frame_id=id; }
  ~FrameContext() { frame_id=previous_; }
private: std::uint64_t previous_;
};
class Scope {
public:
  explicit Scope(Kind kind, std::uint64_t id=frame_id, std::uint64_t value=0)
      : kind_(kind), id_(id), value_(value), start_(Enabled()?Now():0) {}
  ~Scope() { if(start_) Record(kind_,start_,Now(),id_,value_); }
  Scope(const Scope&)=delete;
private: Kind kind_; std::uint64_t id_,value_,start_;
};
template<class Fn> decltype(auto) Invoke(Kind kind,std::uint64_t id,Fn&& fn) {
  Scope scope(kind,id); return fn();
}
} // namespace GekkoAOT::Capture
