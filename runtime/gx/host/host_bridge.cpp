// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx/host/host_bridge.h"

#include <algorithm>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <xmmintrin.h>
#endif
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>

#if defined(__linux__)
#include <pthread.h>
#endif

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace GekkoAOT::GX {
namespace {

std::uint64_t EnvUnsigned64(const char* name, std::uint64_t fallback) {
  const char* text = std::getenv(name);
  if (!text || !*text) return fallback;
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 0);
  return errno == 0 && end != text && *end == '\0' ? static_cast<std::uint64_t>(value)
                                                    : fallback;
}

bool EnvBool(const char* name, bool fallback) {
  const char* text = std::getenv(name);
  if (!text || !*text) return fallback;
  return std::strcmp(text, "0") != 0 && std::strcmp(text, "false") != 0 &&
         std::strcmp(text, "off") != 0;
}

#ifdef _WIN32
std::string WindowsError() {
  const DWORD code = GetLastError();
  if (!code) return {};
  LPSTR buffer = nullptr;
  const DWORD size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                        FORMAT_MESSAGE_IGNORE_INSERTS,
                                    nullptr, code, 0, reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
  std::string out = size && buffer ? std::string(buffer, size)
                                   : ("Win32 error " + std::to_string(code));
  if (buffer) LocalFree(buffer);
  return out;
}
#endif

} // namespace

HostBridge::~HostBridge() { Close(); }

void* HostBridge::FindSymbol(const char* name) {
  if (!library_ || !name) return nullptr;
#ifdef _WIN32
  return reinterpret_cast<void*>(
      GetProcAddress(reinterpret_cast<HMODULE>(library_), name));
#else
  dlerror();
  void* symbol = dlsym(library_, name);
  (void)dlerror();
  return symbol;
#endif
}

bool HostBridge::Open(const std::filesystem::path& path, Native::AddressSpace& memory) {
  Close();
  last_error_.clear();
#ifdef _WIN32
  library_ = reinterpret_cast<void*>(LoadLibraryW(path.wstring().c_str()));
  if (!library_) {
    last_error_ = WindowsError();
    return false;
  }
#else
  library_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library_) {
    if (const char* error = dlerror()) last_error_ = error;
    return false;
  }
#endif

  init_ = reinterpret_cast<InitFn>(FindSymbol("gekkoaot_native_gx_init"));
  write_ = reinterpret_cast<WriteFn>(FindSymbol("gekkoaot_native_gx_write"));
  write_burst_ = reinterpret_cast<WriteBurstFn>(
      FindSymbol("gekkoaot_native_gx_write_burst"));
  sdk_array_ = reinterpret_cast<SdkArrayFn>(FindSymbol("gekkoaot_native_gx_sdk_array"));
  command_burst_ = reinterpret_cast<CommandBurstFn>(FindSymbol("gekkoaot_native_gx_write_commands"));
  sdk_commands_ = reinterpret_cast<CommandBurstFn>(FindSymbol("gekkoaot_native_gx_write_sdk_commands_v317"));
  cache_control_ = reinterpret_cast<CacheControlFn>(
      FindSymbol("gekkoaot_native_gx_cache_control"));
  memory_write_ = reinterpret_cast<MemoryWriteFn>(
      FindSymbol("gekkoaot_native_gx_memory_write"));
  peek_efb_z_ = reinterpret_cast<PeekEfbZFn>(
      FindSymbol("gekkoaot_native_gx_peek_z"));
  peek_efb_argb_ = reinterpret_cast<PeekEfbArgbFn>(
      FindSymbol("gekkoaot_native_gx_peek_argb"));
  read_bbox_ = reinterpret_cast<ReadBoundingBoxFn>(
      FindSymbol("gekkoaot_native_gx_read_bbox"));
  poke_efb_ = reinterpret_cast<PokeEfbFn>(FindSymbol("gekkoaot_native_gx_poke_efb"));
  present_ = reinterpret_cast<PresentFn>(FindSymbol("gekkoaot_native_gx_present"));
  present_xfb_ = reinterpret_cast<PresentXfbFn>(FindSymbol("gekkoaot_native_gx_present_xfb"));
  present_xfb_ex_ = reinterpret_cast<PresentXfbExFn>(
      FindSymbol("gekkoaot_native_gx_present_xfb_ex"));
  present_xfb_async_ = reinterpret_cast<PresentXfbExFn>(
      FindSymbol("gekkoaot_native_gx_present_xfb_async"));
  // Optional: older NativeGX plugins remain valid and simply keep VI-driven
  // presentation. No hard ABI bump is required for this experiment.
  consume_frame_ready_ = reinterpret_cast<ConsumeFrameReadyFn>(
      FindSymbol("gekkoaot_native_gx_consume_frame_ready"));
  consume_frame_ready_async_ = reinterpret_cast<ConsumeFrameReadyFn>(
      FindSymbol("gekkoaot_native_gx_consume_frame_ready_async"));
  present_interpolated_ = reinterpret_cast<PresentInterpolatedFn>(
      FindSymbol("gekkoaot_native_gx_present_interpolated"));
  interpolation_ready_ = reinterpret_cast<InterpolationReadyFn>(
      FindSymbol("gekkoaot_native_gx_frame_interpolation_ready"));
  set_aspect_mode_ = reinterpret_cast<SetAspectModeFn>(
      FindSymbol("gekkoaot_native_gx_set_aspect_mode"));
  target_aspect_ = reinterpret_cast<TargetAspectFn>(
      FindSymbol("gekkoaot_native_gx_widescreen_target_aspect"));
  camera_signature_ = reinterpret_cast<CameraSignatureFn>(
      FindSymbol("gekkoaot_native_gx_camera_signature"));
  should_quit_ = reinterpret_cast<ShouldQuitFn>(FindSymbol("gekkoaot_native_gx_should_quit"));
  shutdown_ = reinterpret_cast<ShutdownFn>(FindSymbol("gekkoaot_native_gx_shutdown"));
  set_pe_callback_ = reinterpret_cast<SetPeCallbackFn>(
      FindSymbol("gekkoaot_native_gx_set_pe_callback"));

  if (!init_ || !write_ || !present_ || !present_xfb_ || !shutdown_) {
    last_error_ = "native GX plugin ABI is incomplete (address-keyed XFB v10b required)";
    Close();
    return false;
  }

  memory_ = &memory;
  present_cycles_ = EnvUnsigned64("GEKKOAOT_NATIVE_GX_PRESENT_CYCLES", 8'100'000ull);
  pending_cycles_ = 0;
  write_batch_size_ = 0u;
  array_hint_count_ = 0u;
  write_batch_first_pc_ = 0u;
  direct_batch_slot_ = EnvBool("GEKKOAOT_GX_DIRECT_BATCH_SLOT", true);
  idle_drain_fast_ = EnvBool("GEKKOAOT_GX_IDLE_DRAIN_FAST", true);
  // v300: memory publication and VI scanout become ordered GX-worker commands
  // instead of guest-thread drains. 0 restores the v36 drain-then-call path.
  async_commands_ = EnvBool("GEKKOAOT_GX_ASYNC_COMMANDS", true);
  typed_arrays_ = EnvBool("GEKKOAOT_NATIVE_GX_TYPED_ARRAYS", false) &&
                  !EnvBool("GEKKOAOT_NATIVE_GX_FIFO_FORENSIC", false) &&
                  !std::getenv("GEKKOAOT_GX_DIAG_FIFO_CAPTURE");
  direct_matrices_ = EnvBool("GEKKOAOT_NATIVE_GX_DIRECT_MATRICES", true) &&
                     !EnvBool("GEKKOAOT_NATIVE_GX_FIFO_FORENSIC", false) &&
                     !std::getenv("GEKKOAOT_GX_DIAG_FIFO_CAPTURE");
  std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_DIRECT_MATRICES_V317=1 enabled=%u transport=ordered-batch\n",
               HasDirectMatrixCommands()?1u:0u);
  tick_publish_ = EnvBool("GEKKOAOT_GX_TICK_PUBLISH", false);
  early_pe_publish_ = EnvBool("GEKKOAOT_GX_EARLY_PE_PUBLISH", true);
  sync_profile_ = EnvBool("GEKKOAOT_RUNTIME_PROFILE", false) ||
                  EnvBool("GEKKOAOT_GX_SYNC_PROFILE", false);
  sync_stats_ = {};
  frame_ready_fence_ = false;
  memory_write_slot_ = static_cast<std::size_t>(-1);
  presents_since_poll_ = 0u;
  async_memory_writes_ = 0u;
  async_presents_ = 0u;
  write_slot_reserved_ = false;
  write_slot_index_ = 0u;
  {
    const auto requested = EnvUnsigned64("GEKKOAOT_WGPIPE_BATCH_BYTES", 512u);
    const auto clamped = std::clamp<std::uint64_t>(requested, 32u, kWriteBatchCapacity);
    write_batch_limit_ = static_cast<std::uint32_t>(clamped & ~31ull);
  if (write_batch_limit_ == 0u) write_batch_limit_ = 32u;
  }
  std::fprintf(stderr,
               "GEKKOAOT_GX_DIRECT_BATCH_SLOT_V242=1 enabled=%u publication=mutex fence=retained\n",
               direct_batch_slot_ ? 1u : 0u);
  quit_requested_ = false;
  cycle_present_enabled_ = true;
  widescreen_active_ = false;
  pe_events_.Reset();
  pe_event_overflow_.store(false,std::memory_order_release);
  pe_finish_seen_.store(false, std::memory_order_relaxed);
  pe_token_.store(0, std::memory_order_relaxed);
  pe_token_seen_.store(false, std::memory_order_relaxed);
  pe_token_interrupt_seen_.store(false, std::memory_order_relaxed);
  async_head_ = async_tail_ = async_count_ = async_peak_depth_ = 0u;
  async_active_ = false;
  async_stop_ = false;
  async_failed_ = false;
  async_writes_enabled_ = false;
  async_submitted_bursts_ = 0u;
  async_processed_bursts_ = 0u;
  async_backpressure_waits_ = 0u;
  async_queue_.clear();

  if (!init_(&HostBridge::ResolveGuest, this)) {
    last_error_ = "native GX plugin initialization failed";
    Close();
    return false;
  }
  ready_ = true;
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_FAKE_VMEM_V86=1 base=%08x bytes=%u resolver=address-space-parity\n",
               Native::AddressSpace::FakeVmemBase,
               Native::AddressSpace::RetailFakeVmemSize);
  std::fprintf(stderr,
               "GEKKOAOT_WGPIPE_BATCH_V161D=1 mode=cp-batched max-bytes=%u ordering=flush-at-observable-boundaries\n",
               write_batch_limit_);
  // Dawn/WebGPU MapAsync uses spontaneous callbacks. A callback cancelled by
  // aurora_shutdown() may be delivered after shutdown has started, so dlclose
  // here is unsafe even though guest execution has already stopped. native-run
  // hosts one game per process; leave the plugin mapped and let process teardown
  // reclaim it. This can be disabled for diagnostics.
  pin_library_on_close_ = EnvBool("GEKKOAOT_NATIVE_GX_PIN_LIBRARY", true);
  if (set_pe_callback_) set_pe_callback_(&HostBridge::OnPeEvent, this);
  return true;
}

void HostBridge::Close() {
  if (ready_) (void)FlushWrites();
  StopAsyncWorker();
  const bool was_ready = ready_;
  const bool keep_mapped = was_ready && pin_library_on_close_ && library_;
  if (ready_ && set_pe_callback_) set_pe_callback_(nullptr, nullptr);
  if (ready_ && shutdown_) shutdown_();
  ready_ = false;
  widescreen_active_ = false;
  memory_ = nullptr;
  init_ = nullptr;
  write_ = nullptr;
  write_burst_ = nullptr;
  sdk_array_ = nullptr;
  command_burst_ = nullptr;
  sdk_commands_ = nullptr;
  cache_control_ = nullptr;
  memory_write_ = nullptr;
  peek_efb_z_ = nullptr;
  peek_efb_argb_ = nullptr;
  read_bbox_ = nullptr;
  poke_efb_ = nullptr;
  present_ = nullptr;
  present_xfb_ = nullptr;
  present_xfb_ex_ = nullptr;
  present_xfb_async_ = nullptr;
  consume_frame_ready_ = nullptr;
  consume_frame_ready_async_ = nullptr;
  present_interpolated_ = nullptr;
  interpolation_ready_ = nullptr;
  set_aspect_mode_ = nullptr;
  target_aspect_ = nullptr;
  camera_signature_ = nullptr;
  should_quit_ = nullptr;
  shutdown_ = nullptr;
  set_pe_callback_ = nullptr;
  pe_event_sink_ = nullptr;
  pe_event_sink_user_ = nullptr;
  if (keep_mapped) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_DSO_PIN_V12=1 policy=process-lifetime reason=async-webgpu-callbacks\n");
  } else {
#ifdef _WIN32
    if (library_) FreeLibrary(reinterpret_cast<HMODULE>(library_));
#else
    if (library_) dlclose(library_);
#endif
  }
  library_ = nullptr;
  pin_library_on_close_ = false;
}

std::uint32_t HostBridge::ContiguousAvailable(std::uint32_t address) const {
  if (!memory_) return 0;

  // GEKKOAOT_NATIVE_GX_FAKE_VMEM_V86:
  // AddressSpace::Resolve() gives the SDK VM compatibility aperture
  // (0x7e000000..0x7fffffff) normal host-backed RAM semantics. NativeGX must
  // report the same contiguous span to Aurora. Previously this helper applied
  // ToPhysical() first, classified FakeVMEM as neither MEM1 nor MEM2 and
  // returned zero even though Resolve() would have succeeded. That made GX
  // arrays/textures/display lists living in the VM window disappear only on
  // the renderer side.
  if (address >= Native::AddressSpace::FakeVmemBase &&
      address < Native::AddressSpace::FakeVmemBase +
                    Native::AddressSpace::RetailFakeVmemSize) {
    const std::uint32_t offset = address - Native::AddressSpace::FakeVmemBase;
    const auto fake = memory_->FakeVmem();
    if (offset < fake.size()) {
      const std::size_t available = fake.size() - offset;
      return static_cast<std::uint32_t>(std::min<std::size_t>(
          available, std::numeric_limits<std::uint32_t>::max()));
    }
    return 0;
  }

  const std::uint32_t physical = Native::AddressSpace::ToPhysical(address);
  std::size_t available = 0;
  if (physical < memory_->Mem1().size()) {
    available = memory_->Mem1().size() - physical;
  } else if (physical >= Native::AddressSpace::Mem2PhysicalBase) {
    const std::uint32_t offset = physical - Native::AddressSpace::Mem2PhysicalBase;
    if (offset < memory_->Mem2().size()) available = memory_->Mem2().size() - offset;
  }
  return static_cast<std::uint32_t>(
      std::min<std::size_t>(available, std::numeric_limits<std::uint32_t>::max()));
}

bool HostBridge::ResolveGuest(void* user, std::uint32_t address, std::uint32_t size,
                              std::uint32_t, std::uint32_t,
                              const void** data, std::uint32_t* available) {
  auto* self = static_cast<HostBridge*>(user);
  if (!self || !self->memory_ || !data || !available) return false;
  const std::uint32_t contiguous = self->ContiguousAvailable(address);
  const std::uint32_t required = std::max<std::uint32_t>(size, 1u);
  if (contiguous < required) return false;
  auto* pointer = self->memory_->Resolve(address, required);
  if (!pointer) return false;
  *data = pointer;
  *available = contiguous;
  return true;
}

void HostBridge::OnPeEvent(void* user, std::uint32_t reg, std::uint32_t value) {
  auto* self = static_cast<HostBridge*>(user);
  if (!self) return;
  if (self->pe_event_sink_) {
    self->pe_event_sink_(self->pe_event_sink_user_, reg, value);
    return;
  }
  if((reg==0x45u && (value&0xffu)==2u)||reg==0x47u||reg==0x48u) {
    if(!self->pe_events_.Push({reg,value}))
      self->pe_event_overflow_.store(true,std::memory_order_release);
    // v300: an idle guest thread may be sleeping in WaitForWorkerEvent for
    // exactly this token/finish. The event is already published; taking the
    // queue mutex orders the notification after the waiter's predicate check.
    if (self->async_writes_enabled_) {
      { std::lock_guard lock(self->async_mutex_); }
      self->async_drained_.notify_all();
    }
  }
}

void HostBridge::SetAsyncWritesEnabled(bool enabled) {
  if (enabled == async_writes_enabled_) return;
  if (!enabled) {
    (void)FlushWrites();
    StopAsyncWorker();
    return;
  }
  if (!ready_) return;

  const auto requested = EnvUnsigned64("GEKKOAOT_GX_WORKER_QUEUE_SLOTS", 64u);
  const auto slots = static_cast<std::size_t>(std::clamp<std::uint64_t>(requested, 4u, 256u));
  {
    std::lock_guard lock(async_mutex_);
    async_queue_.clear();
    async_queue_.resize(slots);
    async_head_ = async_tail_ = async_count_ = async_peak_depth_ = 0u;
    async_active_ = false;
    async_stop_ = false;
    async_failed_ = false;
    async_submitted_bursts_ = 0u;
    async_processed_bursts_ = 0u;
    async_pending_bursts_.store(0u, std::memory_order_relaxed);
    async_backpressure_waits_ = 0u;
    memory_write_slot_ = static_cast<std::size_t>(-1);
    async_writes_enabled_ = true;
  }
  async_worker_ = std::thread(&HostBridge::AsyncWorkerMain, this);
  std::fprintf(stderr,
               "GEKKOAOT_GX_PARSER_WORKER_V36=1 mode=async queue_slots=%zu slot_bytes=%zu "
               "ordering=flush-on-cp-pe-pi+present backpressure=bounded\n",
               slots, kAsyncBurstCapacity);
  const bool async_present = async_commands_ && present_xfb_async_ != nullptr;
  std::fprintf(stderr,
               "GEKKOAOT_GX_ASYNC_COMMANDS_V300=%u memory_publication=%s vi_scanout=%s "
               "frame_ready=%s window_events=guest-thread-idle-or-every-%u-fields\n",
               async_commands_ ? 1u : 0u,
               async_commands_ && memory_write_ ? "ordered-worker" : "drain",
               async_present ? "ordered-worker" : "drain",
               async_commands_ ? "mailbox" : "drain", kForcedEventPollPresents);
}

void HostBridge::StopAsyncWorker() {
  if (!async_writes_enabled_ && !async_worker_.joinable()) return;
  {
    std::lock_guard lock(async_mutex_);
    async_stop_ = true;
  }
  async_not_empty_.notify_all();
  async_not_full_.notify_all();
  async_drained_.notify_all();
  if (async_worker_.joinable()) async_worker_.join();
  const bool had_worker = async_writes_enabled_;
  async_writes_enabled_ = false;
  if (had_worker) {
    std::fprintf(stderr,
                 "GEKKOAOT_GX_PARSER_WORKER_V36=0 submitted=%llu processed=%llu "
                 "backpressure=%llu peak_depth=%zu\n",
                 static_cast<unsigned long long>(async_submitted_bursts_),
                 static_cast<unsigned long long>(async_processed_bursts_),
                 static_cast<unsigned long long>(async_backpressure_waits_),
                 async_peak_depth_);
  }
  async_queue_.clear();
  write_slot_reserved_ = false;
  write_slot_index_ = 0u;
  async_head_ = async_tail_ = async_count_ = async_peak_depth_ = 0u;
  async_active_ = false;
  async_stop_ = false;
  async_pending_bursts_.store(0u, std::memory_order_relaxed);
  memory_write_slot_ = static_cast<std::size_t>(-1);
}

bool HostBridge::SubmitBurstImmediate(const std::uint8_t* bytes, std::uint32_t size,
                                      std::uint32_t guest_pc) {
  if (!bytes || size == 0u) return false;
  if (write_burst_) {
    write_burst_(bytes, size, guest_pc);
    return true;
  }
  if (!write_) return false;
  for (std::uint32_t i = 0; i < size; ++i) write_(bytes[i], 1u, guest_pc);
  return true;
}

bool HostBridge::EnqueueBurst(const std::uint8_t* bytes, std::uint32_t size,
                              std::uint32_t guest_pc) {
  if (!async_writes_enabled_) {
    if(!array_hint_count_)return SubmitBurstImmediate(bytes,size,guest_pc);
    AsyncBurst hints;TakeArrayHints(hints,size);
    return SubmitAnnotatedBurst(bytes,size,guest_pc,hints);
  }
  if (!bytes || size == 0u) return false;

  std::uint32_t offset = 0u;
  while (offset < size) {
    const std::uint32_t chunk =
        std::min<std::uint32_t>(size - offset, static_cast<std::uint32_t>(kAsyncBurstCapacity));
    std::unique_lock lock(async_mutex_);
    if (async_count_ == async_queue_.size() && !async_stop_ && !async_failed_) {
      ++async_backpressure_waits_;
      async_not_full_.wait(lock, [this] {
        return async_count_ < async_queue_.size() || async_stop_ || async_failed_;
      });
    }
    if (async_stop_ || async_failed_ || async_queue_.empty()) return false;
    auto& slot = async_queue_[async_tail_];
    std::memcpy(slot.bytes.data(), bytes + offset, chunk);
    slot.size = chunk;
    slot.guest_pc = guest_pc;
    slot.kind = AsyncKind::Burst;
    TakeArrayHints(slot,chunk);
    async_tail_ = (async_tail_ + 1u) % async_queue_.size();
    ++async_count_;
    async_pending_bursts_.fetch_add(1u, std::memory_order_relaxed);
    ++async_submitted_bursts_;
    async_peak_depth_ = std::max(async_peak_depth_, async_count_);
    lock.unlock();
    async_not_empty_.notify_one();
    offset += chunk;
  }
  return true;
}

void HostBridge::AsyncWorkerMain() {
#if defined(__linux__)
  (void)pthread_setname_np(pthread_self(), "GekkoAOT-GX");
#endif
  for (;;) {
    std::size_t index = 0u;
    {
      std::unique_lock lock(async_mutex_);
      async_not_empty_.wait(lock, [this] { return async_count_ != 0u || async_stop_; });
      if (async_count_ == 0u && async_stop_) break;
      index = async_head_;
      async_active_ = true;
    }

    const bool ok = ExecuteAsyncItem(index);

    {
      std::lock_guard lock(async_mutex_);
      if (!ok) async_failed_ = true;
      async_head_ = (async_head_ + 1u) % async_queue_.size();
      --async_count_;
      ++async_processed_bursts_;
      async_active_ = false;
      async_pending_bursts_.fetch_sub(1u, std::memory_order_release);
      if (async_count_ == 0u) async_drained_.notify_all();
    }
    async_not_full_.notify_one();
    if (!ok) {
      async_not_full_.notify_all();
      async_drained_.notify_all();
      break;
    }
  }
}

bool HostBridge::Write(std::uint64_t value, std::uint8_t size, std::uint32_t guest_pc) {
  if (!ready_ || !write_) return false;
  if (size != 1 && size != 2 && size != 4 && size != 8) return false;
  write_(value, size, guest_pc);
  // Do not call aurora_update()/should_quit after every WGPIPE store. Window
  // events are frame-rate work and are polled by Present(), not guest-store work.
  return true;
}

bool HostBridge::FlushBufferedWrites() {
  if (!ready_ || write_batch_size_ == 0u) return true;
  const std::uint32_t size = write_batch_size_;
  const std::uint32_t guest_pc = write_batch_first_pc_;
  write_batch_size_ = 0u;
  write_batch_first_pc_ = 0u;
  if (write_slot_reserved_) {
    std::unique_lock lock(async_mutex_);
    write_slot_reserved_ = false;
    if (async_stop_ || async_failed_ || async_queue_.empty() ||
        write_slot_index_ != async_tail_ || async_count_ >= async_queue_.size())
      return false;
    // The producer has already written the bytes into this unpublished slot.
    // Publish once, under the same release/acquire mutex as the existing queue.
    auto& slot = async_queue_[write_slot_index_];
    slot.size = size;
    slot.guest_pc = guest_pc;
    slot.kind = AsyncKind::Burst;
    TakeArrayHints(slot,size);
    async_tail_ = (async_tail_ + 1u) % async_queue_.size();
    ++async_count_;
    async_pending_bursts_.fetch_add(1u, std::memory_order_relaxed);
    ++async_submitted_bursts_;
    async_peak_depth_ = std::max(async_peak_depth_, async_count_);
    lock.unlock();
    async_not_empty_.notify_one();
    return true;
  }
  return EnqueueBurst(write_batch_.data(), size, guest_pc);
}

std::uint8_t* HostBridge::WriteBatchBuffer() {
  if (!direct_batch_slot_ || !async_writes_enabled_) return write_batch_.data();
  if (write_slot_reserved_) return async_queue_[write_slot_index_].bytes.data();
  // One producer owns tail. The consumer releases pending credit only after
  // it finishes reading a slot. An acquire of that credit permits its reuse
  // without a second mutex acquisition on the normal, non-full queue path.
  // The publishing mutex below still makes the completed payload visible.
  if (!async_queue_.empty() &&
      async_pending_bursts_.load(std::memory_order_acquire) < async_queue_.size() &&
      !async_stop_.load(std::memory_order_relaxed) &&
      !async_failed_.load(std::memory_order_relaxed)) {
    write_slot_index_ = async_tail_;
    write_slot_reserved_ = true;
    return async_queue_[write_slot_index_].bytes.data();
  }
  std::unique_lock lock(async_mutex_);
  if (async_count_ == async_queue_.size() && !async_stop_ && !async_failed_) {
    ++async_backpressure_waits_;
    async_not_full_.wait(lock, [this] {
      return async_count_ < async_queue_.size() || async_stop_ || async_failed_;
    });
  }
  if (async_stop_ || async_failed_ || async_queue_.empty()) return nullptr;
  write_slot_index_ = async_tail_;
  write_slot_reserved_ = true;
  return async_queue_[write_slot_index_].bytes.data();
}

bool HostBridge::FlushWrites(SyncReason reason) {
  if (!ready_) return true;
  if (!FlushBufferedWrites()) return false;
  if (!async_writes_enabled_) return true;
  SyncStat* stat = sync_profile_ ? &sync_stats_[static_cast<std::size_t>(reason)] : nullptr;
  if (stat) ++stat->calls;

  // The guest CPU is the queue's sole producer. After publishing its partial
  // batch, zero outstanding credit means the parser completed every earlier
  // packet. Its release decrement publishes callbacks and parser state; this
  // acquire is the same visibility fence as taking the draining mutex, without
  // locking twice per CP/PE/PI read while a guest polling loop is already idle.
  if (idle_drain_fast_ && async_pending_bursts_.load(std::memory_order_acquire) == 0u)
    return !async_failed_.load(std::memory_order_acquire);

  const auto start = stat ? std::chrono::steady_clock::now()
                          : std::chrono::steady_clock::time_point{};
  std::unique_lock lock(async_mutex_);
  async_drained_.wait(lock, [this] {
    return (async_count_ == 0u && !async_active_) || async_failed_ || async_stop_;
  });
  if (stat) {
    ++stat->waits;
    stat->wait_ns += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
  }
  return !async_failed_;
}

bool HostBridge::PublishCommandLocked(std::unique_lock<std::mutex>& lock) {
  // Shared backpressure for ordered host commands. The caller published every
  // earlier WGPIPE byte first, so this slot is ordered after them.
  if (async_count_ == async_queue_.size() && !async_stop_ && !async_failed_) {
    ++async_backpressure_waits_;
    async_not_full_.wait(lock, [this] {
      return async_count_ < async_queue_.size() || async_stop_ || async_failed_;
    });
  }
  return !(async_stop_ || async_failed_ || async_queue_.empty());
}

bool HostBridge::EnqueueMemoryWrite(std::uint32_t address, std::uint32_t size) {
  if (!FlushBufferedWrites()) return false;
  std::unique_lock lock(async_mutex_);
  const std::size_t slots = async_queue_.size();
  // Coalesce into the newest published command while the worker has not
  // started it. Taking a slot happens under this mutex, so the append is either
  // fully visible to the worker or lands in a fresh slot.
  if (slots != 0u && async_count_ != 0u &&
      memory_write_slot_ == (async_tail_ + slots - 1u) % slots &&
      !(async_active_ && async_head_ == memory_write_slot_)) {
    auto& slot = async_queue_[memory_write_slot_];
    if (slot.kind == AsyncKind::MemoryWrites && slot.size != 0u) {
      std::uint32_t last[2];
      auto* entries = slot.bytes.data();
      std::memcpy(last, entries + (slot.size - 1u) * sizeof(last), sizeof(last));
      // dcbf/dcbst loops publish ascending 32-byte lines: extend the range.
      const std::uint64_t last_end = std::uint64_t(last[0]) + last[1];
      const std::uint64_t end = std::uint64_t(address) + size;
      if (address >= last[0] && address <= last_end && end <= 0xffffffffull) {
        last[1] = static_cast<std::uint32_t>(std::max(last_end, end) - last[0]);
        std::memcpy(entries + (slot.size - 1u) * sizeof(last), last, sizeof(last));
        ++async_memory_writes_;
        return true;
      }
      if (slot.size < kMemoryWriteEntries) {
        const std::uint32_t entry[2] = {address, size};
        std::memcpy(entries + slot.size * sizeof(entry), entry, sizeof(entry));
        ++slot.size;
        ++async_memory_writes_;
        return true;
      }
    }
  }
  if (!PublishCommandLocked(lock)) return false;
  auto& slot = async_queue_[async_tail_];
  const std::uint32_t entry[2] = {address, size};
  std::memcpy(slot.bytes.data(), entry, sizeof(entry));
  slot.size = 1u;
  slot.guest_pc = 0u;
  slot.kind = AsyncKind::MemoryWrites;
  memory_write_slot_ = async_tail_;
  async_tail_ = (async_tail_ + 1u) % slots;
  ++async_count_;
  async_pending_bursts_.fetch_add(1u, std::memory_order_relaxed);
  ++async_submitted_bursts_;
  ++async_memory_writes_;
  async_peak_depth_ = std::max(async_peak_depth_, async_count_);
  lock.unlock();
  async_not_empty_.notify_one();
  return true;
}

bool HostBridge::EnqueuePresentXfb(std::uint32_t top, std::uint32_t bottom,
                                   std::uint32_t width, std::uint32_t stride_bytes,
                                   std::uint32_t field_height) {
  if (!FlushBufferedWrites()) return false;
  std::unique_lock lock(async_mutex_);
  if (!PublishCommandLocked(lock)) return false;
  auto& slot = async_queue_[async_tail_];
  slot.size = 0u;
  slot.guest_pc = 0u;
  slot.kind = AsyncKind::PresentXfb;
  slot.args = {top, bottom, width, stride_bytes, field_height};
  async_tail_ = (async_tail_ + 1u) % async_queue_.size();
  ++async_count_;
  async_pending_bursts_.fetch_add(1u, std::memory_order_relaxed);
  ++async_submitted_bursts_;
  ++async_presents_;
  async_peak_depth_ = std::max(async_peak_depth_, async_count_);
  lock.unlock();
  async_not_empty_.notify_one();
  return true;
}

namespace {
inline void PrefetchLine(const void* address) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  _mm_prefetch(static_cast<const char*>(address), _MM_HINT_T0);
#elif defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(address, 0, 3);
#else
  (void)address;
#endif
}
}  // namespace

bool HostBridge::ExecuteAsyncItem(std::size_t index) {
  const auto& slot = async_queue_[index];
  switch (slot.kind) {
  case AsyncKind::Burst:
    // v336: the producer core just wrote these lines; a 512-byte batch is too
    // short for the hardware streamer, so the parser stalled on the first byte
    // of almost every command (21% of ProcessCommandOrRun in HPCOS). Request
    // the whole span up front so the transfers overlap.
    for (std::uint32_t line = 0u, end = std::min<std::uint32_t>(slot.size, 4096u); line < end; line += 64u)
      PrefetchLine(slot.bytes.data() + line);
    return SubmitAnnotatedBurst(slot.bytes.data(), slot.size, slot.guest_pc,slot);
  case AsyncKind::MemoryWrites:
    if (memory_write_) {
      for (std::uint32_t i = 0; i < slot.size; ++i) {
        std::uint32_t entry[2];
        std::memcpy(entry, slot.bytes.data() + i * sizeof(entry), sizeof(entry));
        memory_write_(entry[0], entry[1]);
      }
    }
    return true;
  case AsyncKind::PresentXfb:
    if (present_xfb_async_)
      present_xfb_async_(slot.args[0], slot.args[1], slot.args[2], slot.args[3], slot.args[4]);
    return true;
  }
  return true;
}

bool HostBridge::WaitForWorkerEvent(std::chrono::steady_clock::time_point until) {
  if (!ready_ || !async_writes_enabled_) return false;
  if (!PublishPendingWrites()) return false;
  std::unique_lock lock(async_mutex_);
  const auto worker_settled = [this] {
    return pe_events_.Pending() || (async_count_ == 0u && !async_active_) ||
           async_failed_ || async_stop_;
  };
  if (worker_settled()) return false;
  async_drained_.wait_until(lock, until, worker_settled);
  return true;
}

void HostBridge::ReportSyncProfile(double seconds) {
  if (!sync_profile_ || !(seconds > 0.0)) return;
  static constexpr const char* kNames[] = {"other", "frame_ready", "present", "memory",
                                           "cache", "mmio_read", "mmio_write", "efb"};
  static_assert(std::size(kNames) == static_cast<std::size_t>(SyncReason::Count));
  char line[1024];
  int used = std::snprintf(line, sizeof(line), "GEKKOAOT_GX_SYNC_V300=1");
  for (std::size_t i = 0; i < sync_stats_.size() && used > 0 &&
                          static_cast<std::size_t>(used) < sizeof(line); ++i) {
    const auto& stat = sync_stats_[i];
    if (stat.calls == 0u) continue;
    used += std::snprintf(line + used, sizeof(line) - static_cast<std::size_t>(used),
                          " %s=%.0f/%.0f/%.2fms", kNames[i],
                          static_cast<double>(stat.calls) / seconds,
                          static_cast<double>(stat.waits) / seconds,
                          static_cast<double>(stat.wait_ns) / 1e6 / seconds);
  }
  std::fprintf(stderr, "%s async_mem=%llu async_present=%llu backpressure=%llu peak=%zu "
                       "(per-second calls/waits/wait-ms)\n",
               line, static_cast<unsigned long long>(async_memory_writes_),
               static_cast<unsigned long long>(async_presents_),
               static_cast<unsigned long long>(async_backpressure_waits_), async_peak_depth_);
  sync_stats_ = {};
}

namespace {
// v335: a BP write to PE_DONE (0x45) or a PE token register (0x47/0x48) is what
// GXDrawDone/GXSetDrawSync waits on. Publish the batch holding it immediately
// so the worker starts while the guest is still on its way to the wait
// (GoldenEye issues ~90 DrawDone per frame). A data byte that happens to look
// like this pattern only costs an early publication.
bool HasPeSyncCommand(const std::uint8_t* bytes, std::uint32_t size) {
  for (std::uint32_t i = 0u; i + 1u < size; ++i)
    if (bytes[i] == 0x61u && (bytes[i + 1u] == 0x45u || bytes[i + 1u] == 0x47u || bytes[i + 1u] == 0x48u))
      return true;
  return false;
}
}  // namespace

bool HostBridge::WriteBurst(const std::uint8_t* bytes, std::uint32_t size,
                            std::uint32_t guest_pc) {
  if (!ready_ || !bytes || size == 0u) return false;
  const bool pe_sync = early_pe_publish_ && async_writes_enabled_ && HasPeSyncCommand(bytes, size);

  if (!async_writes_enabled_) {
    // Serialized compatibility path: preserve the immediate v161d semantics.
    if (!FlushBufferedWrites()) return false;
    return EnqueueBurst(bytes, size, guest_pc);
  }

  // v36: decouple the CPU/AOT producer from Aurora's FIFO parser.  NativeCP
  // often hands us one 32-byte line at a time, so queueing each line would just
  // move the per-call mutex overhead to another hot path.  Coalesce those lines
  // into the existing bounded host batch (default 512 B), then enqueue one
  // immutable span for the GX worker.  Architectural ordering points call
  // FlushWrites(), which publishes any partial batch and waits for completion.
  if (write_batch_size_ == 0u && size >= write_batch_limit_)
    return EnqueueBurst(bytes, size, guest_pc);

  const std::uint32_t limit =
      std::min<std::uint32_t>(write_batch_limit_, static_cast<std::uint32_t>(write_batch_.size()));

  // v184: NativeCP normally submits exactly one 32-byte gather line. When
  // that line fits wholly in the current host batch, append it directly and
  // avoid the generic split/min loop. Flush occurs at the exact same byte
  // boundary as before, so worker ordering and guest-visible FlushWrites()
  // semantics are unchanged.
  if (size == 32u && limit >= 32u && write_batch_size_ <= limit - 32u) {
    auto* destination = WriteBatchBuffer();
    if (!destination) return false;
    if (write_batch_size_ == 0u) write_batch_first_pc_ = guest_pc;
    std::memcpy(destination + write_batch_size_, bytes, 32u);
    write_batch_size_ += 32u;
    if (write_batch_size_ == limit || pe_sync) return FlushBufferedWrites();
    return true;
  }

  std::uint32_t offset = 0u;
  while (offset < size) {
    auto* destination = WriteBatchBuffer();
    if (!destination) return false;
    if (write_batch_size_ == 0u) write_batch_first_pc_ = guest_pc;
    const std::uint32_t room = limit - write_batch_size_;
    const std::uint32_t chunk = std::min<std::uint32_t>(room, size - offset);
    std::memcpy(destination + write_batch_size_, bytes + offset, chunk);
    write_batch_size_ += chunk;
    offset += chunk;
    if (write_batch_size_ == limit && !FlushBufferedWrites()) return false;
  }
  return !pe_sync || PublishPendingWrites();
}

bool HostBridge::StageArrayBinding(std::uint32_t index,std::uint32_t base,
                                  std::uint32_t stride,std::uint32_t before) {
  if(!ready_ || !HasTypedArrayCommands() || index>=16 || before>=32)return false;
  // Attach metadata to the existing batch. No extra publication, mutex, flush
  // or allocation per SDK call. Overflow/overlap simply uses the byte decoder.
  const auto offset=write_batch_size_+before;
  if(array_hint_count_==kArrayHints || (array_hint_count_ &&
      offset<array_hints_[array_hint_count_-1].offset+NativeSdkWireBytes(array_hints_[array_hint_count_-1])))return true;
  array_hints_[array_hint_count_++]={offset,NativeSdkKind::Array,index,base,stride};return true;
}

bool HostBridge::StageMatrix(std::uint32_t address,std::uint32_t words,std::uint32_t before) {
  if(!ready_ || !HasDirectMatrixCommands() || before>=32u || !NativeMatrixRange(address,words))return false;
  const auto offset=write_batch_size_+before;
  if(array_hint_count_==kArrayHints || (array_hint_count_ &&
      offset<array_hints_[array_hint_count_-1].offset+NativeSdkWireBytes(array_hints_[array_hint_count_-1])))return true;
  array_hints_[array_hint_count_++]={offset,NativeSdkKind::Matrix,address,words,0u};return true;
}

void HostBridge::TakeArrayHints(AsyncBurst& slot,std::uint32_t bytes) {
  slot.array_count=0;
  unsigned remaining=0;
  for(unsigned i=0;i<array_hint_count_;++i) {
    auto hint=array_hints_[i];
    if(hint.offset<bytes)slot.arrays[slot.array_count++]=hint;
    else {hint.offset-=bytes;array_hints_[remaining++]=hint;}
  }
  array_hint_count_=remaining;
}

bool HostBridge::SubmitAnnotatedBurst(const std::uint8_t* bytes,std::uint32_t size,
                                      std::uint32_t pc,const AsyncBurst& hints) {
  if(hints.array_count && sdk_commands_) {
    sdk_commands_(bytes,size,pc,hints.arrays.data(),hints.array_count);return true;
  }
  if(hints.array_count && command_burst_) {
    std::array<NativeArrayCommand,kArrayHints> old{};
    for(unsigned i=0;i<hints.array_count;++i) {
      const auto& hint=hints.arrays[i];
      if(hint.kind!=NativeSdkKind::Array)return false;
      old[i]={hint.offset,hint.a,hint.b,hint.c};
    }
    command_burst_(bytes,size,pc,old.data(),hints.array_count);return true;
  }
  std::uint32_t offset=0;
  for(unsigned i=0;i<hints.array_count;++i) {
    const auto& hint=hints.arrays[i];
    if(hint.offset>offset && !SubmitBurstImmediate(bytes+offset,hint.offset-offset,pc))return false;
    if(hint.kind!=NativeSdkKind::Array)return false;
    sdk_array_(hint.a,hint.b,hint.c,0);
    offset=hint.offset;
  }
  return offset==size || SubmitBurstImmediate(bytes+offset,size-offset,pc);
}

void HostBridge::NotifyCacheControl(std::uint8_t operation, std::uint32_t address) {
  if (!ready_) return;
  if (async_writes_enabled_ && async_commands_ && memory_write_) {
    // dcbst/dcbf/dcbi publish one 32-byte line (other operations are ignored by
    // the renderer). Queue it in FIFO order instead of draining the worker.
    if (operation <= 2u) (void)EnqueueMemoryWrite(address & ~31u, 32u);
    return;
  }
  (void)FlushWrites(SyncReason::CacheControl);
  if (cache_control_) cache_control_(operation, address);
}

void HostBridge::NotifyMemoryWrite(std::uint32_t address, std::uint32_t size) {
  if (!ready_ || size == 0u) return;
  if (async_writes_enabled_ && async_commands_ && memory_write_) {
    // v300: RAM is host-coherent; the notification only invalidates renderer
    // caches. Executing it on the GX worker in FIFO order gives draws issued
    // before the publication the same view as the old drain-then-invalidate
    // path, and every later draw the new bytes, without stalling the guest.
    (void)EnqueueMemoryWrite(address, size);
    return;
  }
  (void)FlushWrites(SyncReason::MemoryWrite);
  if (memory_write_) memory_write_(address, size);
}

bool HostBridge::PeekEfbZ(std::uint16_t x, std::uint16_t y, std::uint32_t* z) {
  if (!ready_ || !peek_efb_z_ || !z) return false;
  (void)FlushWrites(SyncReason::Efb);
  return peek_efb_z_(x, y, z);
}

bool HostBridge::PeekEfbArgb(std::uint16_t x, std::uint16_t y, std::uint32_t* argb) {
  if (!ready_ || !peek_efb_argb_ || !argb) return false;
  (void)FlushWrites(SyncReason::Efb);
  return peek_efb_argb_(x, y, argb);
}

bool HostBridge::ReadBoundingBox(std::array<std::uint16_t, 4>* bounds) {
  if (!ready_ || !read_bbox_ || !bounds) return false;
  if (!FlushWrites(SyncReason::Efb)) return false;
  return read_bbox_(bounds->data());
}

bool HostBridge::PokeEfb(std::uint16_t x, std::uint16_t y, std::uint32_t value,
                         bool depth) {
  if (!ready_ || !poke_efb_ || !FlushWrites(SyncReason::Efb)) return false;
  return poke_efb_(x, y, value, depth);
}

void HostBridge::Present() {
  if (!ready_ || !present_) return;
  (void)FlushWrites();
  present_();
  if (should_quit_ && should_quit_()) quit_requested_ = true;
}

void HostBridge::PresentNow(std::uint32_t xfb_top, std::uint32_t xfb_bottom,
                            std::uint32_t width, std::uint32_t stride_bytes,
                            std::uint32_t field_height) {
  if (!ready_ || !present_xfb_) return;
  if (async_writes_enabled_ && async_commands_ && present_xfb_async_) {
    // v300: the scanout lookup/submission runs on the GX worker right after
    // every GX command issued before this VI field. Window events still have
    // to be pumped here, and their handlers may resize renderer targets, so
    // poll them only while the worker is idle (free) or force one ordered
    // drain after kForcedEventPollPresents fields without an idle point.
    const bool idle = write_batch_size_ == 0u &&
                      async_pending_bursts_.load(std::memory_order_acquire) == 0u;
    if (idle || ++presents_since_poll_ >= kForcedEventPollPresents) {
      (void)FlushWrites(SyncReason::Present);
      presents_since_poll_ = 0u;
      if (should_quit_ && should_quit_()) quit_requested_ = true;
    }
    // present_xfb_ex only receives VI geometry when all of it is known.
    const bool geometry = width != 0u && stride_bytes != 0u && field_height != 0u;
    (void)EnqueuePresentXfb(xfb_top, xfb_bottom, geometry ? width : 0u,
                            geometry ? stride_bytes : 0u, geometry ? field_height : 0u);
    return;
  }
  (void)FlushWrites(SyncReason::Present);
  if (present_xfb_ex_ && width != 0u && stride_bytes != 0u && field_height != 0u)
    present_xfb_ex_(xfb_top, xfb_bottom, width, stride_bytes, field_height);
  else
    present_xfb_(xfb_top, xfb_bottom);
  if (should_quit_ && should_quit_()) quit_requested_ = true;
}

bool HostBridge::ConsumeFrameReady(std::uint32_t* xfb_address) {
  if (!ready_ || !consume_frame_ready_) return false;
  if (consume_frame_ready_async_) {
    // v300: the mailbox is an atomic exchange written by the GX worker. Unless
    // frame interpolation must read parser-owned history, polling it needs no
    // fence; draining here made every hardware tick wait for the whole queue.
    if (async_writes_enabled_ && async_commands_ && !frame_ready_fence_) {
      // Optional hardware-tick publication of the partial WGPIPE batch (A/B);
      // the idle path and every ordering point publish it anyway.
      if (tick_publish_ && !PublishPendingWrites()) return false;
      return consume_frame_ready_async_(xfb_address);
    }
    // Keep the previous FIFO visibility fence whenever commands remain. Only
    // a queue known to be idle may bypass the mutex/drain path: guest RAM reuse
    // and publication retain the same ordering as before this optimization.
    if (write_batch_size_ != 0u || async_pending_bursts_.load(std::memory_order_acquire) != 0u)
      if (!FlushWrites(SyncReason::FrameReady)) return false;
    if (!consume_frame_ready_async_(xfb_address)) return false;
    // The mailbox is safe to poll without draining at every hardware tick.
    // Interpolation/history and other parser-owned state still becomes visible
    // at an actual producer boundary through the existing queue fence.
    (void)FlushWrites(SyncReason::FrameReady);
    return true;
  }
  (void)FlushWrites(SyncReason::FrameReady);
  return consume_frame_ready_(xfb_address);
}

bool HostBridge::FrameInterpolationReady() const {
  return ready_ && interpolation_ready_ && interpolation_ready_();
}

bool HostBridge::PresentInterpolated(float alpha) {
  if (!ready_ || !present_interpolated_) return false;
  const bool presented = present_interpolated_(std::clamp(alpha, 0.0f, 1.0f));
  if (should_quit_ && should_quit_()) quit_requested_ = true;
  return presented;
}

void HostBridge::SetAspectMode(const std::string& mode) {
  if (!ready_ || !set_aspect_mode_) return;
  // v128b exposes only exact 4:3 and live Stretched-to-Window. Accept the old
  // fixed-wide strings as stretch so existing settings migrate harmlessly.
  const bool stretch = mode == "stretch" || mode == "16:9" || mode == "16:10" ||
                       mode == "21:9" || mode == "32:9";
  widescreen_active_ = stretch;
  set_aspect_mode_(stretch ? 1u : 0u);
}

float HostBridge::WidescreenTargetAspect() const {
  constexpr float native_aspect = 4.0f / 3.0f;
  if (!ready_ || !widescreen_active_ || !target_aspect_) return native_aspect;
  const float aspect = target_aspect_();
  return std::isfinite(aspect) && aspect >= 0.5f && aspect <= 5.0f
             ? aspect : native_aspect;
}

bool HostBridge::CurrentPerspectiveCamera(float* fov_y_degrees, float* guest_aspect) const {
  if (!ready_ || !camera_signature_ || !fov_y_degrees || !guest_aspect) return false;
  float fov = 0.0f;
  float aspect = 0.0f;
  if (!camera_signature_(&fov, &aspect) || !std::isfinite(fov) ||
      !std::isfinite(aspect) || fov < 20.0f || fov > 150.0f ||
      aspect < 0.5f || aspect > 5.0f)
    return false;
  *fov_y_degrees = fov;
  *guest_aspect = aspect;
  return true;
}

void HostBridge::AdvanceCycles(std::uint64_t cycles) {
  if (!cycle_present_enabled_ || !ready_ || present_cycles_ == 0 || cycles == 0) return;
  if (cycles > std::numeric_limits<std::uint64_t>::max() - pending_cycles_)
    pending_cycles_ = present_cycles_;
  else
    pending_cycles_ += cycles;

  // A breakpoint or one unusually large dispatcher charge must not cause an
  // unbounded burst of stale presents. Keep at most four catch-up fields.
  unsigned catch_up = 0;
  while (pending_cycles_ >= present_cycles_ && catch_up < 4) {
    pending_cycles_ -= present_cycles_;
    Present();
    ++catch_up;
  }
  if (catch_up == 4 && pending_cycles_ >= present_cycles_)
    pending_cycles_ %= present_cycles_;
}

} // namespace GekkoAOT::GX
