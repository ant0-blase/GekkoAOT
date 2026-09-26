#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include "native/address_space.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

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

  static bool IsWriteGatherPipe(std::uint32_t address);
  bool Write(std::uint64_t value, std::uint8_t size, std::uint32_t guest_pc);
  // Optional bulk WGPIPE ABI. Newer NativeGX plugins consume one already
  // ordered byte span instead of crossing the DSO boundary once per guest
  // store. Older plugins transparently fall back to byte writes.
  bool WriteBurst(const std::uint8_t* bytes, std::uint32_t size, std::uint32_t guest_pc);
  // Optional v141 dynamic-vertex coherency ABI. Guest D-cache publication
  // invalidates overlapping Aurora indexed-array snapshots.
  void NotifyCacheControl(std::uint8_t operation, std::uint32_t address);
  // Optional v162 CPU-visible EFB depth aperture. This mirrors Aurora's
  // GXPeekZ contract: the first read may return 0 while scheduling the first
  // asynchronous depth snapshot; subsequent reads use the newest completed one.
  bool PeekEfbZ(std::uint16_t x, std::uint16_t y, std::uint32_t* z);
  // Optional v163 CPU-visible EFB color aperture for GXPeekARGB. Same
  // asynchronous snapshot contract as PeekEfbZ.
  bool PeekEfbArgb(std::uint16_t x, std::uint16_t y, std::uint32_t* argb);
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
  bool PeEventPending() const {
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
  using CacheControlFn = void (*)(std::uint8_t operation, std::uint32_t address);
  using PeekEfbZFn = bool (*)(std::uint16_t x, std::uint16_t y, std::uint32_t* z);
  using PeekEfbArgbFn = bool (*)(std::uint16_t x, std::uint16_t y, std::uint32_t* argb);
  using PresentFn = void (*)();
  using PresentXfbFn = void (*)(std::uint32_t top, std::uint32_t bottom);
  using PresentXfbExFn = void (*)(std::uint32_t top, std::uint32_t bottom,
                                  std::uint32_t width, std::uint32_t stride_bytes,
                                  std::uint32_t field_height);
  using ConsumeFrameReadyFn = bool (*)(std::uint32_t* xfb_address);
  using PresentInterpolatedFn = bool (*)(float alpha);
  using InterpolationReadyFn = bool (*)();
  using SetAspectModeFn = void (*)(std::uint32_t mode);
  using ShouldQuitFn = bool (*)();
  using ShutdownFn = void (*)();
  using SetPeCallbackFn = void (*)(PeEventFn callback, void* user);

  static bool ResolveGuest(void* user, std::uint32_t address, std::uint32_t size,
                           std::uint32_t space, std::uint32_t resource,
                           const void** data, std::uint32_t* available);
  static void OnPeEvent(void* user, std::uint32_t reg, std::uint32_t value);

  void* FindSymbol(const char* name);
  std::uint32_t ContiguousAvailable(std::uint32_t address) const;
  void Present();

  void* library_ = nullptr;
  Native::AddressSpace* memory_ = nullptr;
  InitFn init_ = nullptr;
  WriteFn write_ = nullptr;
  WriteBurstFn write_burst_ = nullptr;
  CacheControlFn cache_control_ = nullptr;
  PeekEfbZFn peek_efb_z_ = nullptr;
  PeekEfbArgbFn peek_efb_argb_ = nullptr;
  PresentFn present_ = nullptr;
  PresentXfbFn present_xfb_ = nullptr;
  PresentXfbExFn present_xfb_ex_ = nullptr;
  ConsumeFrameReadyFn consume_frame_ready_ = nullptr;
  PresentInterpolatedFn present_interpolated_ = nullptr;
  InterpolationReadyFn interpolation_ready_ = nullptr;
  SetAspectModeFn set_aspect_mode_ = nullptr;
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
  std::uint64_t present_cycles_ = 8'100'000ull;
  std::uint64_t pending_cycles_ = 0;
  std::atomic<bool> pe_finish_seen_{false};
  std::atomic<std::uint16_t> pe_token_{0};
  std::atomic<bool> pe_token_seen_{false};
  std::atomic<bool> pe_token_interrupt_seen_{false};
  std::string last_error_;
};

} // namespace GekkoAOT::GX
