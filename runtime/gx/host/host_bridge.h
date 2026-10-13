#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include "native/address_space.h"
#include "gx/pe_event_queue.h"
#include "gx/native_command_stream.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace GekkoAOT::GX {

// Standalone-host side of the Native GX plugin ABI.  This class deliberately
// keeps renderer implementation headers out of its public contract: native-run
// only dlopens the renderer plugin and hands it guest-memory/WGPIPE traffic.
class HostBridge final {
public:
  HostBridge() = default;
  ~HostBridge();
  HostBridge(const HostBridge&) = delete;
  HostBridge& operator=(const HostBridge&) = delete;

  bool Open(const std::filesystem::path& path, Native::AddressSpace& memory);
  void Close();

  bool Ready() const { return ready_; }
  const std::string& LastError() const { return last_error_; }

  static constexpr bool IsWriteGatherPipe(std::uint32_t address) {
    // Fixed Flipper WGPIPE aperture. This predicate is on every GX store.
    return (address & 0x3fffffe0u) == 0x0c008000u;
  }
  bool Write(std::uint64_t value, std::uint8_t size, std::uint32_t guest_pc);
  // Optional bulk WGPIPE ABI. Newer NativeGX plugins consume one already
  // ordered byte span instead of crossing the DSO boundary once per guest
  // store. Older plugins transparently fall back to byte writes.
  bool WriteBurst(const std::uint8_t* bytes, std::uint32_t size, std::uint32_t guest_pc);
  // Optional typed SDK frontend. Parameters are consumed at the matching
  // transport position; FIFO capture and guest hardware accounting stay exact.
  bool HasTypedArrayCommands() const {return typed_arrays_ && sdk_array_ != nullptr;}
  bool StageArrayBinding(std::uint32_t index,std::uint32_t base,std::uint32_t stride,
                         std::uint32_t preceding_gather_bytes);
  bool HasDirectMatrixCommands() const {return direct_matrices_ && sdk_commands_!=nullptr;}
  bool StageMatrix(std::uint32_t address,std::uint32_t words,
                   std::uint32_t preceding_gather_bytes);
  // v36: the CPU/AOT thread may hand complete CP bursts to a dedicated GX
  // parser worker.  Guest-visible ordering points still call FlushWrites(),
  // which drains the queue before CP/PE/PI state is observed.  The queue is
  // bounded so a renderer that falls behind naturally applies backpressure.
  void SetAsyncWritesEnabled(bool enabled);
  bool AsyncWritesEnabled() const { return async_writes_enabled_; }
  // v161 host-side WGPIPE coalescing. Complete 32-byte gather lines are merged
  // before crossing the NativeGX DSO boundary. Flush at guest-visible ordering
  // boundaries (PE/CP reads, cache publication, presentation).
  enum class SyncReason : std::uint8_t {
    Other, FrameReady, Present, MemoryWrite, CacheControl, MmioRead, MmioWrite, Efb, Count
  };
  bool FlushWrites(SyncReason reason = SyncReason::Other);
  // v300: GEKKOAOT_RUNTIME_PROFILE diagnostics. Prints how often the guest CPU
  // had to wait for the GX worker, and for how long, per ordering reason.
  void ReportSyncProfile(double seconds);
  // Frame interpolation reads parser-owned history after a produced frame, so
  // it keeps the v43 fence; without it the frame-ready mailbox is self-contained.
  void SetFrameReadyFence(bool enabled) { frame_ready_fence_ = enabled; }
  // v300: publish the producer's partial WGPIPE batch to the GX worker without
  // waiting for it. Keeps command latency bounded when nothing else flushes.
  bool PublishPendingWrites() { return write_batch_size_ == 0u || FlushBufferedWrites(); }
  // v300: idle guest support. Returns once the GX worker has published a PE
  // event, drained its queue, failed, or `until` has passed. Returns false
  // without waiting when the worker is idle (nothing asynchronous can arrive).
  bool WaitForWorkerEvent(std::chrono::steady_clock::time_point until);
  bool WritesPending() const {
    return write_batch_size_ != 0u || async_pending_bursts_.load(std::memory_order_acquire) != 0u;
  }
  // Optional v141 dynamic-vertex coherency ABI. Guest D-cache publication
  // invalidates overlapping Aurora indexed-array snapshots.
  void NotifyCacheControl(std::uint8_t operation, std::uint32_t address);
  // Optional v159 host-DMA coherency ABI. Native devices that publish bytes
  // directly into guest RAM notify Aurora so only overlapping cached arrays or
  // textures are invalidated instead of globally rehashing renderer state.
  void NotifyMemoryWrite(std::uint32_t address, std::uint32_t size);
  // Optional v162 CPU-visible EFB depth aperture. This mirrors Aurora's
  // GXPeekZ contract: the first read may return 0 while scheduling the first
  // asynchronous depth snapshot; subsequent reads use the newest completed one.
  bool PeekEfbZ(std::uint16_t x, std::uint16_t y, std::uint32_t* z);
  // Optional v163 CPU-visible EFB color aperture for GXPeekARGB. Same
  // asynchronous snapshot contract as PeekEfbZ.
  bool PeekEfbArgb(std::uint16_t x, std::uint16_t y, std::uint32_t* argb);
  // Ordered native GPU result for PE's four inclusive raster bounds.
  bool ReadBoundingBox(std::array<std::uint16_t, 4>* bounds);
  // CPU EFB pokes enter the same ordered Aurora command stream as GX FIFO.
  bool PokeEfb(std::uint16_t x, std::uint16_t y, std::uint32_t value, bool depth);
  void AdvanceCycles(std::uint64_t cycles);
  void PresentNow(std::uint32_t xfb_top, std::uint32_t xfb_bottom,
                  std::uint32_t width = 0u, std::uint32_t stride_bytes = 0u,
                  std::uint32_t field_height = 0u);
  // Optional v43 ABI: AuroraGX reports the exact XFB address latched by the
  // latest retail GXCopyDisp. This lets the host present a completed render
  // without making VI retrace itself the host presentation clock.
  bool HasFrameReadySignal() const { return consume_frame_ready_ != nullptr; }
  bool ConsumeFrameReady(std::uint32_t* xfb_address);
  // v48 optional ABI: the renderer retains the previous/current completed GX
  // frames and can synthesize an intermediate host presentation without
  // advancing any guest-visible clock.
  bool HasFrameInterpolation() const {
    return present_interpolated_ != nullptr && interpolation_ready_ != nullptr;
  }
  bool FrameInterpolationReady() const;
  bool PresentInterpolated(float alpha);
  // v128b optional ABI: 4:3 / live window-aspect changes without restarting.
  void SetAspectMode(const std::string& mode);
  bool WidescreenActive() const { return widescreen_active_; }
  float WidescreenTargetAspect() const;
  bool CurrentPerspectiveCamera(float* fov_y_degrees, float* guest_aspect) const;
  void SetCyclePresentEnabled(bool enabled) { cycle_present_enabled_ = enabled; }
  bool QuitRequested() const { return quit_requested_; }

  // The renderer callback runs synchronously on the guest CPU thread while a
  // FIFO command is consumed. Give the native host a direct event sink so PE
  // token/finish state is updated exactly once at the source instead of being
  // polled after every WGPIPE store.
  using PeEventSinkFn = void (*)(void* user, std::uint32_t reg, std::uint32_t value);
  void SetPeEventSink(PeEventSinkFn sink, void* user) {
    pe_event_sink_ = sink;
    pe_event_sink_user_ = user;
  }

  bool FinishSeen() const { return pe_finish_seen_.load(std::memory_order_relaxed); }
  bool ConsumeFinishSeen() { return pe_finish_seen_.exchange(false, std::memory_order_acq_rel); }
  std::uint16_t LastToken() const { return pe_token_.load(std::memory_order_relaxed); }
  bool ConsumeTokenSeen() {
    return pe_token_seen_.exchange(false, std::memory_order_acq_rel);
  }
  bool TokenInterruptSeen() const {
    return pe_token_interrupt_seen_.load(std::memory_order_relaxed);
  }
  bool ConsumePeEvent(PeEvent* event) {return pe_events_.Pop(event);}
  bool PeEventOverflow() const {return pe_event_overflow_.load(std::memory_order_acquire);}
  bool PeEventPending() const {
    if(pe_events_.Pending()) return true;
    return pe_finish_seen_.load(std::memory_order_relaxed) ||
           pe_token_seen_.load(std::memory_order_relaxed) ||
           pe_token_interrupt_seen_.load(std::memory_order_relaxed);
  }
  bool ConsumeTokenInterruptSeen() {
    return pe_token_interrupt_seen_.exchange(false, std::memory_order_acq_rel);
  }

private:
  using ResolveFn = bool (*)(void* user, std::uint32_t address, std::uint32_t size,
                             std::uint32_t space, std::uint32_t resource,
                             const void** data, std::uint32_t* available);
  using PeEventFn = void (*)(void* user, std::uint32_t reg, std::uint32_t value);
  using InitFn = bool (*)(ResolveFn resolve, void* user);
  using WriteFn = void (*)(std::uint64_t value, std::uint8_t size, std::uint32_t guest_pc);
  using WriteBurstFn = void (*)(const std::uint8_t* bytes, std::uint32_t size,
                                std::uint32_t guest_pc);
  using SdkArrayFn = void (*)(std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t);
  using CommandBurstFn = void (*)(const std::uint8_t*,std::uint32_t,std::uint32_t,const void*,std::uint32_t);
  using CacheControlFn = void (*)(std::uint8_t operation, std::uint32_t address);
  using MemoryWriteFn = void (*)(std::uint32_t address, std::uint32_t size);
  using PeekEfbZFn = bool (*)(std::uint16_t x, std::uint16_t y, std::uint32_t* z);
  using PeekEfbArgbFn = bool (*)(std::uint16_t x, std::uint16_t y, std::uint32_t* argb);
  using ReadBoundingBoxFn = bool (*)(std::uint16_t* bounds);
  using PokeEfbFn = bool (*)(std::uint16_t x, std::uint16_t y, std::uint32_t value, bool depth);
  using PresentFn = void (*)();
  using PresentXfbFn = void (*)(std::uint32_t top, std::uint32_t bottom);
  using PresentXfbExFn = void (*)(std::uint32_t top, std::uint32_t bottom,
                                  std::uint32_t width, std::uint32_t stride_bytes,
                                  std::uint32_t field_height);
  using ConsumeFrameReadyFn = bool (*)(std::uint32_t* xfb_address);
  using PresentInterpolatedFn = bool (*)(float alpha);
  using InterpolationReadyFn = bool (*)();
  using SetAspectModeFn = void (*)(std::uint32_t mode);
  using TargetAspectFn = float (*)();
  using CameraSignatureFn = bool (*)(float*, float*);
  using ShouldQuitFn = bool (*)();
  using ShutdownFn = void (*)();
  using SetPeCallbackFn = void (*)(PeEventFn callback, void* user);

  static bool ResolveGuest(void* user, std::uint32_t address, std::uint32_t size,
                           std::uint32_t space, std::uint32_t resource,
                           const void** data, std::uint32_t* available);
  static void OnPeEvent(void* user, std::uint32_t reg, std::uint32_t value);

  void* FindSymbol(const char* name);
  std::uint32_t ContiguousAvailable(std::uint32_t address) const;
  bool SubmitBurstImmediate(const std::uint8_t* bytes, std::uint32_t size,
                            std::uint32_t guest_pc);
  bool EnqueueBurst(const std::uint8_t* bytes, std::uint32_t size,
                    std::uint32_t guest_pc);
  bool FlushBufferedWrites();
  enum class AsyncKind : std::uint8_t { Burst, MemoryWrites, PresentXfb };
  bool PublishCommandLocked(std::unique_lock<std::mutex>& lock);
  bool EnqueueMemoryWrite(std::uint32_t address, std::uint32_t size);
  bool EnqueuePresentXfb(std::uint32_t top, std::uint32_t bottom, std::uint32_t width,
                         std::uint32_t stride_bytes, std::uint32_t field_height);
  bool ExecuteAsyncItem(std::size_t index);
  void AsyncWorkerMain();
  void StopAsyncWorker();
  void Present();

  void* library_ = nullptr;
  Native::AddressSpace* memory_ = nullptr;
  InitFn init_ = nullptr;
  WriteFn write_ = nullptr;
  WriteBurstFn write_burst_ = nullptr;
  SdkArrayFn sdk_array_ = nullptr;
  CommandBurstFn command_burst_ = nullptr;
  CommandBurstFn sdk_commands_ = nullptr;
  bool typed_arrays_ = false;
  bool direct_matrices_ = false;
  CacheControlFn cache_control_ = nullptr;
  MemoryWriteFn memory_write_ = nullptr;
  PeekEfbZFn peek_efb_z_ = nullptr;
  PeekEfbArgbFn peek_efb_argb_ = nullptr;
  ReadBoundingBoxFn read_bbox_ = nullptr;
  PokeEfbFn poke_efb_ = nullptr;
  PresentFn present_ = nullptr;
  PresentXfbFn present_xfb_ = nullptr;
  PresentXfbExFn present_xfb_ex_ = nullptr;
  // v300 optional ABI: VI scanout executed on the GX worker, in FIFO order,
  // without pumping window events (those stay on the guest/main thread).
  PresentXfbExFn present_xfb_async_ = nullptr;
  ConsumeFrameReadyFn consume_frame_ready_ = nullptr;
  ConsumeFrameReadyFn consume_frame_ready_async_ = nullptr;
  PresentInterpolatedFn present_interpolated_ = nullptr;
  InterpolationReadyFn interpolation_ready_ = nullptr;
  SetAspectModeFn set_aspect_mode_ = nullptr;
  TargetAspectFn target_aspect_ = nullptr;
  CameraSignatureFn camera_signature_ = nullptr;
  ShouldQuitFn should_quit_ = nullptr;
  ShutdownFn shutdown_ = nullptr;
  SetPeCallbackFn set_pe_callback_ = nullptr;
  PeEventSinkFn pe_event_sink_ = nullptr;
  void* pe_event_sink_user_ = nullptr;
  bool ready_ = false;
  // Keep Aurora mapped until process exit: WebGPU may deliver spontaneous
  // cancellation callbacks after aurora_shutdown() begins. native-run is
  // intentionally one-game-per-process, so unloading brings no steady-state win.
  bool pin_library_on_close_ = false;
  bool quit_requested_ = false;
  bool cycle_present_enabled_ = true;
  bool widescreen_active_ = false;
  std::uint64_t present_cycles_ = 8'100'000ull;
  std::uint64_t pending_cycles_ = 0;
  static constexpr std::size_t kWriteBatchCapacity = 4096u;
  std::array<std::uint8_t, kWriteBatchCapacity> write_batch_{};
  std::uint32_t write_batch_size_ = 0u;
  std::uint32_t write_batch_limit_ = 512u;
  std::uint32_t write_batch_first_pc_ = 0u;
  // Optional producer lease of an unpublished queue slot. The consumer owns
  // only published slots; their bytes remain immutable until consumption ends.
  bool direct_batch_slot_ = false;
  bool idle_drain_fast_ = true;
  bool write_slot_reserved_ = false;
  std::size_t write_slot_index_ = 0u;
  std::uint8_t* WriteBatchBuffer();

  // v36 CPU -> GX parser queue.  Slots are allocated once when parallel mode
  // is enabled, avoiding per-burst heap traffic in the WGPIPE hot path.
  static constexpr std::size_t kAsyncBurstCapacity = 4096u;
  using ArrayHint=NativeSdkCommand;
  static constexpr unsigned kArrayHints=64;
  std::array<ArrayHint,kArrayHints> array_hints_{};
  unsigned array_hint_count_=0;
  struct AsyncBurst {
    std::array<std::uint8_t, kAsyncBurstCapacity> bytes{};
    std::uint32_t size = 0u;
    std::uint32_t guest_pc = 0u;
    // v300: ordered host commands share the FIFO queue. MemoryWrites stores
    // `size` (address,bytes) pairs in `bytes`; PresentXfb uses `args`.
    AsyncKind kind = AsyncKind::Burst;
    std::array<std::uint32_t, 5> args{};
    std::array<ArrayHint,kArrayHints> arrays{};
    unsigned array_count=0;
  };
  void TakeArrayHints(AsyncBurst& slot,std::uint32_t bytes);
  bool SubmitAnnotatedBurst(const std::uint8_t* bytes,std::uint32_t size,std::uint32_t pc,
                            const AsyncBurst& hints);
  static constexpr std::uint32_t kMemoryWriteEntries =
      static_cast<std::uint32_t>(kAsyncBurstCapacity / (2u * sizeof(std::uint32_t)));
  std::vector<AsyncBurst> async_queue_{};
  std::thread async_worker_{};
  std::mutex async_mutex_{};
  std::condition_variable async_not_empty_{};
  std::condition_variable async_not_full_{};
  std::condition_variable async_drained_{};
  std::size_t async_head_ = 0u;
  std::size_t async_tail_ = 0u;
  std::size_t async_count_ = 0u;
  std::size_t async_peak_depth_ = 0u;
  bool async_active_ = false;
  std::atomic<bool> async_stop_{false};
  std::atomic<bool> async_failed_{false};
  bool async_writes_enabled_ = false;
  std::atomic<std::size_t> async_pending_bursts_{0};
  std::uint64_t async_submitted_bursts_ = 0u;
  std::uint64_t async_processed_bursts_ = 0u;
  std::uint64_t async_backpressure_waits_ = 0u;
  // v300 ordered host commands (memory publication, VI scanout).
  bool async_commands_ = true;
  bool tick_publish_ = false;
  bool early_pe_publish_ = true;
  bool frame_ready_fence_ = false;
  std::size_t memory_write_slot_ = static_cast<std::size_t>(-1);
  std::uint32_t presents_since_poll_ = 0u;
  static constexpr std::uint32_t kForcedEventPollPresents = 8u;
  struct SyncStat {
    std::uint64_t calls = 0u;
    std::uint64_t waits = 0u;
    std::uint64_t wait_ns = 0u;
  };
  bool sync_profile_ = false;
  std::array<SyncStat, static_cast<std::size_t>(SyncReason::Count)> sync_stats_{};
  std::uint64_t async_memory_writes_ = 0u;
  std::uint64_t async_presents_ = 0u;
  PeEventQueue pe_events_;
  std::atomic<bool> pe_event_overflow_{false};
  std::atomic<bool> pe_finish_seen_{false};
  std::atomic<std::uint16_t> pe_token_{0};
  std::atomic<bool> pe_token_seen_{false};
  std::atomic<bool> pe_token_interrupt_seen_{false};
  std::string last_error_;
};

} // namespace GekkoAOT::GX
