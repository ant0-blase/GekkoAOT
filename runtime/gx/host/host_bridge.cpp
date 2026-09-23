// SPDX-License-Identifier: GPL-3.0-or-later
#include "gx/host/host_bridge.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <limits>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace GekkoAOT::GX {
namespace {

constexpr std::uint32_t kWriteGatherPipePhysical = 0x0c008000u;
constexpr std::uint32_t kWriteGatherPipeMask = 0x3fffffe0u;

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

bool HostBridge::IsWriteGatherPipe(std::uint32_t address) {
  return (address & kWriteGatherPipeMask) == kWriteGatherPipePhysical;
}

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
  present_ = reinterpret_cast<PresentFn>(FindSymbol("gekkoaot_native_gx_present"));
  present_xfb_ = reinterpret_cast<PresentXfbFn>(FindSymbol("gekkoaot_native_gx_present_xfb"));
  present_xfb_ex_ = reinterpret_cast<PresentXfbExFn>(
      FindSymbol("gekkoaot_native_gx_present_xfb_ex"));
  // Optional: older NativeGX plugins remain valid and simply keep VI-driven
  // presentation. No hard ABI bump is required for this experiment.
  consume_frame_ready_ = reinterpret_cast<ConsumeFrameReadyFn>(
      FindSymbol("gekkoaot_native_gx_consume_frame_ready"));
  present_interpolated_ = reinterpret_cast<PresentInterpolatedFn>(
      FindSymbol("gekkoaot_native_gx_present_interpolated"));
  interpolation_ready_ = reinterpret_cast<InterpolationReadyFn>(
      FindSymbol("gekkoaot_native_gx_frame_interpolation_ready"));
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
  quit_requested_ = false;
  cycle_present_enabled_ = true;
  pe_finish_seen_.store(false, std::memory_order_relaxed);
  pe_token_.store(0, std::memory_order_relaxed);
  pe_token_seen_.store(false, std::memory_order_relaxed);
  pe_token_interrupt_seen_.store(false, std::memory_order_relaxed);

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
  const bool was_ready = ready_;
  const bool keep_mapped = was_ready && pin_library_on_close_ && library_;
  if (ready_ && set_pe_callback_) set_pe_callback_(nullptr, nullptr);
  if (ready_ && shutdown_) shutdown_();
  ready_ = false;
  memory_ = nullptr;
  init_ = nullptr;
  write_ = nullptr;
  write_burst_ = nullptr;
  present_ = nullptr;
  present_xfb_ = nullptr;
  present_xfb_ex_ = nullptr;
  consume_frame_ready_ = nullptr;
  present_interpolated_ = nullptr;
  interpolation_ready_ = nullptr;
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
  // Fallback for hosts that use the plugin ABI without installing the direct
  // sink. Standalone GekkoAOT installs one, so this atomic mailbox is cold.
  switch (reg) {
  case 0x45u: // BPMEM_SETDRAWDONE
    if ((value & 0xffu) == 0x02u)
      self->pe_finish_seen_.store(true, std::memory_order_relaxed);
    break;
  case 0x47u: // BPMEM_PE_TOKEN_ID
    self->pe_token_.store(static_cast<std::uint16_t>(value), std::memory_order_relaxed);
    self->pe_token_seen_.store(true, std::memory_order_relaxed);
    break;
  case 0x48u: // BPMEM_PE_TOKEN_INT_ID
    self->pe_token_.store(static_cast<std::uint16_t>(value), std::memory_order_relaxed);
    self->pe_token_seen_.store(true, std::memory_order_relaxed);
    self->pe_token_interrupt_seen_.store(true, std::memory_order_relaxed);
    break;
  default:
    break;
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

bool HostBridge::WriteBurst(const std::uint8_t* bytes, std::uint32_t size,
                            std::uint32_t guest_pc) {
  if (!ready_ || !bytes || size == 0u) return false;
  if (write_burst_) {
    write_burst_(bytes, size, guest_pc);
    return true;
  }
  if (!write_) return false;
  // Compatibility with pre-burst NativeGX DSOs. Preserve stream ordering
  // exactly; this path disappears as soon as the new optional symbol exists.
  for (std::uint32_t i = 0; i < size; ++i) write_(bytes[i], 1u, guest_pc);
  return true;
}

void HostBridge::Present() {
  if (!ready_ || !present_) return;
  present_();
  if (should_quit_ && should_quit_()) quit_requested_ = true;
}

void HostBridge::PresentNow(std::uint32_t xfb_top, std::uint32_t xfb_bottom,
                            std::uint32_t width, std::uint32_t stride_bytes,
                            std::uint32_t field_height) {
  if (!ready_ || !present_xfb_) return;
  if (present_xfb_ex_ && width != 0u && stride_bytes != 0u && field_height != 0u)
    present_xfb_ex_(xfb_top, xfb_bottom, width, stride_bytes, field_height);
  else
    present_xfb_(xfb_top, xfb_bottom);
  if (should_quit_ && should_quit_()) quit_requested_ = true;
}

bool HostBridge::ConsumeFrameReady(std::uint32_t* xfb_address) {
  if (!ready_ || !consume_frame_ready_) return false;
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
