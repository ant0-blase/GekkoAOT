// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
#include "platform/sdl_input.h"
#endif

#include "core/time.h"
#include "video/native_vp6.h"

#ifdef GEKKOAOT_HAVE_NOD
#include <nod.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <thread>
#include <system_error>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#include <x86intrin.h>
#endif

namespace GekkoAOT::Native {

namespace {
constexpr std::uint32_t kNativeRegionProbe = 0xfffffffcu;
constexpr std::uint32_t kRelAddressProbe = 0xfffffffau;
constexpr std::uint32_t kGlobalIndirectProbe = 0xfffffff9u;
// v70 compile-time SDK lowering.  DolRecomp emits these synthetic host-call
// tokens only for entrypoints proven by the immutable build-time SDK manifest.
// They bypass guest-address lookup while keeping the same native service ABI.
constexpr std::uint32_t kStaticDirectOsBase = 0xfffd0000u;
constexpr std::uint32_t kStaticDirectSimpleBase = 0xfffc0000u;
constexpr std::uint32_t kStaticDirectMask = 0xffffff00u;
constexpr std::uint8_t kRelAddressHandled = 0xfeu;
constexpr std::uint32_t kPpcPrimaryX = 31u;
constexpr std::uint32_t kGekkoPvr = 0x00083214u;
constexpr std::uint32_t kInitialHid0 = 0x0011c464u;
constexpr std::uint16_t kSprDec = 22u;
constexpr std::uint16_t kSprPvr = 287u;
constexpr std::uint16_t kSprHid0 = 1008u;
constexpr std::uint16_t kSprHid1 = 1009u;
constexpr std::uint16_t kSprIbat0u = 528u;
constexpr std::uint16_t kSprIbat0l = 529u;
constexpr std::uint16_t kSprDbat0u = 536u;
constexpr std::uint16_t kSprDbat0l = 537u;
constexpr std::uint16_t kSprDbat1u = 538u;
constexpr std::uint16_t kSprDbat1l = 539u;
// HID0.ICFI is a command bit, not persistent state. Writing it requests an
// instruction-cache invalidate and the bit reads back clear once accepted.
constexpr std::uint32_t kHid0IcfiMask = 1u << 11;
constexpr std::uint32_t kHid0IceMask = 1u << 15;
constexpr std::uint32_t kTimerRatio = 12u;
constexpr std::uint16_t kSprDmau = 922u;
constexpr std::uint16_t kSprDmal = 923u;
constexpr std::uint32_t kDmalTransfer = 0x00000002u;
constexpr std::uint32_t kDmalLoad = 0x00000010u;

struct FastTscInfo {
  bool available = false;
  std::uint64_t hz = 0;
};

FastTscInfo DetectFastInvariantTsc() {
#if defined(_M_X64) || defined(_M_IX86) || defined(__i386__) || defined(__x86_64__)
  unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
#if defined(_MSC_VER)
  int regs[4]{};
  __cpuid(regs, static_cast<int>(0x80000000u));
  const unsigned max_ext = static_cast<unsigned>(regs[0]);
  if (max_ext < 0x80000007u) return {};
  __cpuid(regs, static_cast<int>(0x80000007u));
  if ((static_cast<unsigned>(regs[3]) & (1u << 8)) == 0u) return {};
  __cpuid(regs, 0);
  const unsigned max_basic = static_cast<unsigned>(regs[0]);
  if (max_basic < 0x15u) return {};
  __cpuidex(regs, 0x15, 0);
  eax = static_cast<unsigned>(regs[0]);
  ebx = static_cast<unsigned>(regs[1]);
  ecx = static_cast<unsigned>(regs[2]);
#else
  const unsigned max_ext = __get_cpuid_max(0x80000000u, nullptr);
  if (max_ext < 0x80000007u ||
      !__get_cpuid(0x80000007u, &eax, &ebx, &ecx, &edx) ||
      (edx & (1u << 8)) == 0u)
    return {};
  const unsigned max_basic = __get_cpuid_max(0u, nullptr);
  if (max_basic < 0x15u) return {};
  __cpuid_count(0x15u, 0u, eax, ebx, ecx, edx);
#endif
  // CPUID.15H is the architectural conversion ratio from invariant TSC ticks
  // to the crystal clock. Only use it when the crystal frequency is supplied:
  // that path is exact enough to be a realtime clock. Otherwise retain
  // std::chrono rather than guessing from nominal/base CPU MHz.
  if (eax == 0u || ebx == 0u || ecx == 0u) return {};
  const std::uint64_t hz =
      (static_cast<std::uint64_t>(ecx) * static_cast<std::uint64_t>(ebx)) /
      static_cast<std::uint64_t>(eax);
  if (hz < 100000000ull || hz > 6000000000ull) return {};
  return {true, hz};
#else
  return {};
#endif
}

inline std::uint64_t ReadFastTsc() {
#if defined(_M_X64) || defined(_M_IX86) || defined(__i386__) || defined(__x86_64__)
  return static_cast<std::uint64_t>(__rdtsc());
#else
  return 0u;
#endif
}

bool RuntimeEnvBool(const char* name, bool fallback) {
  const char* text = std::getenv(name);
  if (!text || !*text) return fallback;
  return std::strcmp(text, "0") != 0 && std::strcmp(text, "false") != 0 &&
         std::strcmp(text, "off") != 0 && std::strcmp(text, "no") != 0;
}

std::uint32_t RuntimeEnvU32(const char* name, std::uint32_t fallback,
                            std::uint32_t maximum) {
  const char* text = std::getenv(name);
  if (!text || !*text) return fallback;
  char* end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);
  if (!end || end == text || *end != '\0' || value > maximum) return fallback;
  return static_cast<std::uint32_t>(value);
}

std::int64_t NativeChainCycleBudget() {
  // Keep pure AOT execution inside the game module across ordinary function
  // returns/budget exits.  MMIO/HLE is an explicit barrier (see the host event
  // epochs below), so the larger horizon only coalesces CPU/MEM1-heavy work.
  constexpr std::uint64_t kDefault = 262144u;
  constexpr std::uint64_t kMaximum = 4194304u;
  const char* text = std::getenv("GEKKOAOT_NATIVE_CHAIN_CYCLES");
  if (!text || !*text) return static_cast<std::int64_t>(kDefault);
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (!end || end == text || *end != '\0' || value > kMaximum)
    return static_cast<std::int64_t>(kDefault);
  return static_cast<std::int64_t>(value);
}


std::int64_t SafeNativeChainCycleBudget() {
  // Compatibility/GDB mode used to disable native chaining completely. That
  // guarantees a host boundary after every generated function, but profiles
  // show the boundary machinery itself can consume most of one CPU thread.
  // Keep the starvation fix while coalescing tiny calls. v136b raises the
  // conservative GUI-safe horizon to 16384 guest cycles (~33.7 us at 486 MHz):
  // still far below a frame/VI timescale, while reducing HostRuntime/module ABI
  // crossings on draw-heavy titles. MMIO/HLE/exceptions remain immediate
  // barriers through the host-event epochs in module_dispatch().
  // v153: correctness-first default.  A value of zero returns to HostRuntime
  // after every AOT dispatch so PI/VI/DEC and other asynchronous events are
  // observed at a deterministic host boundary.  Users may opt back into a
  // bounded chain through GEKKOAOT_SAFE_CHAIN_CYCLES.
  constexpr std::uint64_t kDefault = 0u;
  constexpr std::uint64_t kMaximum = 65536u;
  const char* text = std::getenv("GEKKOAOT_SAFE_CHAIN_CYCLES");
  if (!text || !*text) return static_cast<std::int64_t>(kDefault);
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text, &end, 10);
  if (!end || end == text || *end != '\0' || value > kMaximum)
    return static_cast<std::int64_t>(kDefault);
  return static_cast<std::int64_t>(value);
}

inline void BumpHostReadEpoch(CPUState* cpu) {
  if (cpu && cpu->external_read_count != 0xffu) ++cpu->external_read_count;
}

inline void BumpHostWriteEpoch(CPUState* cpu) {
  if (cpu && cpu->external_write_count != 0xffu) ++cpu->external_write_count;
}

std::uint32_t ReadGuestBE32(const std::uint8_t* bytes) {
  return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) |
         (std::uint32_t(bytes[2]) << 8) | std::uint32_t(bytes[3]);
}

bool AddressInRanges(const GekkoAOTRange* ranges, std::uint32_t count, std::uint32_t address) {
  for (std::uint32_t i = 0; i < count; ++i)
    if (address >= ranges[i].start && address < ranges[i].end) return true;
  return false;
}


bool FindModuleChunk(const GekkoAOTModuleDesc* descriptor, std::uint32_t address,
                     std::uint32_t* index) {
  if (!descriptor || !descriptor->chunk_ranges || descriptor->num_chunk_ranges == 0u)
    return false;
  std::uint32_t lo = 0u;
  std::uint32_t hi = descriptor->num_chunk_ranges;
  while (lo < hi) {
    const std::uint32_t mid = lo + (hi - lo) / 2u;
    const auto range = descriptor->chunk_ranges[mid];
    if (address < range.start)
      hi = mid;
    else if (address >= range.end)
      lo = mid + 1u;
    else {
      if (index) *index = mid;
      return true;
    }
  }
  return false;
}

std::uint64_t Fnv1a64(const std::uint8_t* data, std::size_t size) {
  std::uint64_t hash = 0xcbf29ce484222325ull;
  for (std::size_t i = 0; i < size; ++i)
    hash = (hash ^ data[i]) * 0x100000001b3ull;
  return hash;
}

#ifdef GEKKOAOT_HAVE_NOD
const char* NodFormatName(NodFormat format) {
  switch (format) {
  case NOD_FORMAT_ISO: return "ISO";
  case NOD_FORMAT_CISO: return "CISO";
  case NOD_FORMAT_GCZ: return "GCZ";
  case NOD_FORMAT_NFS: return "NFS";
  case NOD_FORMAT_RVZ: return "RVZ";
  case NOD_FORMAT_WBFS: return "WBFS";
  case NOD_FORMAT_WIA: return "WIA";
  case NOD_FORMAT_TGC: return "TGC";
  }
  return "unknown";
}
#endif

// Instructions written by the direct-boot low-memory bootstrap and by the
// Dolphin OS exception setup live outside the DOL's static code ranges.  They
// therefore need a tiny standalone execution path instead of being mistaken
// for missing AOT functions.
constexpr std::uint32_t kPpcRfi = 0x4c000064u;
constexpr std::uint32_t kPpcNop = 0x60000000u;
constexpr std::uint32_t kPpcMsrRfiMask = 0x87c0ffffu;
constexpr std::uint32_t kPpcMsrPow = 0x00040000u;
constexpr std::uint32_t kPpcMsrIle = 0x00010000u;
constexpr std::uint32_t kPpcMsrEe = 0x00008000u;
constexpr std::uint32_t kPpcMsrPr = 0x00004000u;
constexpr std::uint32_t kPpcMsrLe = 0x00000001u;
constexpr std::uint32_t kPpcExceptionEntryClearMask = 0x0004ef36u;
constexpr std::uint32_t kPpcExcProgram = 0x00000001u;
constexpr std::uint32_t kPpcExcDsi = 0x00000002u;
constexpr std::uint32_t kPpcExcAlignment = 0x00000004u;
constexpr std::uint32_t kPpcExcSystemCall = 0x00000008u;
constexpr std::uint32_t kPpcExcMachineCheck = 0x00000010u;
constexpr std::uint32_t kPpcExcFpUnavailable = 0x00000020u;
constexpr std::uint32_t kExceptionVectorSpan = 0x100u;

bool IsLowExceptionVectorAddress(std::uint32_t physical) {
  static constexpr std::uint32_t kVectors[] = {
      0x0100u, 0x0200u, 0x0300u, 0x0400u, 0x0500u, 0x0600u, 0x0700u, 0x0800u,
      0x0900u, 0x0c00u, 0x0d00u, 0x0f00u, 0x1300u, 0x1400u, 0x1700u};
  for (const std::uint32_t vector : kVectors)
    if (physical >= vector && physical < vector + kExceptionVectorSpan) return true;
  return false;
}

void AcknowledgeSynchronousExceptionVector(CPUState* cpu, std::uint32_t physical) {
  if (!cpu || (physical & (kExceptionVectorSpan - 1u)) != 0u) return;

  // CPUState::exception is a host-side pending/unwind latch, not an
  // architectural PPC register. DolRecomp sets a bit when taking a
  // synchronous exception so generated code unwinds to the execution chassis.
  // Once the chassis has actually entered the corresponding low-memory vector,
  // that pending bit must be consumed. Leaving it set makes helpers such as
  // mftb immediately return after their `if (ctx->exception)` guard forever.
  switch (physical) {
  case 0x0200u: cpu->exception &= ~kPpcExcMachineCheck; break;
  case 0x0300u: cpu->exception &= ~kPpcExcDsi; break;
  case 0x0600u: cpu->exception &= ~kPpcExcAlignment; break;
  case 0x0700u:
    cpu->exception &= ~kPpcExcProgram;
    cpu->program_exception = 0u;
    break;
  case 0x0800u: cpu->exception &= ~kPpcExcFpUnavailable; break;
  case 0x0c00u: cpu->exception &= ~kPpcExcSystemCall; break;
  default: break;
  }
}

enum : std::uint8_t {
  kCacheDcbst = 0,
  kCacheDcbf = 1,
  kCacheDcbi = 2,
  kCacheIcbi = 3,
};

bool DecodeCacheControl(std::uint32_t raw, CPUState* cpu, std::uint8_t* operation,
                        std::uint32_t* effective_address) {
  if (!cpu || !operation || !effective_address || (raw >> 26) != kPpcPrimaryX) return false;

  const std::uint32_t xo = (raw >> 1) & 0x3ffu;
  switch (xo) {
  case 54u: *operation = kCacheDcbst; break;
  case 86u: *operation = kCacheDcbf; break;
  case 470u: *operation = kCacheDcbi; break;
  case 982u: *operation = kCacheIcbi; break;
  default: return false;
  }

  const std::uint32_t ra = (raw >> 16) & 31u;
  const std::uint32_t rb = (raw >> 11) & 31u;
  const std::uint32_t base = ra == 0u ? 0u : cpu->gpr[ra];
  *effective_address = base + cpu->gpr[rb];
  return true;
}

bool DecodeSprTransfer(std::uint32_t raw, bool* write, std::uint16_t* spr,
                       std::uint8_t* gpr) {
  if (!write || !spr || !gpr || (raw >> 26) != kPpcPrimaryX) return false;
  const std::uint32_t xo = (raw >> 1) & 0x3ffu;
  if (xo != 339u && xo != 467u) return false; // MFSPR / MTSPR

  const std::uint32_t encoded = (raw >> 11) & 0x3ffu;
  *spr = static_cast<std::uint16_t>(((encoded & 31u) << 5) | ((encoded >> 5) & 31u));
  *gpr = static_cast<std::uint8_t>((raw >> 21) & 31u);
  *write = xo == 467u;
  return true;
}

bool IsHostStoredSpr(std::uint16_t spr) {
  switch (spr) {
  case 22u:
  case 25u:
  case 272u:
  case 273u:
  case 274u:
  case 275u:
  case 287u:
  case 921u:
  case 922u:
  case 923u:
  case 936u:
  case 937u:
  case 938u:
  case 939u:
  case 940u:
  case 941u:
  case 942u:
  case 1008u:
  case 1009u:
  case 1010u:
  case 1013u:
  case 1017u:
  case 1019u:
  case 1020u:
  case 1021u:
  case 1022u:
    return true;
  default:
    break;
  }
  return (spr >= 528u && spr <= 543u) || (spr >= 952u && spr <= 958u);
}

bool IsHostWritableSpr(std::uint16_t spr) {
  return spr != kSprPvr && IsHostStoredSpr(spr);
}

void TraceSprReadV90(std::uint16_t spr, std::uint32_t address, std::uint32_t lr,
                     std::uint32_t value) {
  static unsigned logs = 0u;
  if (logs >= 128u) return;
  // A repeated SPR poll can execute millions of times. Log the first few hits
  // for a stable (SPR, PC) pair, then only powers of two.
  static std::uint16_t last_spr = 0xffffu;
  static std::uint32_t last_address = 0xffffffffu;
  static std::uint64_t repeat_count = 0u;
  if (spr != last_spr || address != last_address) {
    last_spr = spr;
    last_address = address;
    repeat_count = 0u;
  }
  const std::uint64_t count = ++repeat_count;
  if (count <= 8u || (count & (count - 1u)) == 0u) {
    ++logs;
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_SPR_TRACE_V90=1 op=read spr=%u pc=%08x lr=%08x value=%08x repeat=%llu\n",
                 unsigned(spr), address, lr, value,
                 static_cast<unsigned long long>(count));
  }
}

std::uint32_t RotateLeft32(std::uint32_t value, unsigned shift) {
  shift &= 31u;
  if (shift == 0u) return value;
  return (value << shift) | (value >> (32u - shift));
}

std::uint32_t RotationMask(unsigned mb, unsigned me) {
  const std::uint32_t begin = 0xffffffffu >> (mb & 31u);
  const std::uint32_t end = 0x7fffffffu >> (me & 31u);
  const std::uint32_t mask = begin ^ end;
  return me < mb ? ~mask : mask;
}

void SetCr0FromResult(CPUState* cpu, std::uint32_t value) {
  if (!cpu) return;
  std::uint32_t field = 0u;
  const auto signed_value = static_cast<std::int32_t>(value);
  if (signed_value < 0) field |= 0x8u;
  else if (signed_value > 0) field |= 0x4u;
  else field |= 0x2u;
  field |= (cpu->xer >> 31) & 1u;
  cpu->cr = (cpu->cr & 0x0fffffffu) | (field << 28);
}

bool ReadAddressSpaceValue(const AddressSpace& memory, std::uint32_t address,
                           std::uint8_t size, std::uint64_t* out) {
  if (!out) return false;
  switch (size) {
  case 1u: {
    std::uint8_t value = 0;
    if (!memory.Read8(address, &value)) return false;
    *out = value;
    return true;
  }
  case 2u: {
    std::uint16_t value = 0;
    if (!memory.Read16(address, &value)) return false;
    *out = value;
    return true;
  }
  case 4u: {
    std::uint32_t value = 0;
    if (!memory.Read32(address, &value)) return false;
    *out = value;
    return true;
  }
  case 8u: {
    std::uint64_t value = 0;
    if (!memory.Read64(address, &value)) return false;
    *out = value;
    return true;
  }
  default: return false;
  }
}

bool WriteAddressSpaceValue(AddressSpace& memory, std::uint32_t address,
                            std::uint64_t value, std::uint8_t size) {
  switch (size) {
  case 1u: return memory.Write8(address, static_cast<std::uint8_t>(value));
  case 2u: return memory.Write16(address, static_cast<std::uint16_t>(value));
  case 4u: return memory.Write32(address, static_cast<std::uint32_t>(value));
  case 8u: return memory.Write64(address, value);
  default: return false;
  }
}

bool LooksLikeInvalidGameCubeRamAddress(const AddressSpace& memory,
                                           std::uint32_t address) {
  // FakeVMEM is a deliberate v77 compatibility aperture and has already been
  // offered to AddressSpace above. Keep it out of the invalid-RAM classifier.
  if (address >= AddressSpace::FakeVmemBase &&
      address < AddressSpace::FakeVmemBase + AddressSpace::RetailFakeVmemSize)
    return false;

  const std::uint32_t physical = AddressSpace::ToPhysical(address);
  // Retail GameCube has no MEM2. A cached pointer resolving into the Wii MEM2
  // physical aperture is therefore a corrupted/foreign guest pointer, not an
  // MMIO register that should be implemented.
  if (physical >= AddressSpace::Mem2PhysicalBase) {
    const std::uint32_t offset = physical - AddressSpace::Mem2PhysicalBase;
    return offset >= memory.Mem2().size();
  }

  // The compatibility backing covers the full 32 MiB GC physical RAM aperture
  // while lowmem still reports retail 24 MiB. 0x08000000 and above contains
  // hardware apertures such as EFB/MMIO aliases, so only classify the gap below
  // that boundary as an out-of-range RAM pointer. Device handlers still get
  // first refusal before this diagnostic is used.
  return physical >= memory.Mem1().size() && physical < 0x08000000u;
}

bool CrBit(const CPUState* cpu, unsigned bit) {
  if (!cpu || bit >= 32u) return false;
  return ((cpu->cr >> (31u - bit)) & 1u) != 0u;
}
}

const char* RunStatusName(RunStatus status) {
  switch (status) {
  case RunStatus::Completed: return "completed";
  case RunStatus::DispatchLimitReached: return "dispatch limit reached";
  case RunStatus::GuestHalted: return "guest halt";
  case RunStatus::UncompiledAddress: return "uncompiled address";
  case RunStatus::InstructionFallback: return "instruction fallback required";
  case RunStatus::UnhandledMmioRead: return "unhandled MMIO read";
  case RunStatus::UnhandledMmioWrite: return "unhandled MMIO write";
  case RunStatus::UnhandledSprRead: return "unhandled SPR read";
  case RunStatus::UnhandledSprWrite: return "unhandled SPR write";
  case RunStatus::NativeHleFailure: return "native HLE failure";
  case RunStatus::HostRequestedExit: return "host requested exit";
  case RunStatus::ModuleNotLoaded: return "module not loaded";
  case RunStatus::InvalidGuestMemoryRead: return "invalid guest memory read";
  case RunStatus::InvalidGuestMemoryWrite: return "invalid guest memory write";
  }
  return "unknown";
}

HostRuntime::HostRuntime(bool enable_mem2) : memory_(enable_mem2), dsp_(&memory_) {
  compat_diagnostics_enabled_ = RuntimeEnvBool("GEKKOAOT_COMPAT_DIAGNOSTICS", false);
  std::fprintf(stderr,
               "GEKKOAOT_MEMORY_BOUNDS_V143=1 mem1-reported=%u mem1-backing=%u backing-end=%08x fakevmem=%08x+%u mem2=%u policy=dolphin-compatible-tail\n",
               AddressSpace::ReportedMem1Size(),
               static_cast<unsigned>(memory_.Mem1().size()),
               0x80000000u + static_cast<unsigned>(memory_.Mem1().size()),
               AddressSpace::FakeVmemBase, AddressSpace::RetailFakeVmemSize,
               static_cast<unsigned>(memory_.Mem2().size()));
  safe_host_boundaries_enabled_ = RuntimeEnvBool("GEKKOAOT_SAFE_HOST_BOUNDARIES", true);
  compat_break_on_fault_ = RuntimeEnvBool("GEKKOAOT_COMPAT_BREAK_ON_FAULT", false);

  // v47: AOT execution itself is never throttled. The GameCube-visible clock
  // domain comes from steady_clock, while presentation has an independent cap.
  host_fps_limit_hz_ = RuntimeEnvU32("GEKKOAOT_HOST_FPS_LIMIT", 120u, 1000u);
  if (host_fps_limit_hz_ != 0u && host_fps_limit_hz_ < 30u) host_fps_limit_hz_ = 30u;
  frame_interpolation_enabled_ = RuntimeEnvBool("GEKKOAOT_FRAME_INTERPOLATION", true);
  perf_metrics_enabled_ = RuntimeEnvBool("GEKKOAOT_PERF_METRICS", true);
  cp_idle_breakpoint_recovery_enabled_ =
      RuntimeEnvBool("GEKKOAOT_CP_IDLE_BREAKPOINT_RECOVERY", true);
  if (const char* user_dir = std::getenv("GEKKOAOT_USER_DIR"); user_dir && *user_dir)
    live_video_config_path_ = std::filesystem::path(user_dir) / "config.ini";
  Reset();
  if (compat_diagnostics_enabled_) {
    std::fprintf(stderr,
                 "GEKKOAOT_COMPAT_DIAGNOSTICS_V1=1 chain=single-dispatch breadcrumbs=%zu break_on_fault=%u\n",
                 kCompatBreadcrumbCount, compat_break_on_fault_ ? 1u : 0u);
  }
  std::fprintf(stderr,
               "GEKKOAOT_CLOCK_DOMAINS_V49=1 cpu=aot-uncapped hardware=host-realtime vi=1x tb=1x dec=1x dsp=1x ai=1x catchup=full-under-250ms\n");
  std::fprintf(stderr,
               "GEKKOAOT_BOOT_PARITY_V143=1 hid0=0011c464 hid1=80000000 hid2=e0000000 ibat0=80001fff/00000002 dbat0=80001fff/00000002 dbat1=c0001fff/0000002a\n");
  std::fprintf(stderr,
               "GEKKOAOT_LOW_ICACHE_V143=1 scope=exception-vectors line=32 ice=hid0-bit15 icbi=line icfi=all data-cache-does-not-invalidate\n");
  std::fprintf(stderr,
               "GEKKOAOT_CP_IDLE_BREAKPOINT_RECOVERY_V160=%u policy=guest-idle+same-bp+3-vi-frames step=one-32b-burst bp-registers=preserved\n",
               cp_idle_breakpoint_recovery_enabled_ ? 1u : 0u);
  std::fprintf(stderr,
               "GEKKOAOT_HOST_FRAME_SCHEDULER_V48=1 target_hz=%u source=gx-copy guest_throttle=off interpolation=%u\n",
               host_fps_limit_hz_, frame_interpolation_enabled_ ? 1u : 0u);
  std::fprintf(stderr,
               "GEKKOAOT_PERF_METRICS_V49=%u fields=vps,guest_fps,present_fps,interp_fps,speed,target,drops\n",
               perf_metrics_enabled_ ? 1u : 0u);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_INTERCEPT_INDEX_V10=1 policy=compile-time-pruned+sorted-exact\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_EVENT_HORIZON_V2=1 cycles=%lld max-chain=1024 barriers=mmio+hle\n",
               static_cast<long long>(cpu_.cycle_budget));
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_VI_IRQ_TRACE_V81=1 stages=vi-latch+vi-ack+pi-level+pi-mask+ee+vector500 limit=bounded\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_OS_READY_FASTPATH_V82=1 thread-snapshot=single-pointer active-next=batched ready=compact semantics=unchanged\n");
  std::fprintf(stderr,
               "GEKKOAOT_FIXED_EXEC_ROUTER_V87=1 identity=guest-chunk+neighborhood active-image=persistent cross-module-indirect=host-routed rel=oslink\n");
  std::fprintf(stderr,
               "GEKKOAOT_CACHE_COHERENCY_V87=1 dc=host-coherent icbi=fixed-image-route-invalidate gx-cache=guest-commands\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_OS_THREAD_SANITY_V87=1 stack-magic=deadbabe active-links=validated priority=0-31 fail=skip-invalid\n");
  std::fprintf(stderr,
               "GEKKOAOT_SAFE_HOST_BOUNDARIES_V136=%u source=%s policy=%s chain_cycles=%lld hp-vi-gx-starvation-fix=1\n",
               (compat_diagnostics_enabled_ || safe_host_boundaries_enabled_) ? 1u : 0u,
               compat_diagnostics_enabled_ ? "gdb-diagnostics" :
               (safe_host_boundaries_enabled_ ? "runtime-env" : "fast-superchain"),
               compat_diagnostics_enabled_ ? "single-dispatch" :
               (safe_host_boundaries_enabled_ ? "bounded-superchain" : "fast-superchain"),
               static_cast<long long>(cpu_.cycle_budget));
  std::fprintf(stderr,
               "GEKKOAOT_RUNTIME_FASTPATH_V65=1 clock-sampling=batched-125us metrics=125ms "
               "native-os-bootstrap=once context-fp=host-shadow hle-inline=1\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_OS_SDK_COMPAT_V71=1 context=native-sdk-ppc "
               "waitq=priority+guest-import runqueue-hint=persistent reschedule=selectthread\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_SDK_DIRECT_V71=1 resolver=compile-time-token "
               "fallback=dynamic context-r3=exact entry-proof=bl+sdk-adjacency leaf-disambig=1\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_SDK_DIRECT_BOUNDARY_V72=1 "
               "native-abi-state=materialized-before-hostcall pc=entry cycles=settled\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_OS_EXC_PRESERVE_V73=1 "
               "selectthread=preserve-exception-frame scheduler-save=exc-aware sdk=retail-ppc\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_SDK_LEAF_PROOF_V74=1 "
               "get-thread-priority=abi-0x2d4+unique-or-osthread-cluster fail-closed=1\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_OS_CONTEXT_ABI_V76=1 "
               "manifest-class=context-capture caller-state=propagated "
               "save-context=nonvolatile-int+control+gqr+nonvolatile-fp\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_FAKE_VMEM_V77=1 base=7e000000 size=02000000 "
               "policy=flat-until-nonzero-sdr1 mmio=excluded\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_VP6_V70=%u backend=%s mode=codec-service ea-abi=frontend-required "
               "texture-sampling=aurora-separate\n",
               GekkoAOT::Video::NativeVP6Decoder::BackendAvailable() ? 1u : 0u,
               GekkoAOT::Video::NativeVP6Decoder::BackendAvailable() ? "libavcodec" : "unavailable");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_JAUDIO_HLE_V69=1 protocol=8100+8200 voices=64 vpb=0x180 "
               "formats=afc2+afc4+pcm8+pcm16 src=guest-4tap mix=6send "
               "fx=ring filters=lpf+fir+biquad\n");
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
  native_input_ready_ = GekkoAOT::Input::HostInput().Initialize();
  if (native_input_ready_) {
    GekkoAOT::HW::SI::NativeSI::DeviceHooks hooks{};
    hooks.user = this;
    hooks.poll = &HostRuntime::NativePadPoll;
    hooks.buffer = &HostRuntime::NativePadBuffer;
    hooks.direct = &HostRuntime::NativePadDirect;
    si_.SetDeviceHooks(hooks);
    RefreshNativeInput(false);
    std::fprintf(stderr,
                 "GEKKOAOT_STANDALONE_INPUT_V38=1 backend=SDL3 si=native keyboard=1 gamepad=1\n");
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_INPUT_CLOCK_V53B=1 hz=120 source=hardware-realtime "
                 "buffer_reads=latched vi=decoupled\n");
  } else {
    std::fprintf(stderr,
                 "GEKKOAOT_STANDALONE_INPUT_V38=0 backend=SDL3 reason=init-failed\n");
  }
#else
  std::fprintf(stderr,
               "GEKKOAOT_STANDALONE_INPUT_V38=0 backend=none reason=sdl3-not-built\n");
#endif
}

HostRuntime::~HostRuntime() {
  // Graphics owns asynchronous host callbacks, so quiesce it while guest
  // memory and the other host services are still alive. The explicit call in
  // native-run normally does this earlier; this is the idempotent fallback.
  ShutdownNativeGX();
  DetachDiscImage();
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
  if (native_input_ready_) GekkoAOT::Input::HostInput().Shutdown();
#endif
}

void HostRuntime::WireCpuCallbacks() {
  cp_.SetReadBurstCallback(&HostRuntime::ConsumeGpuFifoBurst, this);
  cpu_.ram = memory_.Mem1().data();
  cpu_.ram_size = static_cast<std::uint32_t>(memory_.Mem1().size());
  cpu_.exram = memory_.Mem2().empty() ? nullptr : memory_.Mem2().data();
  cpu_.exram_size = static_cast<std::uint32_t>(memory_.Mem2().size());
  cpu_.external_read = &HostRuntime::ExternalRead;
  cpu_.external_write = &HostRuntime::ExternalWrite;
  cpu_.external_read32 = &HostRuntime::ExternalRead32;
  cpu_.external_write32 = &HostRuntime::ExternalWrite32;
  cpu_.external_pointer = &HostRuntime::ExternalPointer;
  cpu_.instruction_fallback = &HostRuntime::InstructionFallback;
  cpu_.host_call = &HostRuntime::HostCall;
  cpu_.spr_read = &HostRuntime::SprRead;
  cpu_.spr_write = &HostRuntime::SprWrite;
  cpu_.cache_control = &HostRuntime::CacheControl;
  cpu_.external_user_data = this;
}

void HostRuntime::Reset() {
  memory_.Clear();
  cpu_ = {};
  // Compatibility diagnostics and v79 safe-host-boundary mode intentionally
  // disable the in-module superchain. GDB proved that returning to HostRuntime
  // after each AOT dispatch is required for titles that rapidly hand work between
  // CPU, VI, GX and asynchronous devices: with the 262144-cycle horizon the guest
  // can outrun host event delivery and wait forever for an XFB/GX transition.
  cpu_.cycle_budget = compat_diagnostics_enabled_
                          ? 0
                          : (safe_host_boundaries_enabled_
                                 ? SafeNativeChainCycleBudget()
                                 : NativeChainCycleBudget());
  fault_ = {};
  compat_break_fired_ = false;
  compat_breadcrumb_sequence_ = 0;
  compat_breadcrumb_next_ = 0;
  compat_breadcrumbs_.fill({});
  spr_state_.fill(0);
  memory_.SetPagedVmem(false);
  spr_state_[kSprDec] = 0xffffffffu;
  spr_state_[kSprPvr] = kGekkoPvr;
  spr_state_[kSprHid0] = kInitialHid0;
  // BS2/IPL architectural state used by Dolphin's direct-DOL bootstrap. Keep
  // these mappings active even though normal MEM1 aliases have a host fast
  // path: guest OS/MMU code inspects and temporarily relies on the real BATs.
  spr_state_[kSprHid1] = 0x80000000u;
  spr_state_[kSprIbat0u] = 0x80001fffu;
  spr_state_[kSprIbat0l] = 0x00000002u;
  spr_state_[kSprDbat0u] = 0x80001fffu;
  spr_state_[kSprDbat0l] = 0x00000002u;
  spr_state_[kSprDbat1u] = 0xc0001fffu;
  spr_state_[kSprDbat1l] = 0x0000002au;
  InvalidateLowInstructionCache();
  timebase_cycle_remainder_ = 0u;
  decrementer_cycle_remainder_ = 0u;
  decrementer_pending_ = false;
  timed_irq_sync_cycles_ = 0u;
  cp_idle_breakpoint_frames_ = 0u;
  cp_idle_breakpoint_address_ = 0u;
  cp_idle_breakpoint_recoveries_ = 0u;
  hardware_clock_started_ = false;
  hardware_clock_remainder_ = 0u;
  hardware_clock_guest_since_sample_ = 0u;
  hardware_clock_sampled_last_call_ = false;
  hardware_tsc_checked_ = false;
  hardware_tsc_enabled_ = false;
  hardware_tsc_hz_ = 0u;
  hardware_tsc_last_ = 0u;
  hardware_tsc_cycle_remainder_ = 0u;
  hardware_tsc_ns_remainder_ = 0u;
  host_present_pending_ = false;
  host_present_started_ = false;
  host_present_count_ = 0u;
  host_present_dropped_ = 0u;
  perf_metrics_started_ = false;
  perf_hardware_cycles_total_ = 0u;
  perf_vi_boundaries_total_ = 0u;
  perf_guest_frames_total_ = 0u;
  perf_last_hardware_cycles_ = 0u;
  perf_last_vi_boundaries_ = 0u;
  perf_last_guest_frames_ = 0u;
  perf_last_presents_ = 0u;
  perf_last_interpolated_ = 0u;
  perf_last_dropped_ = 0u;
  perf_metrics_poll_cycles_ = 0u;
  produced_frame_seen_ = false;
  interpolation_pair_valid_ = false;
  produced_frame_period_ = std::chrono::nanoseconds(16666667);
  interpolated_present_count_ = 0u;
  direct_present_count_ = 0u;
  live_video_config_poll_cycles_ = 0u;
  live_video_config_mtime_valid_ = false;
  hostcall_negative_cache_.fill(0u);
  hle_fast_cache_.fill({});
  ppc_halt_pcs_.clear();
  active_rel_sections_.clear();
  secondary_dispatches_ = 0;
  fixed_exec_route_cache_ = {};
  fixed_exec_active_descriptor_ = nullptr;
  fixed_exec_route_switches_ = 0;
  global_indirect_dispatches_ = 0;
  icbi_route_invalidations_ = 0;
  rel_mapping_refreshes_ = 0;
  rel_address_translations_ = 0;
  rel_address_misses_ = 0;
  rel_last_canonical_ = 0;
  rel_last_runtime_ = 0;
  lc_.Reset();
  cp_.Reset();
  pe_.Reset();
  pi_.Reset();
  mi_.Reset();
  ai_.Reset();
  di_.Reset(true);
  di_reads_ = 0;
  di_read_bytes_ = 0;
  di_read_failures_ = 0;
  di_async_completion_pending_ = false;
  di_async_cycles_remaining_ = 0u;
  di_async_staging_.clear();
  di_drive_head_offset_ = 0u;
  di_drive_head_valid_ = false;
  di_read_buffer_start_ = 0u;
  di_read_buffer_end_ = 0u;
  exi_.Reset();
  si_.Reset();
  vi_.Reset();
  dsp_.BindMemory(&memory_);
  dsp_.Reset();
  hle_calls_ = 0;
  last_mmio_read_ = 0;
  mmio_reads_ = 0;
  same_mmio_read_streak_ = 0;
  cpu_fifo_gather_.fill(0);
  cpu_fifo_gather_size_ = 0;
  cpu_fifo_gather_base_ = 0;
  cpu_fifo_gather_end_ = 0;
  cpu_fifo_capture_bursts_ = 0;
  cpu_fifo_capture_logged_ = false;
  gpu_fifo_gather_.fill(0);
  gpu_fifo_gather_size_ = 0;
  gpu_fifo_first_pc_ = 0;
  gpu_fifo_bursts_ = 0;
  gpu_fifo_burst_logged_ = false;
  WireCpuCallbacks();
  GekkoAOT::NativeOS::Service::Get().Reset();
  RebuildNativeInterceptIndex();
  for (const auto& [address, kind] : os_hooks_) {
    GekkoAOT::NativeOS::Service::Get().Register(kind, address);
    const auto slot=((address>>2u)^(address>>13u))&(kHleFastCacheSize-1u);
    hle_fast_cache_[slot]={address,HleFastType::Os,static_cast<std::uint8_t>(kind)};
  }
  for (const auto& [address, kind] : simple_hle_hooks_) {
    const auto slot=((address>>2u)^(address>>13u))&(kHleFastCacheSize-1u);
    hle_fast_cache_[slot]={address,HleFastType::Simple,static_cast<std::uint8_t>(kind)};
  }
}

ModuleStatus HostRuntime::LoadModule(const std::filesystem::path& path,
                                     const char* expected_game_id) {
  const auto status = module_.Open(path, expected_game_id);
  if (status != ModuleStatus::Ok) return status;
  const auto* descriptor = module_.Descriptor();
  cpu_.pc = descriptor->entry_point;
  if (descriptor->on_state_loaded) descriptor->on_state_loaded(&cpu_);
  return ModuleStatus::Ok;
}

ModuleStatus HostRuntime::LoadSecondaryModule(const std::filesystem::path& path,
                                              const char* expected_game_id) {
  auto library = std::make_unique<ModuleLibrary>();
  const auto status = library->Open(path, expected_game_id);
  if (status != ModuleStatus::Ok) {
    secondary_module_last_error_ = library->LastError();
    return status;
  }
  if (memory_.PagedVmem() && !library->SupportsMemoryRestart()) {
    secondary_module_last_error_ = ModuleStatusName(ModuleStatus::MemoryRestartRequired);
    return ModuleStatus::MemoryRestartRequired;
  }
  const auto* descriptor = library->Descriptor();
  if (descriptor->on_state_loaded) descriptor->on_state_loaded(&cpu_);
  secondary_modules_.push_back(std::move(library));
  active_rel_sections_.clear();
  secondary_module_last_error_.clear();
  return ModuleStatus::Ok;
}

GameCubeBootResult HostRuntime::InitializeGameCube(
    const std::filesystem::path& boot_bin, const GameCubeBootConfig& config) {
  return InitializeGameCube(ReadFile(boot_bin), config);
}

GameCubeBootResult HostRuntime::InitializeGameCube(
    const std::vector<std::uint8_t>& boot_bin, const GameCubeBootConfig& config) {
  const auto result = InitializeGameCubeBoot(memory_, cpu_, boot_bin, config);
  vi_.Reset(result.ntsc);
  hardware_clock_started_ = false;
  hardware_clock_remainder_ = 0u;
  hardware_clock_guest_since_sample_ = 0u;
  hardware_clock_sampled_last_call_ = false;
  hardware_tsc_checked_ = false;
  hardware_tsc_enabled_ = false;
  hardware_tsc_hz_ = 0u;
  hardware_tsc_last_ = 0u;
  hardware_tsc_cycle_remainder_ = 0u;
  hardware_tsc_ns_remainder_ = 0u;
  host_present_started_ = false;
  host_present_pending_ = false;
  gx_frame_ready_since_vi_ = false;
  std::fprintf(stderr,
               "GEKKOAOT_VI_TIMING_V47=1 mode=retail-rate source=host-monotonic render_clock=independent\n");
  return result;
}

GameCubeFstResult HostRuntime::LoadGameCubeFst(const std::filesystem::path& fst_bin) {
  return LoadGameCubeFst(ReadFile(fst_bin));
}

GameCubeFstResult HostRuntime::LoadGameCubeFst(const std::vector<std::uint8_t>& fst) {
  return InitializeGameCubeFst(memory_, fst);
}

GameCubeBi2Result HostRuntime::LoadGameCubeBi2(const std::filesystem::path& bi2_bin) {
  return LoadGameCubeBi2(ReadFile(bi2_bin));
}

GameCubeBi2Result HostRuntime::LoadGameCubeBi2(const std::vector<std::uint8_t>& bi2) {
  return InitializeGameCubeBi2(memory_, bi2);
}

void HostRuntime::DetachDiscImage() {
  native_vfs_.Reset();
#ifdef GEKKOAOT_HAVE_NOD
  if (disc_image_uses_nod_ && nod_disc_image_)
    nod_free(static_cast<NodHandle*>(nod_disc_image_));
#endif
  nod_disc_image_ = nullptr;
  disc_image_uses_nod_ = false;
  disc_image_.close();
  disc_image_.clear();
  disc_image_path_.clear();
  disc_image_size_ = 0;
  disc_media_backend_ = "none";
  disc_media_format_ = "unknown";
  di_async_completion_pending_ = false;
  di_async_cycles_remaining_ = 0u;
  di_async_bytes_ = 0u;
  di_async_dma_address_ = 0u;
  di_async_disc_offset_ = 0u;
  di_async_staging_.clear();
  di_drive_head_offset_ = 0u;
  di_drive_head_valid_ = false;
  di_read_buffer_start_ = 0u;
  di_read_buffer_end_ = 0u;
  di_.SetDeviceHooks({});
  di_.SetDiscPresent(false);
}

bool HostRuntime::AttachDiscImage(const std::filesystem::path& image_path) {
  DetachDiscImage();
  di_reads_ = 0;
  di_read_bytes_ = 0;
  di_read_failures_ = 0;
  vfs_reads_ = 0;
  vfs_read_bytes_ = 0;
  vfs_fallback_reads_ = 0;

#ifdef GEKKOAOT_HAVE_NOD
  const auto u8_path = image_path.u8string();
  const std::string utf8_path(u8_path.begin(), u8_path.end());
  NodDiscOptions options{};
  options.preloader_threads = 0u; // DI is random-access; avoid sequential preload work.

  NodHandle* handle = nullptr;
  if (nod_disc_open(utf8_path.c_str(), &options, &handle) != NOD_RESULT_OK || !handle)
    return false;

  NodDiscHeader header{};
  constexpr std::array<std::uint8_t, 4> kGameCubeMagic{0xc2u, 0x33u, 0x9fu, 0x3du};
  if (nod_disc_header(handle, &header) != NOD_RESULT_OK ||
      !std::equal(kGameCubeMagic.begin(), kGameCubeMagic.end(), header.gcn_magic)) {
    nod_free(handle);
    return false;
  }

  const std::uint64_t size = nod_disc_size(handle);
  if (size < 0x440u) {
    nod_free(handle);
    return false;
  }

  NodDiscMeta meta{};
  if (nod_disc_meta(handle, &meta) == NOD_RESULT_OK)
    disc_media_format_ = NodFormatName(meta.format);
  else
    disc_media_format_ = "unknown";

  std::error_code error;
  disc_image_path_ = std::filesystem::absolute(image_path, error);
  if (error) disc_image_path_ = image_path;
  disc_image_size_ = size;
  nod_disc_image_ = handle;
  disc_image_uses_nod_ = true;
  disc_media_backend_ = "nod-direct";

  if (RuntimeEnvBool("GEKKOAOT_NATIVE_VFS", true)) {
    const char* root = std::getenv("GEKKOAOT_VFS_ROOT");
    const char* overlay = std::getenv("GEKKOAOT_VFS_OVERLAY");
    const bool mounted = native_vfs_.Mount(
        handle, root && *root ? std::filesystem::path(root) : std::filesystem::path{},
        overlay && *overlay ? std::filesystem::path(overlay) : std::filesystem::path{});
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_VFS_V154=%u mode=fst-file-translation sdk=guest-aot di=raw-fallback mechanical-latency=off-by-default entries=%zu\n",
                 mounted ? 1u : 0u, mounted ? native_vfs_.EntryCount() : 0u);
  } else {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_VFS_V154=0 reason=disabled di=raw-only mechanical-latency=gc-default\n");
  }
#else
  // Lightweight fallback used by standalone unit tests and explicitly
  // GEKKOAOT_NATIVE_NOD=OFF developer builds. Production controller builds
  // enable nod, so compressed containers never need to be materialized.
  std::error_code error;
  const std::uint64_t size = std::filesystem::file_size(image_path, error);
  if (error || size < 0x440u) return false;

  disc_image_.open(image_path, std::ios::binary);
  if (!disc_image_) return false;

  std::array<std::uint8_t, 4> magic{};
  disc_image_.seekg(0x1c, std::ios::beg);
  disc_image_.read(reinterpret_cast<char*>(magic.data()), magic.size());
  if (disc_image_.gcount() != static_cast<std::streamsize>(magic.size()) ||
      magic != std::array<std::uint8_t, 4>{0xc2u, 0x33u, 0x9fu, 0x3du}) {
    disc_image_.close();
    disc_image_.clear();
    return false;
  }

  disc_image_.clear();
  disc_image_path_ = std::filesystem::absolute(image_path, error);
  if (error) disc_image_path_ = image_path;
  disc_image_size_ = size;
  disc_media_backend_ = "raw-file-fallback";
  disc_media_format_ = "ISO/GCM";
#endif

  di_.SetDiscPresent(true);
  di_.SetDeviceHooks({this, &HostRuntime::DiscCommand});
  return true;
}

bool HostRuntime::ReadDiscImage(std::uint64_t offset, void* destination, std::uint32_t size) {
  if (!destination) return false;
  if (offset > disc_image_size_ || size > disc_image_size_ - offset) return false;
  if (size == 0u) return true;

  if (RuntimeEnvBool("GEKKOAOT_NATIVE_VFS", true) && native_vfs_.Ready()) {
    const char* source = nullptr;
    if (native_vfs_.ReadDiscRange(offset, destination, size, &source)) {
      ++vfs_reads_;
      vfs_read_bytes_ += size;
      static unsigned vfs_logs = 0u;
      if (vfs_logs++ < 96u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_VFS_V154=1 phase=read offset=%08llx bytes=%u source=%s reads=%llu\n",
                     static_cast<unsigned long long>(offset), size,
                     source ? source : "vfs",
                     static_cast<unsigned long long>(vfs_reads_));
      return true;
    }
    ++vfs_fallback_reads_;
  }

#ifdef GEKKOAOT_HAVE_NOD
  if (disc_image_uses_nod_) {
    auto* handle = static_cast<NodHandle*>(nod_disc_image_);
    if (!handle || offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
      return false;
    const auto sought = nod_seek(handle, static_cast<std::int64_t>(offset), 0);
    if (sought < 0 || static_cast<std::uint64_t>(sought) != offset) return false;

    auto* out = static_cast<std::uint8_t*>(destination);
    std::size_t done = 0;
    while (done < size) {
      const std::int64_t got = nod_read(handle, out + done, size - done);
      if (got <= 0) return false;
      done += static_cast<std::size_t>(got);
    }
    return true;
  }
#endif

  if (!disc_image_.is_open()) return false;
  disc_image_.clear();
  disc_image_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!disc_image_) return false;
  disc_image_.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
  return disc_image_.gcount() == static_cast<std::streamsize>(size);
}

namespace {
std::uint64_t GameCubeDiLatencyCycles(std::uint64_t offset, std::uint32_t length,
                                      std::uint64_t disc_size,
                                      bool* head_valid, std::uint64_t* head_offset,
                                      std::uint64_t* buffer_start, std::uint64_t* buffer_end) {
  constexpr double kCpuHz = static_cast<double>(GekkoAOT::HW::VI::NativeVI::CpuClockHz);
  constexpr double kCommandSeconds = 600.0 / 1'000'000.0;
  constexpr double kBufferRate = 32.0 * 1024.0 * 1024.0;
  constexpr double kInnerRate = 2.1 * 1024.0 * 1024.0;
  constexpr double kOuterRate = 3.325 * 1024.0 * 1024.0;
  constexpr std::uint64_t kReadAhead = 1024u * 1024u;

  const std::uint64_t end = offset + length;
  const bool buffered = *buffer_end > *buffer_start && offset >= *buffer_start && end <= *buffer_end;
  double seconds = kCommandSeconds;
  if (buffered) {
    seconds += static_cast<double>(length) / kBufferRate;
  } else {
    if (*head_valid && offset != *head_offset) {
      const std::uint64_t delta = offset > *head_offset ? offset - *head_offset : *head_offset - offset;
      const double denom = static_cast<double>(std::max<std::uint64_t>(disc_size, 1u));
      const double distance = std::min(1.0, static_cast<double>(delta) / denom * 12.0);
      // Real GC seeks have a fixed mechanical component plus distance cost;
      // add average rotational latency (half a 28.5 Hz revolution).
      seconds += 0.035 + 0.040 * distance + (0.5 / 28.5);
    }
    const double ratio = disc_size > 1 ? std::clamp(static_cast<double>(offset) /
                                                        static_cast<double>(disc_size - 1),
                                                    0.0, 1.0)
                                       : 0.0;
    const double rate = kInnerRate + (kOuterRate - kInnerRate) * ratio;
    seconds += static_cast<double>(length) / rate;
    *head_valid = true;
    *head_offset = end;
    *buffer_start = offset;
    *buffer_end = std::min<std::uint64_t>(disc_size, end + kReadAhead);
  }
  const auto cycles = static_cast<std::uint64_t>(seconds * kCpuHz);
  return std::clamp<std::uint64_t>(cycles,
      GekkoAOT::HW::VI::NativeVI::CpuClockHz / 2000u,  // >= 500 us
      GekkoAOT::HW::VI::NativeVI::CpuClockHz / 2u);    // <= 500 ms
}
} // namespace

GekkoAOT::HW::DI::NativeDI::CommandResponse HostRuntime::DiscCommand(
    void* user, const GekkoAOT::HW::DI::NativeDI::CommandRequest& request) {
  using Response = GekkoAOT::HW::DI::NativeDI::CommandResponse;
  auto* self = static_cast<HostRuntime*>(user);
  Response response{};
  if (!self) return response;

  const std::uint8_t opcode = static_cast<std::uint8_t>(request.command[0] >> 24);
  // DVDLowRead / DI 0xA8. GameCube encodes the logical disc offset in
  // command[1] as words and uses the low command byte as the read subcommand.
  if (opcode == 0xa8u) {
    const std::uint8_t subcommand = static_cast<std::uint8_t>(request.command[0] & 0xffu);
    std::uint64_t offset = 0;
    std::uint32_t source_length = 0;
    if (subcommand == 0x00u) {
      offset = static_cast<std::uint64_t>(request.command[1]) << 2;
      source_length = request.command[2];
    } else if (subcommand == 0x40u) {
      source_length = 0x20u;
    } else {
      ++self->di_read_failures_;
      return response;
    }

    const std::uint32_t transfer = std::min(source_length, request.dma_length);
    if ((request.control & 0x2u) == 0u || (request.control & 0x4u) != 0u || transfer == 0u) {
      ++self->di_read_failures_;
      return response;
    }

    // Reject overlap before DMA or disc I/O can touch guest memory. A
    // cancellation already clears this pending flag in the DI register write.
    if (self->di_async_completion_pending_) {
      ++self->di_read_failures_;
      response.error_code = 0x00052000u;
      static unsigned overlap_logs = 0u;
      if (overlap_logs++ < 16u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_DI_ASYNC_V58=0 reason=overlap offset=%08llx dma=%08x bytes=%u\n",
                     static_cast<unsigned long long>(offset), request.dma_address, transfer);
      return response;
    }

    // Dolphin-style ordering: media I/O may finish on the host now, but the
    // guest DMA buffer must not become visible before the emulated drive
    // reaches transfer completion. Stage the bytes host-side and publish them
    // atomically immediately before TCINT.
    if (!self->memory_.Resolve(request.dma_address, transfer)) {
      ++self->di_read_failures_;
      response.error_code = 0x00031100u;
      return response;
    }
    self->di_async_staging_.resize(transfer);
    if (!self->ReadDiscImage(offset, self->di_async_staging_.data(), transfer)) {
      self->di_async_staging_.clear();
      ++self->di_read_failures_;
      response.error_code = 0x00031100u;
      return response;
    }

    ++self->di_reads_;
    self->di_read_bytes_ += transfer;

    // Keep TSTART asserted until the drive-time model expires. The previous
    // bytes*2 shortcut completed a 32 KiB read in ~135 us and let streaming
    // state machines outrun retail hardware by orders of magnitude.
    self->di_async_completion_pending_ = true;
    const bool native_vfs_timing =
        RuntimeEnvBool("GEKKOAOT_NATIVE_VFS", true) && self->native_vfs_.Ready() &&
        !RuntimeEnvBool("GEKKOAOT_VFS_GC_TIMING", false);
    if (native_vfs_timing) {
      // NativeVFS is a host filesystem service, not an optical-drive timing
      // model. Keep completion deferred until the next host hardware boundary
      // so DVDReadAsyncPrio cannot callback reentrantly from the initiating
      // MMIO write, but add no artificial seek/rotation/transfer delay.
      self->di_async_cycles_remaining_ = 0u;
    } else if (RuntimeEnvBool("GEKKOAOT_FAST_DISC", false)) {
      self->di_async_cycles_remaining_ =
          std::clamp<std::uint64_t>(static_cast<std::uint64_t>(transfer) * 2u,
                                    8192u, 1048576u);
    } else {
      self->di_async_cycles_remaining_ = GameCubeDiLatencyCycles(
          offset, transfer, self->disc_image_size_, &self->di_drive_head_valid_,
          &self->di_drive_head_offset_, &self->di_read_buffer_start_,
          &self->di_read_buffer_end_);
    }
    self->di_async_bytes_ = transfer;
    self->di_async_dma_address_ = request.dma_address;
    self->di_async_disc_offset_ = offset;
    self->di_async_sequence_ = request.sequence;
    const auto sequence = request.sequence;

    static unsigned schedule_logs = 0u;
    if (schedule_logs++ < 128u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_DI_PARITY_V143=1 phase=schedule seq=%llu offset=%08llx dma=%08x bytes=%u delay_cycles=%llu mode=%s visibility=completion\n",
                   static_cast<unsigned long long>(sequence),
                   static_cast<unsigned long long>(offset), request.dma_address, transfer,
                   static_cast<unsigned long long>(self->di_async_cycles_remaining_),
                   native_vfs_timing ? "native-vfs" :
                       (RuntimeEnvBool("GEKKOAOT_FAST_DISC", false) ? "fast-optin" : "gc-drive"));

    response.result = 0; // pending: NativeDI keeps TSTART asserted
    response.bytes_transferred = transfer;
    response.error_code = 0;
    return response;
  }

  // DVDLowInquiry / DI 0x12 returns a 32-byte DMA response. The first
  // three words identify the retail drive firmware; the rest of the DMA
  // buffer is zero-filled. This is drive metadata rather than disc payload,
  // but it shares the DI DMA boundary and therefore lives in this host hook.
  if (opcode == 0x12u) {
    constexpr std::uint32_t kInquiryLength = 0x20u;
    if ((request.control & 0x2u) == 0u || (request.control & 0x4u) != 0u ||
        request.dma_length < kInquiryLength) {
      response.error_code = 0x00052000u;
      return response;
    }

    auto* target = static_cast<std::uint8_t*>(self->memory_.Resolve(
        request.dma_address, kInquiryLength));
    if (!target) {
      response.error_code = 0x00031100u;
      return response;
    }

    std::memset(target, 0, kInquiryLength);
    constexpr std::uint8_t kInquiryWords[12] = {
        0x00, 0x00, 0x00, 0x02,
        0x20, 0x06, 0x05, 0x26,
        0x41, 0x00, 0x00, 0x00,
    };
    std::memcpy(target, kInquiryWords, sizeof(kInquiryWords));
    response.result = 1;
    response.bytes_transferred = kInquiryLength;
    response.error_code = 0;
    return response;
  }

  // Retail GameCube software commonly issues AudioBufferConfig during drive
  // setup. DTK streaming itself is a later native-audio concern; accepting the
  // configuration command keeps non-streaming games on the native DI path.
  if (opcode == 0xe4u) {
    response.result = 1;
    response.error_code = 0;
    return response;
  }

  return response;
}

DolLoadResult HostRuntime::LoadDolImage(const std::filesystem::path& path) {
  return LoadDolImage(ReadFile(path));
}

DolLoadResult HostRuntime::LoadDolImage(const std::vector<std::uint8_t>& image) {
  loaded_dol_code_ranges_.clear();
  if (image.size() >= 0x100u) {
    for (unsigned i = 0; i < 7; ++i) {
      const auto address = detail::DolBE32(image, 0x48u + i * 4u);
      const auto size = detail::DolBE32(image, 0x90u + i * 4u);
      if (address != 0u && size != 0u && std::uint64_t(address) + size < 0x100000000ull)
        loaded_dol_code_ranges_.push_back({
            address, static_cast<std::uint32_t>(std::uint64_t(address) + size)});
    }
    std::sort(loaded_dol_code_ranges_.begin(), loaded_dol_code_ranges_.end(),
              [](const GekkoAOTRange& a, const GekkoAOTRange& b) { return a.start < b.start; });
  }
  const auto result = LoadDol(image, memory_);
  loaded_dol_entry_point_ = result.entry_point;
  // The AOT descriptor is authoritative for code dispatch. A mismatch is not
  // fatal here because some generated images intentionally have a bootstrap
  // entry different from the descriptor's first native dispatcher address.
  if (!module_.Descriptor()) cpu_.pc = result.entry_point;
  RebuildPpcHaltIndex();
  return result;
}

void HostRuntime::RegisterOsHook(GekkoAOT::NativeOS::Kind kind, std::uint32_t address) {
  if (!address) return;
  os_hooks_[address] = kind;
  RebuildNativeInterceptIndex();
  const auto slot = ((address >> 2u) ^ (address >> 14u)) &
                    (kHostCallNegativeCacheSize - 1u);
  if (hostcall_negative_cache_[slot] == address)
    hostcall_negative_cache_[slot] = 0u;
  const auto fast_slot=((address>>2u)^(address>>13u))&(kHleFastCacheSize-1u);
  hle_fast_cache_[fast_slot]={address,HleFastType::Os,static_cast<std::uint8_t>(kind)};
  GekkoAOT::NativeOS::Service::Get().Register(kind, address);
}

void HostRuntime::RegisterSimpleHle(SimpleHleKind kind, std::uint32_t address) {
  if (!address) return;
  simple_hle_hooks_[address] = kind;
  RebuildNativeInterceptIndex();
  const auto slot = ((address >> 2u) ^ (address >> 14u)) &
                    (kHostCallNegativeCacheSize - 1u);
  if (hostcall_negative_cache_[slot] == address)
    hostcall_negative_cache_[slot] = 0u;
  const auto fast_slot=((address>>2u)^(address>>13u))&(kHleFastCacheSize-1u);
  hle_fast_cache_[fast_slot]={address,HleFastType::Simple,static_cast<std::uint8_t>(kind)};
}

bool HostRuntime::InitializeNativeGX(const std::filesystem::path& bridge_path) {
  if (!native_gx_.Open(bridge_path, memory_)) return false;
  // NativeVI remains the guest timing authority. Optionally let a completed
  // retail GXCopyDisp drive host presentation instead of waiting for VI.
  native_gx_.SetCyclePresentEnabled(false);
  const bool requested_copy_present =
      RuntimeEnvBool("GEKKOAOT_NATIVE_GX_PRESENT_ON_COPY", false);
  gx_present_on_copy_ = requested_copy_present && native_gx_.HasFrameReadySignal();
  gx_present_fallback_vi_ =
      RuntimeEnvBool("GEKKOAOT_NATIVE_GX_PRESENT_FALLBACK_VI", true);
  gx_frame_ready_since_vi_ = false;
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_PRESENT_V43B=%u mode=%s frame_ready_abi=%u vi_fallback=%u\n",
               gx_present_on_copy_ ? 1u : 0u,
               gx_present_on_copy_ ? "gx-copy" : "vi",
               native_gx_.HasFrameReadySignal() ? 1u : 0u,
               gx_present_fallback_vi_ ? 1u : 0u);
  if (requested_copy_present && !native_gx_.HasFrameReadySignal())
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_GX_PRESENT_V43B=0 reason=renderer-missing-frame-ready-abi action=vi-fallback\n");
  const bool interpolation_abi = native_gx_.HasFrameInterpolation();
  if (frame_interpolation_enabled_ && !interpolation_abi)
    frame_interpolation_enabled_ = false;
  std::fprintf(stderr,
               "GEKKOAOT_FRAME_INTERPOLATION_V48=%u mode=temporal-gpu-blend target_hz=%u abi=%u guest_clock=unchanged\n",
               frame_interpolation_enabled_ ? 1u : 0u, host_fps_limit_hz_,
               interpolation_abi ? 1u : 0u);
  native_gx_.SetPeEventSink(&HostRuntime::NativeGXPeEvent, this);
  // Preserve any event produced during renderer initialization before the sink
  // was installed, then stay fully event-driven for normal execution.
  SyncNativeGXInterrupts();
  return true;
}

void HostRuntime::ShutdownNativeGX() {
  if (!native_gx_.Ready()) return;
  native_gx_.SetPeEventSink(nullptr, nullptr);
  native_gx_.Close();
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_GX_HOST_SHUTDOWN_V12=1 order=gx-before-input-disc\n");
}

void HostRuntime::NativeGXPeEvent(void* user, std::uint32_t reg, std::uint32_t value) {
  auto* self = static_cast<HostRuntime*>(user);
  if (!self) return;
  switch (reg) {
  case 0x45u: // BPMEM_SETDRAWDONE
    if ((value & 0xffu) == 0x02u) self->pe_.SetFinish();
    break;
  case 0x47u: // BPMEM_PE_TOKEN_ID
    self->pe_.SetToken(static_cast<std::uint16_t>(value), false);
    break;
  case 0x48u: // BPMEM_PE_TOKEN_INT_ID
    self->pe_.SetToken(static_cast<std::uint16_t>(value), true);
    break;
  default:
    return;
  }
  self->SyncNativePEInterrupts();
}

HostRuntime* HostRuntime::Self(CPUState* cpu) {
  return cpu ? static_cast<HostRuntime*>(cpu->external_user_data) : nullptr;
}

const char* HostRuntime::CompatDispatchKindName(CompatDispatchKind kind) {
  switch (kind) {
  case CompatDispatchKind::Main: return "main";
  case CompatDispatchKind::Secondary: return "secondary";
  case CompatDispatchKind::LowMemory: return "lowmem";
  case CompatDispatchKind::NativeHle: return "hle";
  case CompatDispatchKind::Idle: return "idle";
  }
  return "unknown";
}

void HostRuntime::RecordCompatBreadcrumb(std::uint32_t pc, CompatDispatchKind kind) {
  if (!compat_diagnostics_enabled_) return;
  auto& entry = compat_breadcrumbs_[compat_breadcrumb_next_];
  entry.sequence = ++compat_breadcrumb_sequence_;
  entry.pc = pc;
  entry.lr = cpu_.lr;
  entry.sp = cpu_.gpr[1];
  entry.kind = kind;
  entry.instruction = 0u;
  (void)memory_.Read32(pc, &entry.instruction);
  compat_breadcrumb_next_ = (compat_breadcrumb_next_ + 1u) % kCompatBreadcrumbCount;
}

void HostRuntime::DumpCompatHistory(const char* reason, std::uint32_t address,
                                    std::uint32_t value) const {
  if (!compat_diagnostics_enabled_) return;
  std::fprintf(stderr,
               "GEKKOAOT_COMPAT_FAILURE_V1 reason=%s address=0x%08x value=0x%08x "
               "pc=0x%08x lr=0x%08x sp=0x%08x ctr=0x%08x cr=0x%08x xer=0x%08x "
               "msr=0x%08x srr0=0x%08x srr1=0x%08x dar=0x%08x dsisr=0x%08x "
               "exception=0x%08x program_exception=0x%08x dispatch_seq=%llu\n",
               reason ? reason : "unknown", address, value, cpu_.pc, cpu_.lr, cpu_.gpr[1],
               cpu_.ctr, cpu_.cr, cpu_.xer, cpu_.msr, cpu_.srr0, cpu_.srr1, cpu_.dar,
               cpu_.dsisr, cpu_.exception, cpu_.program_exception,
               static_cast<unsigned long long>(compat_breadcrumb_sequence_));
  std::fprintf(stderr,
               "GEKKOAOT_COMPAT_GPRS_V1 r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x "
               "r6=%08x r7=%08x r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x r13=%08x\n",
               cpu_.gpr[0], cpu_.gpr[1], cpu_.gpr[2], cpu_.gpr[3], cpu_.gpr[4], cpu_.gpr[5],
               cpu_.gpr[6], cpu_.gpr[7], cpu_.gpr[8], cpu_.gpr[9], cpu_.gpr[10], cpu_.gpr[11],
               cpu_.gpr[12], cpu_.gpr[13]);

  const std::uint64_t available =
      std::min<std::uint64_t>(compat_breadcrumb_sequence_, kCompatBreadcrumbCount);
  const std::size_t start = compat_breadcrumb_sequence_ < kCompatBreadcrumbCount
      ? 0u : compat_breadcrumb_next_;
  for (std::uint64_t i = 0; i < available; ++i) {
    const auto& entry = compat_breadcrumbs_[(start + static_cast<std::size_t>(i)) %
                                            kCompatBreadcrumbCount];
    if (entry.sequence == 0u) continue;
    std::fprintf(stderr,
                 "GEKKOAOT_COMPAT_BREADCRUMB_V1 seq=%llu kind=%s pc=0x%08x op=0x%08x "
                 "lr=0x%08x sp=0x%08x\n",
                 static_cast<unsigned long long>(entry.sequence),
                 CompatDispatchKindName(entry.kind), entry.pc, entry.instruction, entry.lr, entry.sp);
  }
  std::fflush(stderr);
}

void HostRuntime::CompatDebuggerBreak() {
  if (!compat_break_on_fault_ || compat_break_fired_) return;
  compat_break_fired_ = true;
#if defined(_MSC_VER)
  __debugbreak();
#elif defined(SIGTRAP)
  std::raise(SIGTRAP);
#else
  std::abort();
#endif
}

void HostRuntime::ReportCompatFailure(const char* reason, std::uint32_t address,
                                      std::uint32_t value, bool break_into_debugger) {
  if (!compat_diagnostics_enabled_) return;
  DumpCompatHistory(reason, address, value);
  if (break_into_debugger) CompatDebuggerBreak();
}

void HostRuntime::SetFault(FaultKind kind, std::uint32_t address, std::uint32_t value) {
  if (fault_.kind != FaultKind::None) return;
  fault_.kind = kind;
  fault_.address = address;
  fault_.value = value;
  // Fatal host faults must unwind generated code just like architectural
  // exceptions. Otherwise an unsupported fallback can repeat inside the AOT
  // body forever before Run() gets a chance to inspect fault_. Bit 31 is a
  // host-only stop latch, never delivered as a guest exception vector.
  cpu_.exception |= 0x80000000u;
  const char* reason = "runtime-fault";
  switch (kind) {
  case FaultKind::None: reason = "none"; break;
  case FaultKind::InstructionFallback: reason = "instruction-fallback"; break;
  case FaultKind::MmioRead: reason = "mmio-read"; break;
  case FaultKind::MmioWrite: reason = "mmio-write"; break;
  case FaultKind::SprRead: reason = "spr-read"; break;
  case FaultKind::SprWrite: reason = "spr-write"; break;
  case FaultKind::HleFailure: reason = "hle-failure"; break;
  case FaultKind::InvalidMemoryRead: reason = "invalid-memory-read"; break;
  case FaultKind::InvalidMemoryWrite: reason = "invalid-memory-write"; break;
  }
  ReportCompatFailure(reason, address, value);
}

namespace {
constexpr std::uint16_t kPadUseOrigin = 0x0080u;
constexpr std::uint32_t kSiGcController = 0x09000000u;

std::uint8_t PadAxis(std::int8_t value) {
  return static_cast<std::uint8_t>(std::clamp(128 + static_cast<int>(value), 0, 255));
}

std::uint32_t PadHighWord(const GekkoAOT::Input::Pad& pad) {
  return static_cast<std::uint32_t>(PadAxis(pad.stick_y)) |
         (static_cast<std::uint32_t>(PadAxis(pad.stick_x)) << 8) |
         (static_cast<std::uint32_t>(pad.buttons | kPadUseOrigin) << 16);
}

std::uint32_t PadLowWord(const GekkoAOT::Input::Pad& pad, std::uint8_t mode) {
  const std::uint32_t sub_y = PadAxis(pad.sub_y);
  const std::uint32_t sub_x = PadAxis(pad.sub_x);
  switch (mode) {
  case 0u: case 5u: case 6u: case 7u:
    return (pad.analog_b >> 4) |
           (static_cast<std::uint32_t>(pad.analog_a >> 4) << 4) |
           (static_cast<std::uint32_t>(pad.right >> 4) << 8) |
           (static_cast<std::uint32_t>(pad.left >> 4) << 12) |
           (sub_y << 16) | (sub_x << 24);
  case 1u:
    return (pad.analog_b >> 4) |
           (static_cast<std::uint32_t>(pad.analog_a >> 4) << 4) |
           (static_cast<std::uint32_t>(pad.right) << 8) |
           (static_cast<std::uint32_t>(pad.left) << 16) |
           ((sub_y >> 4) << 24) | ((sub_x >> 4) << 28);
  case 2u:
    return pad.analog_b |
           (static_cast<std::uint32_t>(pad.analog_a) << 8) |
           (static_cast<std::uint32_t>(pad.right >> 4) << 16) |
           (static_cast<std::uint32_t>(pad.left >> 4) << 20) |
           ((sub_y >> 4) << 24) | ((sub_x >> 4) << 28);
  case 4u:
    return pad.analog_b | (static_cast<std::uint32_t>(pad.analog_a) << 8) |
           (sub_y << 16) | (sub_x << 24);
  case 3u:
  default:
    return pad.right | (static_cast<std::uint32_t>(pad.left) << 8) |
           (sub_y << 16) | (sub_x << 24);
  }
}
} // namespace

void HostRuntime::RefreshNativeInput(bool poll_si) {
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
  if (!native_input_ready_) return;
  GekkoAOT::Input::HostInput().Poll(native_pads_);
  if (poll_si) {
    si_.PollNow();
    SyncNativeSIInterrupt();
  }
#else
  (void)poll_si;
#endif
}

bool HostRuntime::NativePadPoll(void* user, std::uint32_t channel,
                                std::uint32_t* hi, std::uint32_t* lo) {
  auto* self = static_cast<HostRuntime*>(user);
  if (!self || !hi || !lo || channel >= self->native_pads_.size()) return false;
  const auto& pad = self->native_pads_[channel];
  if (!pad.connected) return false;
  *hi = PadHighWord(pad);
  *lo = PadLowWord(pad, self->native_pad_mode_[channel]);
  return true;
}

int HostRuntime::NativePadBuffer(void* user, std::uint32_t channel, std::uint8_t* buffer,
                                 std::uint32_t request_length,
                                 std::uint32_t expected_response_length) {
  auto* self = static_cast<HostRuntime*>(user);
  if (!self || !buffer || request_length == 0u || channel >= self->native_pads_.size()) return -1;

  // A transfer must consume the already-latched SI state. Sampling SDL here
  // makes the host device cadence proportional to uncapped guest PAD traffic,
  // so one physical press can be observed as several fresh samples before the
  // next GameCube hardware tick. The hardware-time sampler in
  // AdvanceRuntimeCycles() is the sole owner of host -> guest input updates.
  const auto& pad = self->native_pads_[channel];
  if (!pad.connected) return -1;

  const std::uint8_t command = buffer[0];
  if (command == 0x00u || command == 0xffu) {
    if (expected_response_length < 3u) return -1;
    buffer[0] = static_cast<std::uint8_t>(kSiGcController >> 24);
    buffer[1] = static_cast<std::uint8_t>(kSiGcController >> 16);
    buffer[2] = static_cast<std::uint8_t>(kSiGcController >> 8);
    return 3;
  }
  if (command == 0x40u) {
    if (expected_response_length < 8u) return -1;
    const std::uint32_t hi = PadHighWord(pad);
    const std::uint32_t lo = PadLowWord(pad, self->native_pad_mode_[channel]);
    for (unsigned i = 0; i < 4; ++i) {
      buffer[i] = static_cast<std::uint8_t>(hi >> (24u - i * 8u));
      buffer[4u + i] = static_cast<std::uint8_t>(lo >> (24u - i * 8u));
    }
    return 8;
  }
  if (command == 0x41u || command == 0x42u) {
    if (expected_response_length < 10u) return -1;
    buffer[0] = 0; buffer[1] = 0;
    buffer[2] = PadAxis(pad.stick_x); buffer[3] = PadAxis(pad.stick_y);
    buffer[4] = PadAxis(pad.sub_x); buffer[5] = PadAxis(pad.sub_y);
    buffer[6] = pad.left; buffer[7] = pad.right; buffer[8] = 0; buffer[9] = 0;
    return 10;
  }
  if (command == 0x1du) return 0;
  return -1;
}

void HostRuntime::NativePadDirect(void* user, std::uint32_t channel,
                                  std::uint32_t command, bool polling_enabled) {
  auto* self = static_cast<HostRuntime*>(user);
  if (!self || channel >= self->native_pad_mode_.size()) return;
  const std::uint8_t opcode = static_cast<std::uint8_t>(command >> 16);
  const std::uint8_t parameter2 = static_cast<std::uint8_t>(command >> 8);
  const std::uint8_t parameter1 = static_cast<std::uint8_t>(command);
  (void)parameter1;
  if (opcode != 0x40u) return;
  if (!polling_enabled) self->native_pad_mode_[channel] = parameter2;
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
  if (self->native_input_ready_)
    GekkoAOT::Input::HostInput().Rumble(channel, parameter1 <= 2u ? parameter1 : 0u);
#endif
}

PageTranslation HostRuntime::TranslateVmemAddress(std::uint32_t address, bool write) {
  using Status = PageTranslation::Status;
  if ((cpu_.msr & 0x10u) == 0u) return {Status::Mapped, address};
  const bool user = (cpu_.msr & kPpcMsrPr) != 0u;
  // BAT translation takes precedence over the hashed page table.
  for (unsigned index = 0; index < 4; ++index) {
    const auto upper = spr_state_[536u + index * 2u];
    const auto lower = spr_state_[537u + index * 2u];
    if ((upper & (user ? 1u : 2u)) == 0u) continue;
    const auto offset_mask = ((upper & 0x1ffcu) << 15u) | 0x1ffffu;
    if ((address & ~offset_mask) != (upper & 0xfffe0000u & ~offset_mask)) continue;
    const auto pp = lower & 3u;
    if (pp == 0u || (write && pp != 2u)) return {Status::Protection};
    return {Status::Mapped, (lower & 0xfffe0000u & ~offset_mask) | (address & offset_mask)};
  }
  return TranslatePage(memory_, address, cpu_.sr[address >> 28u], spr_state_[25u],
                       user, write ? PageAccess::Write : PageAccess::Read);
}

bool HostRuntime::AccessPagedVmem(std::uint32_t address, std::uint64_t* value,
                                  std::uint8_t size, bool write) {
  if (!memory_.PagedVmem() || !AddressSpace::IsVmem(address)) return false;
  if (size != 1u && size != 2u && size != 4u && size != 8u) {
    SetFault(write ? FaultKind::InvalidMemoryWrite : FaultKind::InvalidMemoryRead, address, size);
    return true;
  }
  // Preflight both pages of a scalar before writing any data. Contiguous
  // virtual pages need not have adjacent physical frames.
  std::array<std::uint8_t*, 8> bytes{};
  for (unsigned offset = 0; offset < size;) {
    const auto ea = address + offset;
    const auto translation = TranslateVmemAddress(ea, write);
    using Status = PageTranslation::Status;
    if (translation.status != Status::Mapped) {
      if (translation.status == Status::InvalidTable) {
        SetFault(write ? FaultKind::InvalidMemoryWrite : FaultKind::InvalidMemoryRead, ea, spr_state_[25u]);
        return true;
      }
      const auto old_msr = cpu_.msr;
      cpu_.dar = ea;
      cpu_.dsisr = (write ? 0x02000000u : 0u) |
          (translation.status == Status::Missing ? 0x40000000u :
           translation.status == Status::DirectStore ? 0x04000000u : 0x08000000u);
      cpu_.srr0 = cpu_.pc;
      cpu_.srr1 = old_msr & kPpcMsrRfiMask;
      cpu_.msr = ((old_msr & ~kPpcMsrLe) |
                  ((old_msr & kPpcMsrIle) ? kPpcMsrLe : 0u)) & ~kPpcExceptionEntryClearMask;
      cpu_.pc = (old_msr & 0x40u) ? 0xfff00300u : 0x300u;
      cpu_.exception |= kPpcExcDsi;
      BumpHostReadEpoch(&cpu_);
      return true;
    }
    const unsigned count = std::min<unsigned>(size - offset, 0x1000u - (ea & 0xfffu));
    // Page tables describe physical MEM1 frames. Do not admit cached aliases,
    // device apertures or the old fake backing as physical RAM.
    if (translation.physical >= memory_.Mem1().size() ||
        count > memory_.Mem1().size() - translation.physical) {
      SetFault(write ? FaultKind::InvalidMemoryWrite : FaultKind::InvalidMemoryRead,
               ea, translation.physical);
      return true;
    }
    for (unsigned i = 0; i < count; ++i)
      bytes[offset + i] = memory_.Mem1().data() + translation.physical + i;
    offset += count;
  }
  if (write) {
    for (unsigned i = 0; i < size; ++i) *bytes[i] = std::uint8_t(*value >> ((size - 1u - i) * 8u));
  } else {
    *value = 0;
    for (unsigned i = 0; i < size; ++i) *value = (*value << 8u) | *bytes[i];
  }
  return true;
}

std::uint64_t HostRuntime::ExternalRead(CPUState* cpu, std::uint32_t address, std::uint8_t size) {
  if (auto* self = Self(cpu)) {
    std::uint64_t value = 0;
    if (self->AccessPagedVmem(address, &value, size, false)) return value;

    // DolRecomp resolves the normal cached/uncached MEM1 aliases directly,
    // but low physical addresses used by Nintendo's exception/OS code fall
    // through to the external bus.  Treat any address that the standalone
    // AddressSpace can actually resolve as RAM before trying hardware MMIO.
    // This is especially important for the physical OSContext pointer stored
    // at low-memory 0xC0: first-level vectors run through the RAM executor,
    // then compiled handlers continue accessing that physical pointer.
    if (ReadAddressSpaceValue(self->memory_, address, size, &value)) return value;

    // Any access that leaves ordinary MEM1/MEM2 is a timing-visible host
    // boundary.  The in-module superchain checks this epoch after the current
    // native wrapper returns and yields to HostRuntime before chaining again.
    BumpHostReadEpoch(cpu);

    // Keep low-cost polling telemetry for black-screen diagnosis.  A tight
    // SDK hardware wait is valid guest code, so never terminate it here; the
    // final run result merely reports which MMIO register dominated the tail.
    ++self->mmio_reads_;
    if (address == self->last_mmio_read_)
      ++self->same_mmio_read_streak_;
    else {
      self->last_mmio_read_ = address;
      self->same_mmio_read_streak_ = 1;
    }

    if (self->lc_.Read(address, size, &value)) return value;
    if (self->cp_.Read(address, size, &value)) {
      self->SyncNativeCPInterrupt();
      return value;
    }
    if (self->pe_.Read(address, size, &value)) {
      self->SyncNativePEInterrupts();
      return value;
    }
    if (self->pi_.Read(address, size, &value)) return value;
    if (self->mi_.Read(address, size, &value)) return value;
    if (self->vi_.Read(address, size, &value)) return value;
    if (self->ai_.Read(address, size, &value)) {
      self->SyncNativeAIInterrupt();
      return value;
    }
    if (self->di_.Read(address, size, &value)) {
      self->SyncNativeDIInterrupt();
      return value;
    }
    if (self->exi_.ReadRegister(address, size, &value)) {
      self->SyncNativeEXIInterrupt();
      return value;
    }
    if (self->si_.Read(address, size, &value)) {
      self->SyncNativeSIInterrupt();
      return value;
    }
    if (self->dsp_.Read(address, size, &value)) {
      self->SyncNativeDSPInterrupt();
      return value;
    }
    const std::uint32_t physical = AddressSpace::ToPhysical(address);
    if (physical >= 0x08000000u && physical < 0x0c000000u) {
      // CPU-visible EFB aperture, matching the Flipper/Dolphin address layout:
      //   x = (addr & 0xfff) >> 2, y = (addr >> 12) & 0x3ff
      //   bit 22 clear -> color, bit 22 set -> depth
      //   bit 23 set   -> combined Z+color (still fail-closed)
      // v162 added depth for GXPeekZ; v163 closes the normal 32-bit color read
      // side for GXPeekARGB while keeping unknown aperture semantics explicit.
      if (size == 4u && (physical & 0x00800000u) == 0u && self->native_gx_.Ready()) {
        const std::uint16_t x =
            static_cast<std::uint16_t>((physical & 0x00000fffu) >> 2u);
        const std::uint16_t y =
            static_cast<std::uint16_t>((physical >> 12u) & 0x000003ffu);

        if ((physical & 0x00400000u) != 0u) {
          std::uint32_t z = 0u;
          if (self->native_gx_.PeekEfbZ(x, y, &z)) {
            static unsigned efb_z_logs = 0u;
            if (efb_z_logs++ < 32u)
              std::fprintf(stderr,
                           "GEKKOAOT_EFB_Z_PEEK_V162=1 ea=%08x physical=%08x x=%u y=%u z=%06x pc=%08x lr=%08x source=aurora-depth-snapshot\n",
                           address, physical, unsigned(x), unsigned(y),
                           z & 0x00ffffffu, cpu->pc, cpu->lr);
            return z & 0x00ffffffu;
          }
        } else {
          std::uint32_t argb = 0u;
          if (self->native_gx_.PeekEfbArgb(x, y, &argb)) {
            // PE_ALPHAREAD is CPU-visible state, so apply it here after the
            // renderer has reproduced the current EFB pixel-format precision.
            // 0=force 00, 1=force FF, 2=return the stored alpha.
            const std::uint16_t alpha_read = self->pe_.AlphaReadMode();
            if (alpha_read == 0u)
              argb &= 0x00ffffffu;
            else if (alpha_read == 1u)
              argb |= 0xff000000u;
            static unsigned efb_color_logs = 0u;
            if (efb_color_logs++ < 32u)
              std::fprintf(stderr,
                           "GEKKOAOT_EFB_COLOR_PEEK_V163=1 ea=%08x physical=%08x x=%u y=%u argb=%08x alpha_read=%u pc=%08x lr=%08x source=aurora-color-snapshot\n",
                           address, physical, unsigned(x), unsigned(y), argb,
                           unsigned(alpha_read), cpu->pc, cpu->lr);
            return argb;
          }
        }
      }

      static unsigned efb_read_logs = 0;
      if (efb_read_logs++ < 32u)
        std::fprintf(stderr,
                     "GEKKOAOT_EFB_APERTURE_V163=0 op=read ea=%08x physical=%08x size=%u pc=%08x lr=%08x reason=unsupported-cpu-efb-access action=fault\n",
                     address, physical, unsigned(size), cpu->pc, cpu->lr);
    }
    if (LooksLikeInvalidGameCubeRamAddress(self->memory_, address)) {
      std::fprintf(stderr,
                   "GEKKOAOT_INVALID_GUEST_EA_V86=1 op=read ea=%08x physical=%08x size=%u pc=%08x lr=%08x sp=%08x r3=%08x r4=%08x r9=%08x r10=%08x r11=%08x action=fault\n",
                   address, AddressSpace::ToPhysical(address), unsigned(size),
                   cpu->pc, cpu->lr, cpu->gpr[1], cpu->gpr[3], cpu->gpr[4],
                   cpu->gpr[9], cpu->gpr[10], cpu->gpr[11]);
      self->SetFault(FaultKind::InvalidMemoryRead, address, size);
    } else {
      self->SetFault(FaultKind::MmioRead, address);
    }
  }
  return 0;
}

bool HostRuntime::CpuFifoTargetsGpu() const {
  // GXBeginDisplayList swaps only the CPU FIFO (PI) to a temporary MEM1
  // recording buffer while the GP FIFO (CP) remains the render FIFO.  The two
  // FIFO objects therefore have different base/end pairs during DL capture.
  // Do not require GPLinkEnabled here: SDK code can briefly toggle link/read
  // bits while keeping the same CPU/GP FIFO object, and those writes still
  // belong to the live graphics stream in the current synchronous backend.
  return pi_.FifoBase() == cp_.FifoBase() && pi_.FifoEnd() == cp_.FifoEnd();
}

bool HostRuntime::CaptureWriteGatherToCpuFifo(std::uint64_t value, std::uint8_t size) {
  if (size != 1u && size != 2u && size != 4u && size != 8u) return false;

  const std::uint32_t base = pi_.FifoBase();
  const std::uint32_t end = pi_.FifoEnd();
  if (base == 0u && end == 0u) return false;

  // There is one physical write-gather pipe, regardless of which PI FIFO is
  // selected. GXBeginDisplayList switches PI before resetting WPAR; without
  // that reset, residual bytes reach the FIFO selected when a line completes.
  if (gpu_fifo_gather_size_ != 0u) {
    if (cpu_fifo_gather_size_ != 0u) {
      SetFault(FaultKind::MmioWrite, 0x0c008000u, gpu_fifo_gather_size_);
      return true;
    }
    std::copy_n(gpu_fifo_gather_.begin(), gpu_fifo_gather_size_, cpu_fifo_gather_.begin());
    cpu_fifo_gather_size_ = gpu_fifo_gather_size_;
    gpu_fifo_gather_size_ = 0u;
  }
  if (cpu_fifo_gather_size_ != 0u &&
      (cpu_fifo_gather_base_ != base || cpu_fifo_gather_end_ != end)) {
    if (const char* setting = std::getenv("GEKKOAOT_NATIVE_WPAR_TRACE");
        setting && setting[0] == '1' && setting[1] == '\0')
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_WG_SWITCH_V105=1 bytes=%u old=%08x-%08x new=%08x-%08x\n",
                   cpu_fifo_gather_size_, cpu_fifo_gather_base_, cpu_fifo_gather_end_,
                   base, end);
  }
  cpu_fifo_gather_base_ = base;
  cpu_fifo_gather_end_ = end;

  for (std::uint8_t i = 0; i < size; ++i) {
    cpu_fifo_gather_[cpu_fifo_gather_size_++] =
        static_cast<std::uint8_t>(value >> ((size - 1u - i) * 8u));

    if (cpu_fifo_gather_size_ != cpu_fifo_gather_.size()) continue;

    const std::uint32_t write_pointer = pi_.FifoWritePointer();
    auto* destination = memory_.Resolve(write_pointer, cpu_fifo_gather_.size());
    if (!destination) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_DL_CAPTURE_ERROR=1 reason=unmapped-cpu-fifo write=%08x base=%08x end=%08x\n",
                   write_pointer, base, end);
      SetFault(FaultKind::MmioWrite, write_pointer, 32u);
      cpu_fifo_gather_size_ = 0u;
      return true;
    }

    std::memcpy(destination, cpu_fifo_gather_.data(), cpu_fifo_gather_.size());
    pi_.AdvanceCpuFifoWritePointer();
    ++cpu_fifo_capture_bursts_;
    cpu_fifo_gather_size_ = 0u;

    if (!cpu_fifo_capture_logged_) {
      cpu_fifo_capture_logged_ = true;
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_DL_CAPTURE_V1=1 mode=write-gather-to-cpu-fifo base=%08x end=%08x first_write=%08x burst=32\n",
                   base, end, write_pointer);
    }
  }

  return true;
}

bool HostRuntime::ConsumeGpuFifoBurst(void* user, std::uint32_t address) {
  auto* self = static_cast<HostRuntime*>(user);
  if (!self->native_gx_.Ready()) return true;
  const auto* bytes = self->memory_.Resolve(address, 32u);
  if (!bytes || !self->native_gx_.WriteBurst(bytes, 32u, self->cpu_.pc)) {
    self->SetFault(FaultKind::MmioWrite, address, 32u);
    return false;
  }
  return true;
}

bool HostRuntime::QueueGpuFifoGather(std::uint64_t value, std::uint8_t size,
                                     std::uint32_t guest_pc) {
  if (size != 1u && size != 2u && size != 4u && size != 8u) return false;
  if (!native_gx_.Ready()) return false;
  if (gpu_fifo_gather_size_ == 0u) gpu_fifo_first_pc_ = guest_pc;

  for (std::uint8_t i = 0; i < size; ++i) {
    gpu_fifo_gather_[gpu_fifo_gather_size_++] =
        static_cast<std::uint8_t>(value >> ((size - 1u - i) * 8u));
    if (gpu_fifo_gather_size_ != gpu_fifo_gather_.size()) continue;

    // A completed WG line first reaches the PI CPU FIFO in MEM1. CP alone
    // decides when that line can be consumed: GPRead and breakpoints gate
    // renderer effects, not merely the guest-visible pointer accounting.
    if (pi_.FifoBase() != 0u || pi_.FifoEnd() != 0u) {
      const auto destination = pi_.FifoWritePointer();
      auto* bytes = memory_.Resolve(destination, gpu_fifo_gather_.size());
      if (!bytes) {
        gpu_fifo_gather_size_ = 0u;
        SetFault(FaultKind::MmioWrite, destination, 32u);
        return true;
      }
      std::memcpy(bytes, gpu_fifo_gather_.data(), gpu_fifo_gather_.size());
      pi_.AdvanceCpuFifoWritePointer();
      if (cp_.NotifyGatherWrite(32u)) SyncNativeCPInterrupt();
    } else {
      // Retain the pre-FIFO direct bridge path for hosts without a configured
      // retail PI FIFO. Once configured, all consumption uses the CP path.
      if (!native_gx_.WriteBurst(gpu_fifo_gather_.data(), 32u, gpu_fifo_first_pc_)) {
        gpu_fifo_gather_size_ = 0u;
        SetFault(FaultKind::MmioWrite, 0x0c008000u, 32u);
        return true;
      }
    }
    ++gpu_fifo_bursts_;
    gpu_fifo_gather_size_ = 0u;
    if (!gpu_fifo_burst_logged_) {
      gpu_fifo_burst_logged_ = true;
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_GPU_WG_BURST_V13=1 burst=32 abi=bulk mode=host-gather\n");
    }
  }
  return true;
}

void HostRuntime::ExternalWrite(CPUState* cpu, std::uint32_t address, std::uint64_t value,
                                std::uint8_t size) {
  if (auto* self = Self(cpu)) {
    // v136b hot path: WGPIPE is a fixed MMIO aperture and cannot alias MEM1,
    // MEM2, paged VMEM or locked cache. Most draw-heavy ExternalWrite calls
    // land here, so handle it before generic address-space probes.
    // Partial lines remain inside the native superchain; only a complete
    // 32-byte gather line becomes a host-visible CP/GPU boundary.
    if (GekkoAOT::GX::HostBridge::IsWriteGatherPipe(address)) {
      if (size == 4u && value == 0xc274d554u &&
          std::getenv("GEKKOAOT_GX_DIAG_GLYPH_PC"))
        std::fprintf(stderr, "GEKKOAOT_GX_DIAG_GLYPH_PC pc=%08x value=%08llx gather=%u pi_write=%08x\n",
                     cpu->pc, static_cast<unsigned long long>(value),
                     self->gpu_fifo_gather_size_, self->pi_.FifoWritePointer());
      if (size == 4u && cpu->pc == 0x800228c0u &&
          (value == 0xc274d554u || value == 0x80000000u) &&
          std::getenv("GEKKOAOT_GX_DIAG_GLYPH_STATE")) {
        static bool seen_normal = false, seen_zero = false;
        bool& seen = value == 0xc274d554u ? seen_normal : seen_zero;
        if (!seen) {
          seen = true;
          const std::uint32_t object = cpu->gpr[31];
          const auto* saved_lr_bytes = self->memory_.Resolve(cpu->gpr[1] + 180u, 4u);
          const std::uint32_t saved_lr = saved_lr_bytes
              ? (std::uint32_t(saved_lr_bytes[0]) << 24u) |
                    (std::uint32_t(saved_lr_bytes[1]) << 16u) |
                    (std::uint32_t(saved_lr_bytes[2]) << 8u) | saved_lr_bytes[3]
              : 0u;
          std::fprintf(stderr, "GEKKOAOT_GX_DIAG_GLYPH_STATE pc=%08x x=%08llx object=%08x sp=%08x caller=%08x r20=%08x r23=%08x r29=%08x r30=%08x f26=%.9g f27=%.9g f30=%.9g f31=%.9g bytes=",
                       cpu->pc, static_cast<unsigned long long>(value), object, cpu->gpr[1], saved_lr,
                       cpu->gpr[20], cpu->gpr[23], cpu->gpr[29], cpu->gpr[30],
                       cpu->fpr[26], cpu->fpr[27], cpu->fpr[30], cpu->fpr[31]);
          const auto* object_bytes = self->memory_.Resolve(object, 112u);
          if (object_bytes) for (unsigned i = 0; i < 112u; ++i)
            std::fprintf(stderr, "%02x", object_bytes[i]);
          std::fprintf(stderr, " caller_bytes=");
          const auto* caller_bytes = self->memory_.Resolve(cpu->gpr[20], 64u);
          if (caller_bytes) for (unsigned i = 0; i < 64u; ++i)
            std::fprintf(stderr, "%02x", caller_bytes[i]);
          std::fprintf(stderr, " caller_stack=");
          const auto* caller_stack = self->memory_.Resolve(cpu->gpr[1] + 176u + 32u, 40u);
          if (caller_stack) for (unsigned i = 0; i < 40u; ++i)
            std::fprintf(stderr, "%02x", caller_stack[i]);
          std::fprintf(stderr, "\n");
        }
      }
      // The CPU write-gather pipe always targets the currently selected PI CPU
      // FIFO.  During GXBeginDisplayList that FIFO is a temporary RAM buffer,
      // not the CP/GP render FIFO.  Record complete 32-byte gather bursts into
      // MEM1 and leave Aurora untouched; GXCallDisplayList will later replay
      // the recorded bytes through the native CALL_DL path.
      if (!self->CpuFifoTargetsGpu()) {
        const std::uint64_t before = self->cpu_fifo_capture_bursts_;
        if (self->CaptureWriteGatherToCpuFifo(value, size)) {
          if (self->cpu_fifo_capture_bursts_ != before) BumpHostWriteEpoch(cpu);
          return;
        }
      } else if (self->cpu_fifo_gather_size_ != 0u) {
        if (self->gpu_fifo_gather_size_ != 0u) {
          self->SetFault(FaultKind::MmioWrite, address, self->cpu_fifo_gather_size_);
          return;
        }
        std::copy_n(self->cpu_fifo_gather_.begin(), self->cpu_fifo_gather_size_,
                    self->gpu_fifo_gather_.begin());
        self->gpu_fifo_gather_size_ = self->cpu_fifo_gather_size_;
        self->gpu_fifo_first_pc_ = cpu->pc;
        self->cpu_fifo_gather_size_ = 0u;
      }

      const std::uint64_t before = self->gpu_fifo_bursts_;
      if (self->QueueGpuFifoGather(value, size, cpu->pc)) {
        // A complete write-gather line is the actual hardware visibility
        // boundary. Partial lines stay inside the native superchain.
        if (self->gpu_fifo_bursts_ != before) BumpHostWriteEpoch(cpu);
        return;
      }
    }
    if (self->AccessPagedVmem(address, &value, size, true)) return;
    // See ExternalRead(): compiled OS interrupt handlers may carry physical
    // MEM1 pointers out of the low-memory vector while address translation is
    // being restored.  Keep those accesses in RAM instead of misclassifying
    // them as MMIO writes.
    if (WriteAddressSpaceValue(self->memory_, address, value, size)) return;
    if (self->lc_.Write(address, value, size)) {
      BumpHostWriteEpoch(cpu);
      return;
    }

    BumpHostWriteEpoch(cpu);
    if (self->cp_.Write(address, value, size)) {
      self->SyncNativeCPInterrupt();
      return;
    }
    if (self->pe_.Write(address, value, size)) {
      self->SyncNativePEInterrupts();
      return;
    }
    if (self->pi_.Write(address, value, size)) return;
    if (self->mi_.Write(address, value, size)) return;
    if (self->vi_.Write(address, value, size)) {
      if (self->vi_.ConsumeInterruptUpdate()) self->SyncNativeVIInterrupt();
      return;
    }
    if (self->ai_.Write(address, value, size)) {
      self->SyncNativeAIInterrupt();
      return;
    }
    if (self->di_.Write(address, value, size)) {
      if (self->di_async_completion_pending_ &&
          (self->di_.DMAControl() & 1u) == 0u) {
        self->di_async_completion_pending_ = false;
        self->di_async_cycles_remaining_ = 0u;
        self->di_async_staging_.clear();
        static unsigned cancel_logs = 0u;
        if (cancel_logs++ < 32u)
          std::fprintf(stderr,
                       "GEKKOAOT_NATIVE_DI_ASYNC_V98=1 phase=cancel-immediate seq=%llu pc=%08x\n",
                       static_cast<unsigned long long>(self->di_async_sequence_), cpu->pc);
      }
      self->SyncNativeDIInterrupt();
      return;
    }
    if (self->exi_.WriteRegister(address, value, size)) {
      self->SyncNativeEXIInterrupt();
      return;
    }
    if (self->si_.Write(address, value, size)) {
      self->SyncNativeSIInterrupt();
      return;
    }
    if (self->dsp_.Write(address, value, size)) {
      self->SyncNativeDSPInterrupt();
      return;
    }
    const std::uint32_t physical = AddressSpace::ToPhysical(address);
    if (physical >= 0x08000000u && physical < 0x0c000000u) {
      static unsigned efb_write_logs = 0;
      if (efb_write_logs++ < 32u)
        std::fprintf(stderr,
                     "GEKKOAOT_EFB_APERTURE_V87=0 op=write ea=%08x physical=%08x size=%u value=%016llx pc=%08x lr=%08x reason=cpu-efb-access-not-implemented action=fault\n",
                     address, physical, unsigned(size),
                     static_cast<unsigned long long>(value), cpu->pc, cpu->lr);
    }
    if (LooksLikeInvalidGameCubeRamAddress(self->memory_, address)) {
      std::fprintf(stderr,
                   "GEKKOAOT_INVALID_GUEST_EA_V86=1 op=write ea=%08x physical=%08x size=%u value=%016llx pc=%08x lr=%08x sp=%08x r3=%08x r4=%08x r9=%08x r10=%08x r11=%08x action=fault\n",
                   address, AddressSpace::ToPhysical(address), unsigned(size),
                   static_cast<unsigned long long>(value), cpu->pc, cpu->lr,
                   cpu->gpr[1], cpu->gpr[3], cpu->gpr[4], cpu->gpr[9],
                   cpu->gpr[10], cpu->gpr[11]);
      self->SetFault(FaultKind::InvalidMemoryWrite, address,
                     static_cast<std::uint32_t>(value));
    } else {
      self->SetFault(FaultKind::MmioWrite, address, static_cast<std::uint32_t>(value));
    }
  }
}

std::uint32_t HostRuntime::ExternalRead32(CPUState* cpu, std::uint32_t address, std::uint8_t) {
  return static_cast<std::uint32_t>(ExternalRead(cpu, address, 4));
}

void HostRuntime::ExternalWrite32(CPUState* cpu, std::uint32_t address, std::uint32_t value,
                                  std::uint8_t) {
  ExternalWrite(cpu, address, value, 4);
}

void* HostRuntime::ExternalPointer(CPUState* cpu, std::uint32_t address, std::uint32_t size) {
  auto* self = Self(cpu);
  if (!self) return nullptr;
  // v136: essentially every NativeOS/SDK pointer lives in MEM1/MEM2. Resolve
  // normal RAM first so the hot path avoids probing the locked-cache aperture
  // on every guest structure read. LC addresses do not alias AddressSpace, so
  // this preserves the exact fallback semantics for 0xE0000000..0xE003FFFF.
  if (void* pointer = self->memory_.Resolve(address, size)) return pointer;
  return self->lc_.Pointer(address, size);
}

void HostRuntime::InstructionFallback(CPUState* cpu, std::uint32_t instruction,
                                      std::uint32_t address) {
  auto* self = Self(cpu);
  if (!self) return;

  // Keep the standalone fallback useful as a correctness safety net for AOT
  // backends that emitted ppc_fallback_instruction() for floating-point memory
  // operations.  These instructions are still executed natively here; there is
  // no PowerPC interpreter behind this path.  Current pinned DolRecomp lowers
  // them directly, so a clean/fresh module should normally never hit this code.
  //
  // This also makes stale modules deterministic instead of spinning forever at
  // e.g. NDDEMO 0x80062AC0 (lfs f0,0(r4)).
  const auto read32 = [&](std::uint32_t ea, std::uint32_t* value) {
    if (self->memory_.Read32(ea, value)) return true;
    *value = static_cast<std::uint32_t>(ExternalRead(cpu, ea, 4u));
    return self->fault_.kind == FaultKind::None && cpu->exception == 0u;
  };
  const auto read64 = [&](std::uint32_t ea, std::uint64_t* value) {
    if (self->memory_.Read64(ea, value)) return true;
    *value = ExternalRead(cpu, ea, 8u);
    return self->fault_.kind == FaultKind::None && cpu->exception == 0u;
  };
  const auto write32 = [&](std::uint32_t ea, std::uint32_t value) {
    if (self->memory_.Write32(ea, value)) return true;
    ExternalWrite(cpu, ea, value, 4u);
    return self->fault_.kind == FaultKind::None && cpu->exception == 0u;
  };
  const auto write64 = [&](std::uint32_t ea, std::uint64_t value) {
    if (self->memory_.Write64(ea, value)) return true;
    ExternalWrite(cpu, ea, value, 8u);
    return self->fault_.kind == FaultKind::None && cpu->exception == 0u;
  };
  const auto load_float = [&](std::uint32_t fr, std::uint32_t ea, bool single) {
    if (single) {
      std::uint32_t bits = 0;
      if (!read32(ea, &bits)) return false;
      float value = 0.0f;
      std::memcpy(&value, &bits, sizeof(value));
      const double expanded = static_cast<double>(value);
      cpu->fpr[fr] = expanded;
      // Gekko lfs fills both halves of the paired-single register.
      cpu->ps1[fr] = expanded;
      return true;
    }
    std::uint64_t bits = 0;
    if (!read64(ea, &bits)) return false;
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    // lfd writes PS0 only; PS1 is architecturally preserved.
    cpu->fpr[fr] = value;
    return true;
  };
  const auto store_float = [&](std::uint32_t fr, std::uint32_t ea, bool single) {
    if (single) {
      const float value = static_cast<float>(cpu->fpr[fr]);
      std::uint32_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      return write32(ea, bits);
    }
    std::uint64_t bits = 0;
    std::memcpy(&bits, &cpu->fpr[fr], sizeof(bits));
    return write64(ea, bits);
  };
  const auto finish_fp_memory = [&](std::uint32_t fr, std::uint32_t ra,
                                    std::uint32_t ea, bool load, bool single,
                                    bool update) {
    const bool ok = load ? load_float(fr, ea, single)
                         : store_float(fr, ea, single);
    if (!ok) return true;
    if (update) cpu->gpr[ra] = ea;
    cpu->pc = address + 4u;
    return true;
  };

  const std::uint32_t primary = instruction >> 26;
  if (primary >= 48u && primary <= 55u) {
    const std::uint32_t fr = (instruction >> 21) & 31u;
    const std::uint32_t ra = (instruction >> 16) & 31u;
    const bool update = (primary & 1u) != 0u;
    const bool load = primary <= 51u;
    const bool single = primary == 48u || primary == 49u ||
                        primary == 52u || primary == 53u;
    if (update && ra == 0u) {
      self->SetFault(FaultKind::InstructionFallback, address, instruction);
      return;
    }
    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::int16_t>(instruction & 0xffffu));
    const std::uint32_t base = ra == 0u && !update ? 0u : cpu->gpr[ra];
    const std::uint32_t ea = base + static_cast<std::uint32_t>(displacement);
    (void)finish_fp_memory(fr, ra, ea, load, single, update);
    return;
  }

  if (primary == kPpcPrimaryX && (instruction & 1u) == 0u) {
    const std::uint32_t xo = (instruction >> 1) & 0x3ffu;
    const std::uint32_t fr = (instruction >> 21) & 31u;
    const std::uint32_t ra = (instruction >> 16) & 31u;
    const std::uint32_t rb = (instruction >> 11) & 31u;
    bool recognized = true;
    bool load = false;
    bool single = false;
    bool update = false;
    switch (xo) {
    case 535u: load = true; single = true; break;       // lfsx
    case 567u: load = true; single = true; update = true; break;  // lfsux
    case 599u: load = true; break;                      // lfdx
    case 631u: load = true; update = true; break;       // lfdux
    case 663u: single = true; break;                    // stfsx
    case 695u: single = true; update = true; break;     // stfsux
    case 727u: break;                                   // stfdx
    case 759u: update = true; break;                    // stfdux
    case 983u: {                                        // stfiwx
      const std::uint32_t base = ra == 0u ? 0u : cpu->gpr[ra];
      const std::uint32_t ea = base + cpu->gpr[rb];
      std::uint64_t bits = 0;
      std::memcpy(&bits, &cpu->fpr[fr], sizeof(bits));
      if (write32(ea, static_cast<std::uint32_t>(bits))) cpu->pc = address + 4u;
      return;
    }
    default: recognized = false; break;
    }
    if (recognized) {
      if (update && ra == 0u) {
        self->SetFault(FaultKind::InstructionFallback, address, instruction);
        return;
      }
      const std::uint32_t base = ra == 0u && !update ? 0u : cpu->gpr[ra];
      const std::uint32_t ea = base + cpu->gpr[rb];
      (void)finish_fp_memory(fr, ra, ea, load, single, update);
      return;
    }
  }

  // Indirect branches can land on an instruction inside an AOT function
  // instead of one of its compiled CFG entry labels. DolRecomp routes such
  // interior entries through this callback, even when the opcode itself is
  // fully supported. Execute the XL branch directly so a table target that
  // points at a standalone blr can return to its caller.
  if (primary == 19u) {
    const std::uint32_t xo = (instruction >> 1) & 0x3ffu;
    const std::uint32_t bo = (instruction >> 21) & 31u;
    if (xo == 16u || (xo == 528u && (bo & 4u) != 0u)) { // bclr / bcctr
      const std::uint32_t bi = (instruction >> 16) & 31u;
      const std::uint32_t target = (xo == 16u ? cpu->lr : cpu->ctr) & ~3u;
      bool ctr_ok = true;
      if ((bo & 4u) == 0u) {
        cpu->ctr -= 1u;
        ctr_ok = (cpu->ctr != 0u) != ((bo & 2u) != 0u);
      }
      const bool cr_ok = (bo & 16u) != 0u ||
                         CrBit(cpu, bi) == ((bo & 8u) != 0u);
      if ((instruction & 1u) != 0u) cpu->lr = address + 4u;
      cpu->pc = ctr_ok && cr_ok ? target : address + 4u;
      return;
    }
  }

  std::uint8_t cache_operation = 0;
  std::uint32_t effective_address = 0;
  if (DecodeCacheControl(instruction, cpu, &cache_operation, &effective_address)) {
    CacheControl(cpu, cache_operation, effective_address, address);
    // DolRecomp Portable-C returns immediately after ppc_fallback_instruction(),
    // so resume at the following guest instruction just like a one-step
    // interpreter fallback would do.
    cpu->pc = address + 4u;
    return;
  }

  bool spr_write = false;
  std::uint16_t spr = 0;
  std::uint8_t gpr = 0;
  if (DecodeSprTransfer(instruction, &spr_write, &spr, &gpr)) {
    if (spr_write) {
      SprWrite(cpu, spr, cpu->gpr[gpr], address);
    } else {
      cpu->gpr[gpr] = SprRead(cpu, spr, address);
    }
    if (self->fault_.kind == FaultKind::None) cpu->pc = address + 4u;
    return;
  }

  self->SetFault(FaultKind::InstructionFallback, address, instruction);
}

void HostRuntime::RebuildNativeInterceptIndex() {
  native_intercepts_.clear();
  native_intercepts_.reserve(os_hooks_.size() + simple_hle_hooks_.size());
  for (const auto& [address, _] : os_hooks_) native_intercepts_.push_back(address);
  for (const auto& [address, _] : simple_hle_hooks_) native_intercepts_.push_back(address);
  std::sort(native_intercepts_.begin(), native_intercepts_.end());
  native_intercepts_.erase(std::unique(native_intercepts_.begin(), native_intercepts_.end()),
                           native_intercepts_.end());
}

bool HostRuntime::DispatchOsHle(GekkoAOT::NativeOS::Kind kind, std::uint32_t address) {
  ++hle_calls_;

  // GEKKOAOT_NATIVE_OS_INTERRUPT_ENTRY_FENCE_V149:
  // A statically lowered NativeOS leaf can be entered from inside a compiled
  // native call chain without first returning to HostRuntime::Run(), so the
  // normal dispatch-boundary asynchronous-interrupt probe is skipped.  That is
  // observable for the three MSR[EE] helpers: OSDisableInterrupts in particular
  // may clear EE before an already-pending external/DEC interrupt gets the
  // architectural chance it would have had at the guest function-entry
  // boundary.
  //
  // The direct-HLE state-sync path has already published PC as the intercepted
  // guest entrypoint.  If an interrupt is pending, take it now and report the
  // host call handled without executing the leaf.  SRR0 therefore points back
  // at the SDK entry and RFI naturally retries it, matching the guest path and
  // preserving the OSDisableInterrupts RAS semantics.  Apply the same fence to
  // Enable/Restore because callers may enter either while EE is already set.
  if (kind == GekkoAOT::NativeOS::Kind::DisableInterrupts ||
      kind == GekkoAOT::NativeOS::Kind::EnableInterrupts ||
      kind == GekkoAOT::NativeOS::Kind::RestoreInterrupts) {
    if (TryTakeExternalInterrupt() || TryTakeDecrementerInterrupt())
      return true;
  }

  auto& native_os = GekkoAOT::NativeOS::Service::Get();
  if (GekkoAOT::NativeOS::Service::IsInlineFastKind(kind)) {
    if (native_os.DispatchInlineFast(kind, &cpu_)) return true;
  } else if (native_os.Dispatch(kind, &cpu_)) {
    return true;
  }

  // Native mode is fail-closed: there is no guest/Dolphin scheduler fallback.
  // Blocking with an empty run queue is handled by NativeOS::EnterIdle(); a
  // false return now means a genuine native scheduler/ABI failure.
  SetFault(FaultKind::HleFailure, address);
  return false;
}

bool HostRuntime::DispatchSimpleHle(SimpleHleKind kind, std::uint32_t address) {
  ++hle_calls_;
  const auto finish=[&]() { cpu_.pc=cpu_.lr; return true; };
  const auto pointer=[&](std::uint32_t guest,std::uint32_t size)->std::uint8_t* {
    static std::uint8_t zero_length_sentinel=0;
    return size==0 ? &zero_length_sentinel : memory_.Resolve(guest,size);
  };
  switch(kind) {
  case SimpleHleKind::OsGetTime: return GekkoAOT::Time::GetTime(&cpu_);
  case SimpleHleKind::OsGetTick: return GekkoAOT::Time::GetTick(&cpu_);
  case SimpleHleKind::Memcpy: {
    const std::uint32_t dst_addr=cpu_.gpr[3],src_addr=cpu_.gpr[4],size=cpu_.gpr[5];
    auto* dst=pointer(dst_addr,size); auto* src=pointer(src_addr,size);
    if(!dst||!src){SetFault(FaultKind::HleFailure,address);return false;}
    if(size) std::memcpy(dst,src,size);
    cpu_.gpr[3]=dst_addr;
    return finish();
  }
  case SimpleHleKind::Memmove: {
    const std::uint32_t dst_addr=cpu_.gpr[3],src_addr=cpu_.gpr[4],size=cpu_.gpr[5];
    auto* dst=pointer(dst_addr,size); auto* src=pointer(src_addr,size);
    if(!dst||!src){SetFault(FaultKind::HleFailure,address);return false;}
    if(size) std::memmove(dst,src,size);
    cpu_.gpr[3]=dst_addr;
    return finish();
  }
  case SimpleHleKind::Memset: {
    const std::uint32_t dst_addr=cpu_.gpr[3],size=cpu_.gpr[5]; auto* dst=pointer(dst_addr,size);
    if(!dst){SetFault(FaultKind::HleFailure,address);return false;}
    if(size) std::memset(dst,static_cast<int>(cpu_.gpr[4]&0xffu),size);
    cpu_.gpr[3]=dst_addr; return finish();
  }
  case SimpleHleKind::Memcmp: {
    const std::uint32_t lhs_addr=cpu_.gpr[3],rhs_addr=cpu_.gpr[4],size=cpu_.gpr[5];
    auto* lhs=pointer(lhs_addr,size); auto* rhs=pointer(rhs_addr,size);
    if(!lhs||!rhs){SetFault(FaultKind::HleFailure,address);return false;}
    std::int32_t result=0; if(size) result=std::memcmp(lhs,rhs,size);
    cpu_.gpr[3]=static_cast<std::uint32_t>(result); return finish();
  }
  case SimpleHleKind::Bzero: {
    const std::uint32_t dst_addr=cpu_.gpr[3],size=cpu_.gpr[4]; auto* dst=pointer(dst_addr,size);
    if(!dst){SetFault(FaultKind::HleFailure,address);return false;}
    if(size) std::memset(dst,0,size);
    return finish();
  }
  }
  return false;
}

bool HostRuntime::DispatchHostCall(std::uint32_t address) {
  // v70: resolved SDK calls can be lowered by DolRecomp straight to a stable
  // service token.  No runtime address map, interception query, or redispatch
  // through module_dispatch is required for the positive path.  cpu_.pc still
  // contains the real guest entrypoint and is retained for fault diagnostics.
  if ((address & kStaticDirectMask) == kStaticDirectOsBase) {
    const auto raw = static_cast<std::uint8_t>(address & 0xffu);
    if (raw < static_cast<std::uint8_t>(GekkoAOT::NativeOS::Kind::Count))
      return DispatchOsHle(static_cast<GekkoAOT::NativeOS::Kind>(raw), cpu_.pc);
    return false;
  }
  if ((address & kStaticDirectMask) == kStaticDirectSimpleBase) {
    const auto raw = static_cast<std::uint8_t>(address & 0xffu);
    if (raw <= static_cast<std::uint8_t>(SimpleHleKind::Bzero))
      return DispatchSimpleHle(static_cast<SimpleHleKind>(raw), cpu_.pc);
    return false;
  }

  // v87: generated modules use this host token only when their local indirect
  // dispatcher cannot resolve a bctr/bctrl target. Resolve the target against
  // the complete active AOT set so callbacks and virtual calls may cross fixed
  // DOL/ELF images (and live RELs) without falling back to an emulator.
  if (address == kGlobalIndirectProbe) {
    return DispatchGlobalIndirect(cpu_.external_addr);
  }

  // REL side modules use this sentinel only for canonical->runtime translation.
  if (address == kRelAddressProbe) {
    const std::uint32_t canonical=cpu_.external_addr;
    std::uint32_t runtime=canonical;
    if(TranslateCanonicalRelAddress(canonical,&runtime)) {
      cpu_.external_value=runtime; cpu_.external_rid=kRelAddressHandled;
      ++rel_address_translations_; rel_last_canonical_=canonical; rel_last_runtime_=runtime;
      return true;
    }
    ++rel_address_misses_; rel_last_canonical_=canonical; rel_last_runtime_=canonical;
    return false;
  }

  // Static-intercept AOT removes this probe from every region that cannot
  // contain a native SDK/HLE entry. The few remaining guarded chunks use the
  // exact sorted entrypoint set, so there is no hot hash/cache layer anymore.
  if (address == kNativeRegionProbe) {
    const std::uint32_t start=cpu_.external_addr,end=cpu_.external_value;
    const auto it=std::lower_bound(native_intercepts_.begin(), native_intercepts_.end(), start);
    const bool blocked=start<end && it!=native_intercepts_.end() && *it<end;
    cpu_.external_rid=0xffu;
    return blocked;
  }

  // Exact direct-mapped positive cache. Hot OS/libc HLE no longer pays an
  // unordered_map lookup at every call; collisions simply fall through to the
  // authoritative maps below and refresh this slot.
  const auto fast_slot=((address>>2u)^(address>>13u))&(kHleFastCacheSize-1u);
  const auto fast=hle_fast_cache_[fast_slot];
  if(fast.address==address) {
    if(fast.type==HleFastType::Os)
      return DispatchOsHle(static_cast<GekkoAOT::NativeOS::Kind>(fast.kind),address);
    if(fast.type==HleFastType::Simple)
      return DispatchSimpleHle(static_cast<SimpleHleKind>(fast.kind),address);
  }

  const auto negative_slot=((address>>2u)^(address>>14u))&(kHostCallNegativeCacheSize-1u);
  if(hostcall_negative_cache_[negative_slot]==address) return false;

  if(auto it=os_hooks_.find(address);it!=os_hooks_.end()) {
    hle_fast_cache_[fast_slot]={address,HleFastType::Os,static_cast<std::uint8_t>(it->second)};
    return DispatchOsHle(it->second,address);
  }
  if(auto it=simple_hle_hooks_.find(address);it!=simple_hle_hooks_.end()) {
    hle_fast_cache_[fast_slot]={address,HleFastType::Simple,static_cast<std::uint8_t>(it->second)};
    return DispatchSimpleHle(it->second,address);
  }
  hostcall_negative_cache_[negative_slot]=address;
  return false;
}

bool HostRuntime::HostCall(CPUState* cpu, std::uint32_t address) {
  auto* self = Self(cpu);
  if (!self) return false;
  const bool handled = self->DispatchHostCall(address);
  // Compiler/runtime admission probes are not guest-visible events.  Real HLE
  // calls are: stop the native superchain after the current wrapper so timer
  // and interrupt state is settled before more guest work runs.
  if (handled && address != kNativeRegionProbe && address != kRelAddressProbe &&
      address != kGlobalIndirectProbe)
    BumpHostWriteEpoch(cpu);
  return handled;
}


bool HostRuntime::DispatchGlobalIndirect(std::uint32_t runtime_address) {
  if (!runtime_address) return false;

  // An indirect SDK call can arrive here too. Prefer the normal HLE surface
  // before entering native code so bctrl through a function pointer keeps the
  // same semantics as a directly lowered SDK call.
  if (runtime_address != kGlobalIndirectProbe && DispatchHostCall(runtime_address)) {
    ++global_indirect_dispatches_;
    return true;
  }

  bool overlap = false;
  if (const auto* fixed = SelectFixedExecutableModule(runtime_address, &overlap)) {
    if (fixed->dispatch && fixed->dispatch(&cpu_, runtime_address)) {
      ++global_indirect_dispatches_;
      static unsigned logs = 0;
      if (logs++ < 96u)
        std::fprintf(stderr,
                     "GEKKOAOT_GLOBAL_INDIRECT_V87=1 target=%08x route=fixed-overlap count=%llu\\n",
                     runtime_address,
                     static_cast<unsigned long long>(global_indirect_dispatches_));
      return true;
    }
  }

  // Fast non-overlap main executable path.
  const auto* main = module_.Descriptor();
  if (main && main->dispatch &&
      AddressInRanges(main->code_ranges, main->num_code_ranges, runtime_address) &&
      main->dispatch(&cpu_, runtime_address)) {
    ++global_indirect_dispatches_;
    return true;
  }

  // Resolve live REL mappings first, then fixed secondary coverage. The nested
  // dispatch deliberately does not advance host devices here: its guest cycles
  // accumulate in CPUState::downcount and are charged by the outer dispatch
  // boundary, exactly like an intra-module indirect call.
  const GekkoAOTModuleDesc* secondary = nullptr;
  std::uint32_t linked = runtime_address;
  if (ResolveSecondaryAddress(runtime_address, &secondary, &linked) && secondary &&
      secondary->dispatch && secondary->dispatch(&cpu_, linked)) {
    cpu_.pc = TranslateSecondaryAddress(secondary, cpu_.pc);
    ++secondary_dispatches_;
    ++global_indirect_dispatches_;
    static unsigned logs = 0;
    if (logs++ < 96u)
      std::fprintf(stderr,
                   "GEKKOAOT_GLOBAL_INDIRECT_V87=1 target=%08x linked=%08x route=secondary count=%llu\\n",
                   runtime_address, linked,
                   static_cast<unsigned long long>(global_indirect_dispatches_));
    return true;
  }

  static unsigned miss_logs = 0;
  if (miss_logs++ < 64u)
    std::fprintf(stderr,
                 "GEKKOAOT_GLOBAL_INDIRECT_V87=0 target=%08x pc=%08x lr=%08x action=unresolved\\n",
                 runtime_address, cpu_.pc, cpu_.lr);
  return false;
}

void HostRuntime::SyncNativeCPInterrupt() {
  const bool pending = cp_.InterruptPending();
  pi_.SetInterrupt(GekkoAOT::HW::PI::NativePI::CommandProcessor, pending);

  // v88: bounded transition trace for the real CP FIFO breakpoint path. This
  // is intentionally generic: titles such as HPPOA use GXEnableBreakPt as a
  // completion fence and will deadlock if a synchronous host GPU skips the
  // programmed read-pointer stop.
  static bool previous_pending = false;
  static unsigned transition_logs = 0;
  if (pending != previous_pending && transition_logs < 96u) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_CP_BREAKPOINT_V88=1 pending=%u control=%04x status=%04x read=%08x write=%08x breakpoint=%08x distance=%08x\n",
                 pending ? 1u : 0u, unsigned(cp_.Control()), unsigned(cp_.Status()),
                 cp_.FifoReadPointer(), cp_.FifoWritePointer(), cp_.FifoBreakpoint(),
                 cp_.FifoReadWriteDistance());
    ++transition_logs;
  }
  previous_pending = pending;
}

void HostRuntime::SyncNativePEInterrupts() {
  pi_.SetInterrupt(GekkoAOT::HW::PI::NativePI::PeToken, pe_.TokenInterruptPending());
  pi_.SetInterrupt(GekkoAOT::HW::PI::NativePI::PeFinish, pe_.FinishInterruptPending());
}

void HostRuntime::SyncNativeDSPInterrupt() {
  pi_.SetInterrupt(GekkoAOT::HW::PI::NativePI::Dsp, dsp_.InterruptPending());
}

void HostRuntime::SyncNativeAIInterrupt() {
  pi_.SetInterrupt(GekkoAOT::HW::PI::NativePI::Audio, ai_.InterruptPending());
}

void HostRuntime::SyncNativeDIInterrupt() {
  pi_.SetInterrupt(GekkoAOT::HW::PI::NativePI::Dvd, di_.InterruptPending());
}

void HostRuntime::SyncNativeEXIInterrupt() {
  pi_.SetInterrupt(GekkoAOT::HW::PI::NativePI::Exi, exi_.InterruptPending());
}

void HostRuntime::SyncNativeSIInterrupt() {
  pi_.SetInterrupt(GekkoAOT::HW::PI::NativePI::Serial, si_.InterruptPending());
}

void HostRuntime::SyncNativeVIInterrupt() {
  const bool vi_pending = vi_.InterruptPending();
  pi_.SetInterrupt(GekkoAOT::HW::PI::NativePI::Video, vi_pending);

  // v81: distinguish a missing VI latch from a correctly latched interrupt
  // that the SDK/PI mask is still blocking. Report only the transition into
  // the blocked state; normal per-half-line VI updates stay silent.
  static bool video_mask_block_reported = false;
  const bool video_masked =
      (pi_.InterruptMaskValue() & GekkoAOT::HW::PI::NativePI::Video) != 0u;
  if (vi_pending && !video_masked) {
    if (!video_mask_block_reported) {
      video_mask_block_reported = true;
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_VI_IRQ_TRACE_V81=1 phase=blocked-pi-mask pc=%08x msr=%08x cause=%08x mask=%08x\n",
                   cpu_.pc, cpu_.msr, pi_.InterruptCauseValue(),
                   pi_.InterruptMaskValue());
    }
  } else {
    video_mask_block_reported = false;
  }
}

void HostRuntime::ServiceIdleCpBreakpointRecovery(bool frame_boundary) {
  if (!frame_boundary || !cp_idle_breakpoint_recovery_enabled_) return;

  // This path intentionally keys off the retail low-memory scheduler state,
  // not a game address. __OSCurrentThread == nullptr with a non-empty active
  // thread list means the SDK SelectThread idle loop owns the CPU.
  std::uint32_t current_thread = 0u;
  std::uint32_t active_head = 0u;
  if (!memory_.Read32(0x800000e4u, &current_thread) ||
      !memory_.Read32(0x800000dcu, &active_head) ||
      current_thread != 0u || active_head == 0u ||
      !cp_.BreakpointActiveForHost() || cp_.BreakpointInterruptEnabled() ||
      cp_.FifoReadWriteDistance() < 32u) {
    cp_idle_breakpoint_frames_ = 0u;
    cp_idle_breakpoint_address_ = 0u;
    return;
  }

  const std::uint32_t breakpoint = cp_.FifoBreakpoint();
  if (cp_idle_breakpoint_address_ != breakpoint) {
    cp_idle_breakpoint_address_ = breakpoint;
    cp_idle_breakpoint_frames_ = 1u;
    return;
  }

  if (cp_idle_breakpoint_frames_ < 0xffffffffu)
    ++cp_idle_breakpoint_frames_;

  // A legitimate framebuffer hold normally resolves on the next retrace.
  // Three complete VI frames is therefore intentionally generous and keeps the
  // recovery out of normal GX breakpoint traffic.
  if (cp_idle_breakpoint_frames_ < 3u) return;

  const std::uint32_t read_before = cp_.FifoReadPointer();
  const std::uint32_t distance_before = cp_.FifoReadWriteDistance();
  const std::uint16_t token_before = pe_.Token();

  if (!cp_.StepPastHandledBreakpointOnce()) return;

  SyncNativeGXInterrupts();
  SyncNativeCPInterrupt();
  ++cp_idle_breakpoint_recoveries_;

  std::fprintf(stderr,
               "GEKKOAOT_CP_IDLE_BREAKPOINT_RECOVERY_V160=1 phase=step count=%llu "
               "bp=%08x read=%08x->%08x distance=%08x->%08x pe_token=%04x->%04x "
               "current=0 active=%08x\n",
               static_cast<unsigned long long>(cp_idle_breakpoint_recoveries_),
               breakpoint, read_before, cp_.FifoReadPointer(), distance_before,
               cp_.FifoReadWriteDistance(), unsigned(token_before), unsigned(pe_.Token()),
               active_head);

  // Do not repeatedly walk the FIFO in one deadlock. Once one burst has crossed
  // the held fence, wait for the guest's VI/PE callbacks to observe the new
  // token/state. If the guest later programs another breakpoint the address
  // change starts a new, independently qualified recovery window.
  cp_idle_breakpoint_frames_ = 0u;
  cp_idle_breakpoint_address_ = 0u;
}

bool HostRuntime::TryTakeExternalInterrupt() {
  // External interrupts are level-sensitive at the PI boundary. The device/PI
  // cause remains asserted until guest software acknowledges it; this method
  // only performs the CPU-side exception entry when interrupts are enabled.
  // Read the cause/mask pair once. InterruptPending() used to reload the same
  // two fields, which is measurable because this probe sits on every dispatch.
  const std::uint32_t pi_cause = pi_.InterruptCauseValue();
  const std::uint32_t pi_mask = pi_.InterruptMaskValue();
  const std::uint32_t pending = pi_cause & pi_mask;
  if (pending == 0u) return false;
  const bool video_ready =
      (pending & GekkoAOT::HW::PI::NativePI::Video) != 0u;
  if ((cpu_.msr & kPpcMsrEe) == 0u) {
    if (video_ready) {
      static bool video_ee_block_reported = false;
      if (!video_ee_block_reported) {
        video_ee_block_reported = true;
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_VI_IRQ_TRACE_V81=1 phase=blocked-ee pc=%08x msr=%08x cause=%08x mask=%08x\n",
                     cpu_.pc, cpu_.msr, pi_cause, pi_mask);
      }
    }
    return false;
  }

  if (video_ready) {
    static unsigned video_take_logs = 0u;
    if (video_take_logs++ < 128u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_VI_IRQ_TRACE_V81=1 phase=vector500 pc=%08x msr=%08x cause=%08x mask=%08x\n",
                   cpu_.pc, cpu_.msr, pi_cause, pi_mask);
  }

  // Mirror the Gekko/750 external-interrupt entry sequence used by Dolphin:
  // SRR0 resumes at the next instruction (the standalone dispatcher PC is
  // already that architectural boundary), SRR1 keeps the architecturally
  // saved MSR fields, LE inherits ILE, and exception mode clears EE/IR/DR/RI
  // plus the other architecturally specified control bits before vectoring.
  const std::uint32_t old_msr = cpu_.msr;
  cpu_.srr0 = cpu_.pc;
  cpu_.srr1 = old_msr & kPpcMsrRfiMask;

  std::uint32_t exception_msr = old_msr;
  exception_msr = (exception_msr & ~kPpcMsrLe) |
                  ((old_msr & kPpcMsrIle) != 0u ? kPpcMsrLe : 0u);
  exception_msr &= ~kPpcExceptionEntryClearMask;
  cpu_.msr = exception_msr;
  cpu_.pc = 0x00000500u;
  return true;
}


bool HostRuntime::TryTakeDecrementerInterrupt() {
  // DEC is an EE-masked asynchronous exception just like PI delivery, but it
  // vectors through 0x900 and its CPU-side pending latch is consumed on entry.
  // The architectural DEC register itself keeps counting independently.
  if (!decrementer_pending_ || (cpu_.msr & kPpcMsrEe) == 0u) return false;

  decrementer_pending_ = false;
  const std::uint32_t old_msr = cpu_.msr;
  cpu_.srr0 = cpu_.pc;
  cpu_.srr1 = old_msr & kPpcMsrRfiMask;

  std::uint32_t exception_msr = old_msr;
  exception_msr = (exception_msr & ~kPpcMsrLe) |
                  ((old_msr & kPpcMsrIle) != 0u ? kPpcMsrLe : 0u);
  exception_msr &= ~kPpcExceptionEntryClearMask;
  cpu_.msr = exception_msr;
  cpu_.pc = 0x00000900u;
  return true;
}

void HostRuntime::AdvancePerformanceCounters(std::uint64_t cpu_cycles,
                                              std::uint64_t tb_ticks) {
  // Gekko manual 11.2: event 1 counts core cycles, event 3 counts selected
  // rising TBL edges. Use the existing hardware clock exactly once, alongside
  // TB/DEC; reading an SPR is not itself a source of elapsed time.
  const auto mmcr0 = spr_state_[952u];
  const auto mmcr1 = spr_state_[956u];
  const bool user = (cpu_.msr & 0x4000u) != 0u;
  const bool marked = (cpu_.msr & 4u) != 0u;
  if ((mmcr0 & 0x80000000u) ||
      (mmcr0 & (user ? 0x20000000u : 0x40000000u)) ||
      (mmcr0 & (marked ? 0x10000000u : 0x08000000u))) return;

  const unsigned selectors[] = {(mmcr0 >> 6) & 127u, mmcr0 & 63u,
                                (mmcr1 >> 27) & 31u, (mmcr1 >> 22) & 31u};
  constexpr unsigned registers[] = {953u, 954u, 957u, 958u};
  // RTCSELECT uses IBM bit numbering: TBL bits 31/23/19/15.
  constexpr unsigned edge_bits[] = {0u, 8u, 12u, 16u};
  const unsigned bit = edge_bits[(mmcr0 >> 23) & 3u];
  const std::uint64_t period = 1ull << (bit + 1u);
  const std::uint64_t phase = cpu_.timebase & (period - 1u);
  const std::uint64_t edge = 1ull << bit;
  const std::uint64_t distance = phase < edge ? edge - phase : period + edge - phase;
  const std::uint64_t rises = tb_ticks < distance ? 0u : 1u + (tb_ticks - distance) / period;
  const bool secondary_enabled = (mmcr0 & 0x2000u) == 0u ||
                                 (spr_state_[953u] & 0x80000000u) != 0u;
  for (unsigned i = 0; i != 4u; ++i) {
    if (i != 0u && !secondary_enabled) continue;
    // Instruction/cache/stall events need their own instrumentation. Do not
    // misrepresent them as cycles. Performance-monitor IRQs are not yet wired.
    const auto increment = selectors[i] == 1u ? cpu_cycles :
                           selectors[i] == 3u ? rises : 0u;
    spr_state_[registers[i]] += static_cast<std::uint32_t>(increment);
  }
}

void HostRuntime::AdvanceCpuTimers(std::uint64_t cpu_cycles) {
  if (cpu_cycles == 0u) return;

  // On retail GameCube the Time Base and Decrementer tick at one twelfth of
  // the 486 MHz Gekko core clock (40.5 MHz). Keep independent phase remainders
  // because writes to TB/DEC establish a new software-visible timer origin.
  // Generated slices are normally tiny (hundreds/thousands of cycles). Keep
  // the common path in 32-bit arithmetic; 64-bit div/mod was a measurable
  // hotspot on x86-64 and atom-class cores.
  std::uint64_t tb_ticks = 0;
  std::uint64_t dec_ticks = 0;
  if (cpu_cycles <= 0xffffffffull - kTimerRatio) {
    const std::uint32_t c = static_cast<std::uint32_t>(cpu_cycles);
    const std::uint32_t tb_total = timebase_cycle_remainder_ + c;
    tb_ticks = tb_total / static_cast<std::uint32_t>(kTimerRatio);
    timebase_cycle_remainder_ = tb_total - static_cast<std::uint32_t>(tb_ticks) * static_cast<std::uint32_t>(kTimerRatio);
    const std::uint32_t dec_total = decrementer_cycle_remainder_ + c;
    dec_ticks = dec_total / static_cast<std::uint32_t>(kTimerRatio);
    decrementer_cycle_remainder_ = dec_total - static_cast<std::uint32_t>(dec_ticks) * static_cast<std::uint32_t>(kTimerRatio);
  } else {
    const std::uint64_t tb_total = static_cast<std::uint64_t>(timebase_cycle_remainder_) + cpu_cycles;
    tb_ticks = tb_total / kTimerRatio;
    timebase_cycle_remainder_ = static_cast<std::uint32_t>(tb_total % kTimerRatio);
    const std::uint64_t dec_total = static_cast<std::uint64_t>(decrementer_cycle_remainder_) + cpu_cycles;
    dec_ticks = dec_total / kTimerRatio;
    decrementer_cycle_remainder_ = static_cast<std::uint32_t>(dec_total % kTimerRatio);
  }
  AdvancePerformanceCounters(cpu_cycles, tb_ticks);
  cpu_.timebase += tb_ticks;
  if (dec_ticks == 0u) return;

  const std::uint32_t old_dec = spr_state_[kSprDec];
  // DEC raises its exception when decrementing 0 -> 0xffffffff. The distance
  // to that transition is old_dec+1 in the 32-bit modulo counter; 0xffffffff
  // therefore correctly means one complete 2^32-tick revolution.
  const std::uint64_t ticks_until_exception = static_cast<std::uint64_t>(old_dec) + 1ull;
  if (dec_ticks >= ticks_until_exception) decrementer_pending_ = true;
  spr_state_[kSprDec] = old_dec - static_cast<std::uint32_t>(dec_ticks);
}

void HostRuntime::SyncNativeGXInterrupts() {
  if (!native_gx_.PeEventPending()) return;
  const bool token_seen = native_gx_.ConsumeTokenSeen();
  const bool token_interrupt = native_gx_.ConsumeTokenInterruptSeen();
  if (token_seen || token_interrupt)
    pe_.SetToken(native_gx_.LastToken(), token_interrupt);
  if (native_gx_.ConsumeFinishSeen())
    pe_.SetFinish();
  SyncNativePEInterrupts();
}

std::uint32_t HostRuntime::SprRead(CPUState* cpu, std::uint16_t spr, std::uint32_t address) {
  auto* self = Self(cpu);
  if (!self) return 0;
  // User monitor registers are read-only aliases, not independent storage.
  if (spr >= 936u && spr <= 942u) spr += 16u;

  // Mirror DolRecomp's architectural SPR fast-paths as well as the standalone
  // host-stored implementation-dependent registers.  Generated code normally
  // handles these fields directly, but RAM-resident Nintendo exception vectors
  // reach them through the tiny low-memory executor below.
  switch (spr) {
  case 1u: return cpu->xer;
  case 8u: return cpu->lr;
  case 9u: return cpu->ctr;
  case 18u: return cpu->dsisr;
  case 19u: return cpu->dar;
  case kSprDec: {
    const std::uint32_t value = self->spr_state_[kSprDec];
    TraceSprReadV90(kSprDec, address, cpu->lr, value);
    return value;
  }
  case 26u: return cpu->srr0;
  case 27u: return cpu->srr1;
  case 268u: return static_cast<std::uint32_t>(cpu->timebase);
  case 269u: return static_cast<std::uint32_t>(cpu->timebase >> 32);
  case 921u: {
    // BNE waits for outstanding 32-byte bus transfers. A partial staging
    // line can contain GXFlush padding while the bus is already idle; the SDK
    // polls BNE before resetting WPAR and discarding those residual bytes.
    // Our complete bursts are transferred synchronously, so none are pending
    // when control returns to guest code.
    const auto value = self->spr_state_[921u] & ~31u;
    static const bool trace = [] {
      const char* setting = std::getenv("GEKKOAOT_NATIVE_WPAR_TRACE");
      return setting && setting[0] == '1' && setting[1] == '\0';
    }();
    if (trace && (self->cpu_fifo_gather_size_ || self->gpu_fifo_gather_size_)) {
      static std::uint64_t reads = 0;
      ++reads;
      if ((reads & (reads - 1u)) == 0u) {
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_WPAR_TRACE_V105=1 reads=%llu pc=%08x lr=%08x value=%08x cpu_bytes=%u gpu_bytes=%u gpu_first_pc=%08x pi_write=%08x cp_distance=%08x\n",
                     static_cast<unsigned long long>(reads), address, cpu->lr, value,
                     self->cpu_fifo_gather_size_, self->gpu_fifo_gather_size_,
                     self->gpu_fifo_first_pc_, self->pi_.FifoWritePointer(),
                     self->cp_.FifoReadWriteDistance());
      }
    }
    return value;
  }
  case 920u:
    TraceSprReadV90(920u, address, cpu->lr, cpu->hid2);
    return cpu->hid2;
  default: break;
  }
  if (spr >= 912u && spr <= 919u) return cpu->gqr[spr - 912u];
  if (IsHostStoredSpr(spr)) {
    const std::uint32_t value = self->spr_state_[spr];
    TraceSprReadV90(spr, address, cpu->lr, value);
    return value;
  }
  self->SetFault(FaultKind::SprRead, address, spr);
  return 0;
}

void HostRuntime::SprWrite(CPUState* cpu, std::uint16_t spr, std::uint32_t value,
                           std::uint32_t address) {
  auto* self = Self(cpu);
  if (!self) return;
  if (spr >= 936u && spr <= 942u) {
    self->SetFault(FaultKind::SprWrite, address, spr);
    return;
  }
  if (spr == 952u || spr == 956u) {
    static unsigned logs = 0u;
    if (logs < 32u) {
      ++logs;
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_PMC_V91=1 pc=%08x spr=%u value=%08x events=cycles+tb-edges irq=unimplemented\n",
                   address, unsigned(spr), value);
    }
  }
  switch (spr) {
  case 1u: cpu->xer = value; return;
  case 8u: cpu->lr = value; return;
  case 9u: cpu->ctr = value; return;
  case 18u: cpu->dsisr = value; return;
  case 19u: cpu->dar = value; return;
  case 25u:
    self->spr_state_[spr] = value;
    // A cached module compiled before memory fault restart entries cannot
    // safely RFI to an interior load/store. Preserve its legacy VM aperture
    // until it is rebuilt, independently of game identity.
    {
      bool restartable = !self->module_.IsOpen() || self->module_.SupportsMemoryRestart();
      for (const auto& side : self->secondary_modules_)
        restartable = restartable && side->SupportsMemoryRestart();
      const bool enable_pages = value != 0u && restartable;
      if (enable_pages != self->memory_.PagedVmem()) {
        std::fprintf(stderr,
                     "GEKKOAOT_VM_MIGRATION_V97=1 sdr1=%08x paging=%u module_restart=%u side_modules=%zu legacy_fallback=%u\n",
                     value, enable_pages ? 1u : 0u, restartable ? 1u : 0u,
                     self->secondary_modules_.size(), (value && !restartable) ? 1u : 0u);
      } else if (value != 0u && !restartable) {
        static unsigned fallback_logs = 0;
        if (fallback_logs++ < 2u)
          std::fprintf(stderr,
                       "GEKKOAOT_VM_MIGRATION_V97=1 sdr1=%08x paging=0 module_restart=0 legacy_fallback=1\n",
                       value);
      }
      self->memory_.SetPagedVmem(enable_pages);
    }
    return;
  case kSprDec: {
    const std::uint32_t old_value = self->spr_state_[kSprDec];
    self->spr_state_[kSprDec] = value;
    self->decrementer_cycle_remainder_ = 0u;
    // A software write that flips DEC from non-negative to negative triggers
    // the same decrementer exception as a hardware 0 -> -1 transition.
    if ((old_value & 0x80000000u) == 0u && (value & 0x80000000u) != 0u)
      self->decrementer_pending_ = true;
    return;
  }
  case 26u: cpu->srr0 = value; return;
  case 27u: cpu->srr1 = value; return;
  case 268u:
  case 284u:
    cpu->timebase = (cpu->timebase & 0xffffffff00000000ull) | value;
    if (spr == 284u) self->timebase_cycle_remainder_ = 0u;
    return;
  case 269u:
  case 285u:
    cpu->timebase = (std::uint64_t(value) << 32) | (cpu->timebase & 0xffffffffull);
    if (spr == 285u) self->timebase_cycle_remainder_ = 0u;
    return;
  case 921u:
    if (const char* setting = std::getenv("GEKKOAOT_NATIVE_WPAR_TRACE");
        setting && setting[0] == '1' && setting[1] == '\0') {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_WPAR_WRITE_V105=1 pc=%08x lr=%08x value=%08x cpu_bytes=%u gpu_bytes=%u cpu_fifo=%08x-%08x gpu_fifo=%08x-%08x\n",
                   address, cpu->lr, value, self->cpu_fifo_gather_size_,
                   self->gpu_fifo_gather_size_, self->pi_.FifoBase(),
                   self->pi_.FifoEnd(), self->cp_.FifoBase(), self->cp_.FifoEnd());
    }
    self->spr_state_[921u] = value & ~31u;
    self->cpu_fifo_gather_size_ = 0u;
    self->gpu_fifo_gather_size_ = 0u;
    return;
  case 920u: cpu->hid2 = value; return;
  default: break;
  }
  if (spr >= 912u && spr <= 919u) {
    cpu->gqr[spr - 912u] = value;
    return;
  }
  if (spr == kSprHid0) {
    const std::uint32_t old_value = self->spr_state_[kSprHid0];
    const bool invalidate_icache = (value & kHid0IcfiMask) != 0u;

    // ICFI is command-like state, not a sticky configuration bit. Accept the
    // invalidate request and clear it before guest code can poll HID0 again.
    // Reuse the v87 icbi coherency path so fixed-image AOT identity caches are
    // invalidated as well.
    self->spr_state_[kSprHid0] = value & ~kHid0IcfiMask;
    if (invalidate_icache) {
      self->InvalidateLowInstructionCache();
      CacheControl(cpu, kCacheIcbi, 0u, address);
    }

    static unsigned hid0_logs = 0u;
    if (hid0_logs++ < 64u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_HID0_V90=1 op=write pc=%08x lr=%08x old=%08x requested=%08x latched=%08x icfi=%u action=%s\n",
                   address, cpu->lr, old_value, value,
                   self->spr_state_[kSprHid0], invalidate_icache ? 1u : 0u,
                   invalidate_icache ? "self-clear+invalidate-aot-identity" : "latch-state");
    return;
  }
  if (IsHostWritableSpr(spr)) {
    self->spr_state_[spr] = value;

    // DMAU/DMAL drive the Gekko locked-cache DMA engine.  The transfer starts
    // when DMAL.DMA_T is written.  Model completion synchronously, matching the
    // standalone runtime's other boot-time DMA engines: the length field is in
    // 32-byte blocks and an encoded length of zero means 128 blocks.
    if (spr == kSprDmal && (value & kDmalTransfer) != 0u) {
      const std::uint32_t dmau = self->spr_state_[kSprDmau];
      const std::uint32_t memory_address = dmau & ~31u;
      const std::uint32_t cache_address = value & ~31u;
      std::uint32_t blocks = ((dmau & 0x1fu) << 2) | ((value >> 2) & 0x3u);
      if (blocks == 0u) blocks = 128u;

      const bool memory_to_lc = (value & kDmalLoad) != 0u;
      const bool ok = memory_to_lc
                          ? self->lc_.MemoryToLockedCache(self->memory_, cache_address,
                                                          memory_address, blocks)
                          : self->lc_.LockedCacheToMemory(self->memory_, memory_address,
                                                          cache_address, blocks);
      // DMA_T reads back clear once the synchronous transfer has completed.
      self->spr_state_[kSprDmal] &= ~kDmalTransfer;
      if (!ok)
        self->SetFault(FaultKind::SprWrite, address,
                       (std::uint32_t(spr) << 16) | (value & 0xffffu));
    }
    return;
  }
  self->SetFault(FaultKind::SprWrite, address, (std::uint32_t(spr) << 16) | (value & 0xffffu));
}

void HostRuntime::CacheControl(CPUState* cpu, std::uint8_t operation,
                               std::uint32_t address,
                               std::uint32_t instruction_address) {
  auto* self = Self(cpu);
  if (!self) return;

  // Host RAM itself is coherent, but NativeGX/Aurora keeps GPU-side snapshots
  // of indexed arrays. dcbst/dcbf/dcbi are the guest's publication boundary
  // for CPU-authored dynamic geometry, so forward them to the renderer.
  if (operation == kCacheDcbst || operation == kCacheDcbf || operation == kCacheDcbi)
    self->native_gx_.NotifyCacheControl(operation, address);

  // icbi is different for an AOT runtime: the guest is declaring that code at
  // this address may have changed. Drop fixed-executable identity caches so a
  // loader/overlay is re-identified from the authoritative guest bytes before
  // the next overlapping dispatch. Native code itself remains immutable; SMC
  // outside precompiled images therefore still fails closed elsewhere.
  if (operation == kCacheIcbi) {
    self->InvalidateLowInstructionCacheLine(address);
    self->fixed_exec_route_cache_ = {};
    self->fixed_exec_active_descriptor_ = nullptr;
    ++self->icbi_route_invalidations_;
    static unsigned logs = 0;
    if (logs++ < 64u)
      std::fprintf(stderr,
                   "GEKKOAOT_ICBI_AOT_IDENTITY_V87=1 ea=%08x pc=%08x invalidations=%llu action=drop-fixed-image-route\n",
                   address, instruction_address,
                   static_cast<unsigned long long>(self->icbi_route_invalidations_));
  }
}

void HostRuntime::InvalidateLowInstructionCache() {
  low_instruction_cache_valid_.fill(false);
}

void HostRuntime::InvalidateLowInstructionCacheLine(std::uint32_t address) {
  const std::uint32_t physical = AddressSpace::ToPhysical(address);
  if (physical >= kLowInstructionCacheSpan) return;
  const std::size_t line = physical / kLowInstructionCacheLineBytes;
  if (line < low_instruction_cache_valid_.size())
    low_instruction_cache_valid_[line] = false;
}

bool HostRuntime::FetchLowMemoryInstruction(std::uint32_t address,
                                            std::uint32_t* instruction) {
  if (!instruction) return false;
  const std::uint32_t physical = AddressSpace::ToPhysical(address);
  if (physical >= kLowInstructionCacheSpan || (physical & 3u) != 0u)
    return memory_.Read32(address, instruction);

  // When HID0.ICE is clear the core observes RAM directly. With ICE enabled,
  // preserve a 32-byte line until icbi/ICFI, so DCStoreRange alone does not
  // publish rewritten exception code to instruction fetch. This is required by
  // IPL/Datel-style boot flows and mirrors the architectural distinction
  // between D-cache publication and I-cache invalidation.
  if ((spr_state_[kSprHid0] & kHid0IceMask) == 0u)
    return memory_.Read32(address, instruction);

  const std::uint32_t line_base = physical & ~(kLowInstructionCacheLineBytes - 1u);
  const std::size_t line = line_base / kLowInstructionCacheLineBytes;
  if (!low_instruction_cache_valid_[line]) {
    const std::size_t first_word = line_base / sizeof(std::uint32_t);
    for (std::uint32_t offset = 0; offset < kLowInstructionCacheLineBytes; offset += 4u) {
      std::uint32_t value = 0;
      if (!memory_.Read32(line_base + offset, &value)) return false;
      low_instruction_cache_[first_word + offset / 4u] = value;
    }
    low_instruction_cache_valid_[line] = true;
  }
  *instruction = low_instruction_cache_[physical / sizeof(std::uint32_t)];
  return true;
}

bool HostRuntime::ExecuteLowMemoryInstruction(std::uint32_t address,
                                              std::uint64_t* charged_cycles) {
  if (!charged_cycles) return false;
  const std::uint32_t physical = AddressSpace::ToPhysical(address);
  if (!IsLowExceptionVectorAddress(physical) || (physical & 3u) != 0u) return false;

  std::uint32_t instruction = 0;
  if (!FetchLowMemoryInstruction(address, &instruction)) return false;

  // Gekko manual 9.4.2: sync/eieio do not flush the write-gather pipe.
  // Only completing a 32-byte line publishes it. WPAR writes discard tails.

  // Crossing from generated code into a real low-memory exception vector is
  // the standalone equivalent of the normal execution chassis accepting the
  // pending synchronous exception. Consume only the matching synchronous bit;
  // unrelated/future asynchronous causes remain pending.
  AcknowledgeSynchronousExceptionVector(&cpu_, physical);

  // RAM-resident OS vectors access the low-memory context pointers and, on
  // some SDK revisions, hardware apertures. Reuse the standalone address space
  // first and fall through to the normal external bus only when the address is
  // genuinely outside MEM1/MEM2.
  const auto read16 = [this](std::uint32_t ea, std::uint16_t* out) {
    if (memory_.Read16(ea, out)) return true;
    *out = static_cast<std::uint16_t>(ExternalRead(&cpu_, ea, 2u));
    return fault_.kind == FaultKind::None;
  };
  const auto read32 = [this](std::uint32_t ea, std::uint32_t* out) {
    if (memory_.Read32(ea, out)) return true;
    *out = static_cast<std::uint32_t>(ExternalRead(&cpu_, ea, 4u));
    return fault_.kind == FaultKind::None;
  };
  const auto write16 = [this](std::uint32_t ea, std::uint16_t value) {
    if (memory_.Write16(ea, value)) return true;
    ExternalWrite(&cpu_, ea, value, 2u);
    return fault_.kind == FaultKind::None;
  };
  const auto write32 = [this](std::uint32_t ea, std::uint32_t value) {
    if (memory_.Write32(ea, value)) return true;
    ExternalWrite(&cpu_, ea, value, 4u);
    return fault_.kind == FaultKind::None;
  };

  // rfi semantics intentionally mirror pinned DolRecomp 40637c4: restore only
  // the architecturally writable RFI MSR fields, clear POW and resume at the
  // aligned SRR0. Exception vectors execute in supervisor state; if a corrupt
  // vector somehow reaches rfi with MSR[PR] set, keep fail-closed diagnostics
  // rather than silently inventing a nested Program exception.
  if (instruction == kPpcRfi) {
    if ((cpu_.msr & kPpcMsrPr) != 0u) {
      SetFault(FaultKind::InstructionFallback, address, instruction);
      return true;
    }
    cpu_.msr = (cpu_.msr & ~kPpcMsrRfiMask) | (cpu_.srr1 & kPpcMsrRfiMask);
    cpu_.msr &= ~kPpcMsrPow;
    cpu_.pc = cpu_.srr0 & ~3u;
    *charged_cycles = 2u;
    return true;
  }

  // Nintendo's first-level OS exception vectors are copied into low MEM1 at
  // runtime, so their instructions cannot be part of the DOL AOT ranges. The
  // System Call vector used by SMB2 starts by toggling HID0[ICFI] around an
  // isync/sync pair. Reuse the exact standalone SPR bridge rather than adding a
  // second shadow register model just for RAM-resident exception code.
  bool spr_write = false;
  std::uint16_t spr = 0;
  std::uint8_t gpr = 0;
  if (DecodeSprTransfer(instruction, &spr_write, &spr, &gpr)) {
    if (spr_write)
      SprWrite(&cpu_, spr, cpu_.gpr[gpr], address);
    else
      cpu_.gpr[gpr] = SprRead(&cpu_, spr, address);
    if (fault_.kind == FaultKind::None) cpu_.pc = address + 4u;
    *charged_cycles = 1u;
    return true;
  }

  const std::uint32_t primary = instruction >> 26;

  // D-form integer loads/stores used by Nintendo's first-level exception
  // vector template. The rA==0 rule is the normal PowerPC zero-base form.
  if (primary == 32u || primary == 36u || primary == 40u || primary == 44u) {
    const std::uint32_t rt_rs = (instruction >> 21) & 31u;
    const std::uint32_t ra = (instruction >> 16) & 31u;
    const auto displacement = static_cast<std::int32_t>(
        static_cast<std::int16_t>(instruction & 0xffffu));
    const std::uint32_t base = ra == 0u ? 0u : cpu_.gpr[ra];
    const std::uint32_t ea = base + static_cast<std::uint32_t>(displacement);
    bool ok = false;
    if (primary == 32u) { // lwz
      std::uint32_t value = 0;
      ok = read32(ea, &value);
      if (ok) cpu_.gpr[rt_rs] = value;
    } else if (primary == 36u) { // stw
      ok = write32(ea, cpu_.gpr[rt_rs]);
    } else if (primary == 40u) { // lhz
      std::uint16_t value = 0;
      ok = read16(ea, &value);
      if (ok) cpu_.gpr[rt_rs] = value;
    } else { // sth
      ok = write16(ea, static_cast<std::uint16_t>(cpu_.gpr[rt_rs]));
    }
    if (ok) cpu_.pc = address + 4u;
    *charged_cycles = 1u;
    return true;
  }

  if (primary == 14u || primary == 15u) { // addi / addis (li/lis/subi aliases)
    const std::uint32_t rd = (instruction >> 21) & 31u;
    const std::uint32_t ra = (instruction >> 16) & 31u;
    const auto immediate = static_cast<std::int32_t>(
        static_cast<std::int16_t>(instruction & 0xffffu));
    const std::uint32_t base = ra == 0u ? 0u : cpu_.gpr[ra];
    const std::uint32_t addend = primary == 15u
                                      ? (static_cast<std::uint32_t>(immediate) << 16)
                                      : static_cast<std::uint32_t>(immediate);
    cpu_.gpr[rd] = base + addend;
    cpu_.pc = address + 4u;
    *charged_cycles = 1u;
    return true;
  }

  if (primary == 21u) { // rlwinm[.]
    const std::uint32_t rs = (instruction >> 21) & 31u;
    const std::uint32_t ra = (instruction >> 16) & 31u;
    const unsigned sh = (instruction >> 11) & 31u;
    const unsigned mb = (instruction >> 6) & 31u;
    const unsigned me = (instruction >> 1) & 31u;
    const std::uint32_t value = RotateLeft32(cpu_.gpr[rs], sh) & RotationMask(mb, me);
    cpu_.gpr[ra] = value;
    if ((instruction & 1u) != 0u) SetCr0FromResult(&cpu_, value);
    cpu_.pc = address + 4u;
    *charged_cycles = 1u;
    return true;
  }

  if (primary == 16u) { // bc / bcl and aliases such as bne
    const std::uint32_t bo = (instruction >> 21) & 31u;
    const std::uint32_t bi = (instruction >> 16) & 31u;
    bool ctr_ok = true;
    if ((bo & 0x4u) == 0u) {
      cpu_.ctr -= 1u;
      ctr_ok = (cpu_.ctr != 0u) != ((bo & 0x2u) != 0u);
    }
    const bool cond_ok = (bo & 0x10u) != 0u ||
                         CrBit(&cpu_, bi) == ((bo & 0x8u) != 0u);
    if ((instruction & 1u) != 0u) cpu_.lr = address + 4u;
    if (ctr_ok && cond_ok) {
      const auto displacement = static_cast<std::int32_t>(
          static_cast<std::int16_t>(instruction & 0xfffcu));
      const std::uint32_t base = (instruction & 2u) != 0u ? 0u : address;
      cpu_.pc = base + static_cast<std::uint32_t>(displacement);
    } else {
      cpu_.pc = address + 4u;
    }
    *charged_cycles = 1u;
    return true;
  }

  if (primary == kPpcPrimaryX) {
    const std::uint32_t xo = (instruction >> 1) & 0x3ffu;
    const std::uint32_t rd_rs = (instruction >> 21) & 31u;
    if (xo == 19u) { // mfcr
      cpu_.gpr[rd_rs] = cpu_.cr;
      cpu_.pc = address + 4u;
      *charged_cycles = 1u;
      return true;
    }
    if (xo == 83u) { // mfmsr
      if ((cpu_.msr & kPpcMsrPr) != 0u) {
        SetFault(FaultKind::InstructionFallback, address, instruction);
      } else {
        cpu_.gpr[rd_rs] = cpu_.msr;
        cpu_.pc = address + 4u;
      }
      *charged_cycles = 1u;
      return true;
    }
    if (xo == 444u) { // or (mr is or rA,rS,rS)
      const std::uint32_t ra = (instruction >> 16) & 31u;
      const std::uint32_t rb = (instruction >> 11) & 31u;
      const std::uint32_t value = cpu_.gpr[rd_rs] | cpu_.gpr[rb];
      cpu_.gpr[ra] = value;
      if ((instruction & 1u) != 0u) SetCr0FromResult(&cpu_, value);
      cpu_.pc = address + 4u;
      *charged_cycles = 1u;
      return true;
    }
  }

  if (primary == 24u) { // ori rA,rS,UIMM
    const std::uint32_t rs = (instruction >> 21) & 31u;
    const std::uint32_t ra = (instruction >> 16) & 31u;
    cpu_.gpr[ra] = cpu_.gpr[rs] | (instruction & 0xffffu);
    cpu_.pc = address + 4u;
    *charged_cycles = 1u;
    return true;
  }

  // The guest's sync/isync ordering is stronger than this single-threaded
  // low-vector executor strictly needs. Keep a host fence anyway so future
  // device/worker integration cannot reorder state across the architectural
  // barriers. Low-memory instruction visibility is handled by the v143
  // 32-byte I-cache model above; sync/isync do not themselves invalidate it.
  if (instruction == 0x7c0004acu || instruction == 0x4c00012cu) { // sync / isync
    std::atomic_thread_fence(std::memory_order_seq_cst);
    cpu_.pc = address + 4u;
    *charged_cycles = 1u;
    return true;
  }

  // Keep simple branch stubs executable as well. This is useful for low-memory
  // vectors that trampoline into statically compiled SDK exception handlers.
  if ((instruction >> 26) == 18u) { // b / ba / bl / bla
    std::int32_t displacement = static_cast<std::int32_t>(instruction & 0x03fffffcu);
    if ((displacement & 0x02000000) != 0)
      displacement |= static_cast<std::int32_t>(0xfc000000u);
    const bool absolute = (instruction & 2u) != 0u;
    const bool link = (instruction & 1u) != 0u;
    if (link) cpu_.lr = address + 4u;
    const std::uint32_t base = absolute ? 0u : address;
    cpu_.pc = base + static_cast<std::uint32_t>(displacement);
    *charged_cycles = 1u;
    return true;
  }

  if (instruction == kPpcNop) {
    cpu_.pc = address + 4u;
    *charged_cycles = 1u;
    return true;
  }

  // Surface the exact RAM-resident vector opcode. As OSExceptionInit copies
  // more of its 0x98-byte vector template into low memory, this gives us a
  // deterministic next instruction to implement instead of reporting a vague
  // "uncompiled address".
  SetFault(FaultKind::InstructionFallback, address, instruction);
  return true;
}

std::uint64_t HostRuntime::TakeRealtimeHardwareCycles(std::uint64_t charged,
                                                       bool force_sample) {
  using Clock = std::chrono::steady_clock;
  constexpr std::uint64_t kClockHz = GekkoAOT::HW::VI::NativeVI::CpuClockHz;
  constexpr std::uint64_t kNsPerSecond = 1000000000ull;
  // Keep scheduler/device service bounded while coalescing tiny AOT exits. The
  // expensive clock source itself is replaced by invariant TSC when the host
  // publishes an exact CPUID.15H ratio; non-x86 and uncertain VMs retain the
  // portable steady_clock path.
  constexpr std::uint64_t kGuestPollQuantum =
      (GekkoAOT::HW::VI::NativeVI::CpuClockHz / 8000u) + 1u;
  constexpr std::uint64_t kIdlePollQuantum =
      (GekkoAOT::HW::VI::NativeVI::CpuClockHz / 16000u) + 1u;
  constexpr auto kPauseThreshold = std::chrono::milliseconds(250);
  const std::uint64_t poll_quantum = force_sample ? kIdlePollQuantum : kGuestPollQuantum;

  hardware_clock_sampled_last_call_ = false;
  if (charged > std::numeric_limits<std::uint64_t>::max() -
                    hardware_clock_guest_since_sample_)
    hardware_clock_guest_since_sample_ = poll_quantum;
  else
    hardware_clock_guest_since_sample_ += charged;

  if (hardware_clock_started_ && hardware_clock_guest_since_sample_ < poll_quantum)
    return 0u;
  hardware_clock_guest_since_sample_ = 0u;

  if (!hardware_tsc_checked_) {
    hardware_tsc_checked_ = true;
    const auto info = DetectFastInvariantTsc();
    hardware_tsc_enabled_ = RuntimeEnvBool("GEKKOAOT_RUNTIME_TSC", true) && info.available;
    hardware_tsc_hz_ = hardware_tsc_enabled_ ? info.hz : 0u;
    std::fprintf(stderr,
                 "GEKKOAOT_RUNTIME_CLOCK_V136=1 source=%s hz=%llu fallback=steady-clock env=GEKKOAOT_RUNTIME_TSC\n",
                 hardware_tsc_enabled_ ? "invariant-tsc-cpuid15" : "steady-clock",
                 static_cast<unsigned long long>(hardware_tsc_hz_));
  }

  hardware_clock_sampled_last_call_ = true;
  if (!hardware_clock_started_) {
    hardware_clock_started_ = true;
    hardware_clock_last_ = Clock::now();
    if (hardware_tsc_enabled_) hardware_tsc_last_ = ReadFastTsc();
    return 0u;
  }

  if (hardware_tsc_enabled_) {
    const std::uint64_t now_ticks = ReadFastTsc();
    const std::uint64_t delta_ticks = now_ticks - hardware_tsc_last_;
    hardware_tsc_last_ = now_ticks;
    if (delta_ticks == 0u || hardware_tsc_hz_ == 0u) return 0u;

    const std::uint64_t whole_seconds = delta_ticks / hardware_tsc_hz_;
    const std::uint64_t tick_frac = delta_ticks % hardware_tsc_hz_;

    // Keep hardware_clock_last_ in the same steady_clock domain used by the
    // presentation scheduler, without another vDSO call. Carry the division
    // remainder so long runs do not accumulate truncation drift.
    const std::uint64_t ns_scaled =
        tick_frac * kNsPerSecond + hardware_tsc_ns_remainder_;
    const std::uint64_t elapsed_ns =
        whole_seconds * kNsPerSecond + ns_scaled / hardware_tsc_hz_;
    hardware_tsc_ns_remainder_ = ns_scaled % hardware_tsc_hz_;
    hardware_clock_last_ += std::chrono::nanoseconds(elapsed_ns);

    // Match the old pause policy: a debugger stop/window drag must not fast-
    // forward VI/audio/DEC when execution resumes. The synthetic host time is
    // still advanced above so host presentation never queues a catch-up burst.
    if (whole_seconds != 0u || delta_ticks > hardware_tsc_hz_ / 4u)
      return 0u;

    const std::uint64_t cycle_scaled =
        tick_frac * kClockHz + hardware_tsc_cycle_remainder_;
    hardware_tsc_cycle_remainder_ = cycle_scaled % hardware_tsc_hz_;
    return whole_seconds * kClockHz + cycle_scaled / hardware_tsc_hz_;
  }

  const auto now = Clock::now();
  auto elapsed = now - hardware_clock_last_;
  hardware_clock_last_ = now;
  if (elapsed < Clock::duration::zero()) return 0u;
  if (elapsed > kPauseThreshold) return 0u;

  const auto ns_signed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  if (ns_signed <= 0) return 0u;
  const auto ns = static_cast<std::uint64_t>(ns_signed);
  const std::uint64_t whole = ns / kNsPerSecond;
  const std::uint64_t frac = ns % kNsPerSecond;
  const std::uint64_t scaled = frac * kClockHz + hardware_clock_remainder_;
  hardware_clock_remainder_ = scaled % kNsPerSecond;
  return whole * kClockHz + scaled / kNsPerSecond;
}

void HostRuntime::QueueHostPresent(std::uint32_t xfb_top,
                                   std::uint32_t xfb_bottom,
                                   std::uint32_t width,
                                   std::uint32_t stride_bytes,
                                   std::uint32_t field_height) {
  if (!native_gx_.Ready() || xfb_top == 0u) return;
  if (host_present_pending_) ++host_present_dropped_;
  host_present_top_ = xfb_top;
  host_present_bottom_ = xfb_bottom != 0u ? xfb_bottom : xfb_top;
  host_present_width_ = width;
  host_present_stride_bytes_ = stride_bytes;
  host_present_field_height_ = field_height;
  host_present_pending_ = true;
}

void HostRuntime::NoteProducedFrame() {
  ++perf_guest_frames_total_;
  using Clock = std::chrono::steady_clock;
  // AdvanceRuntimeCycles already sampled monotonic time for the hardware domain.
  // Reuse it here; frame production is allowed to see the most recent sample
  // (at most ~125 us old) instead of paying another vDSO call at 50/60 Hz.
  const auto now = hardware_clock_started_ ? hardware_clock_last_ : Clock::now();
  if (produced_frame_seen_) {
    auto delta = std::chrono::duration_cast<std::chrono::nanoseconds>(now - produced_frame_last_);
    constexpr auto kMinPeriod = std::chrono::milliseconds(4);
    constexpr auto kMaxPeriod = std::chrono::milliseconds(100);
    if (delta >= kMinPeriod && delta <= kMaxPeriod) {
      // Mild low-pass filtering avoids target-FPS jitter when one guest frame
      // lands a little early/late, while reacting quickly to 30/50/60 Hz modes.
      produced_frame_period_ = std::chrono::nanoseconds(
          (produced_frame_period_.count() * 3 + delta.count()) / 4);
    }
  } else {
    produced_frame_seen_ = true;
  }
  produced_frame_last_ = now;

  interpolation_pair_valid_ =
      frame_interpolation_enabled_ && native_gx_.FrameInterpolationReady();
  if (interpolation_pair_valid_) {
    const std::uint64_t target = std::max<std::uint32_t>(host_fps_limit_hz_, 1u);
    const auto present_period = std::chrono::nanoseconds(1000000000ull / target);
    // Start one output slot into the transition. This avoids displaying the
    // previous frame again at alpha=0 while keeping the generated frame between
    // two real guest frames.
    interpolation_pair_start_ = now - present_period;
  }
}

void HostRuntime::ServiceHostFrameScheduler(std::chrono::steady_clock::time_point now) {
  if (!native_gx_.Ready()) return;
  if (!host_present_pending_ && !interpolation_pair_valid_) return;

  std::chrono::nanoseconds present_period{0};
  if (host_fps_limit_hz_ != 0u) {
    present_period = std::chrono::nanoseconds(
        static_cast<std::int64_t>(1000000000ull / host_fps_limit_hz_));
    if (host_present_started_ && now < host_present_next_) return;
    host_present_started_ = true;
    // Never queue a burst after a host stall. The newest temporal state wins.
    host_present_next_ = now + present_period;
  }

  bool presented = false;
  if (interpolation_pair_valid_ && host_fps_limit_hz_ != 0u) {
    // Generate extra host frames only when the requested output cadence is
    // actually above the measured guest-frame cadence. At/below native rate,
    // direct XFB presentation avoids needless latency and blending.
    const auto guest_period = std::max(produced_frame_period_, std::chrono::nanoseconds(1));
    const bool target_above_guest = present_period * 100 < guest_period * 95;
    if (target_above_guest) {
      const auto elapsed = now - interpolation_pair_start_;
      const double alpha = std::clamp(
          static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) /
              static_cast<double>(guest_period.count()),
          0.0, 1.0);
      presented = native_gx_.PresentInterpolated(static_cast<float>(alpha));
      if (presented) {
        ++interpolated_present_count_;
        ++host_present_count_;
        host_present_pending_ = false;
      }
    }
  }

  if (!presented && host_present_pending_) {
    native_gx_.PresentNow(host_present_top_, host_present_bottom_,
                          host_present_width_, host_present_stride_bytes_,
                          host_present_field_height_);
    host_present_pending_ = false;
    ++direct_present_count_;
    ++host_present_count_;
  }
}

void HostRuntime::ServicePerformanceMetrics(std::uint64_t hardware_cycles) {
  if (!perf_metrics_enabled_) return;

  // Metrics are human-facing telemetry. Polling steady_clock on every AOT
  // boundary was pure overhead (~one third of CPU in several retail games).
  // Gate the wall-clock read using already-accounted GameCube hardware time.
  constexpr std::uint64_t kMetricsPollCycles =
      GekkoAOT::HW::VI::NativeVI::CpuClockHz / 8u; // 125 ms
  if (hardware_cycles > std::numeric_limits<std::uint64_t>::max() -
                            perf_metrics_poll_cycles_)
    perf_metrics_poll_cycles_ = kMetricsPollCycles;
  else
    perf_metrics_poll_cycles_ += hardware_cycles;
  if (perf_metrics_started_ && perf_metrics_poll_cycles_ < kMetricsPollCycles)
    return;
  perf_metrics_poll_cycles_ = 0u;

  using Clock = std::chrono::steady_clock;
  const auto now = Clock::now();
  if (!perf_metrics_started_) {
    perf_metrics_started_ = true;
    perf_metrics_last_ = now;
    perf_last_hardware_cycles_ = perf_hardware_cycles_total_;
    perf_last_vi_boundaries_ = perf_vi_boundaries_total_;
    perf_last_guest_frames_ = perf_guest_frames_total_;
    perf_last_presents_ = host_present_count_;
    perf_last_interpolated_ = interpolated_present_count_;
    perf_last_dropped_ = host_present_dropped_;
    return;
  }

  const auto elapsed_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - perf_metrics_last_).count();
  if (elapsed_ns < 750000000ll || elapsed_ns <= 0) return;

  const double seconds = static_cast<double>(elapsed_ns) / 1000000000.0;
  const auto hardware_delta = perf_hardware_cycles_total_ - perf_last_hardware_cycles_;
  const auto vi_delta = perf_vi_boundaries_total_ - perf_last_vi_boundaries_;
  const auto guest_delta = perf_guest_frames_total_ - perf_last_guest_frames_;
  const auto present_delta = host_present_count_ - perf_last_presents_;
  const auto interp_delta = interpolated_present_count_ - perf_last_interpolated_;
  const auto dropped_delta = host_present_dropped_ - perf_last_dropped_;

  const double vps = static_cast<double>(vi_delta) / seconds;
  const double guest_fps = static_cast<double>(guest_delta) / seconds;
  const double present_fps = static_cast<double>(present_delta) / seconds;
  const double interp_fps = static_cast<double>(interp_delta) / seconds;
  const double speed =
      100.0 * static_cast<double>(hardware_delta) /
      (static_cast<double>(GekkoAOT::HW::VI::NativeVI::CpuClockHz) * seconds);
  const double nominal_vps =
      static_cast<double>(GekkoAOT::HW::VI::NativeVI::CpuClockHz) /
      (static_cast<double>(std::max<std::uint32_t>(vi_.TicksPerHalfLine(), 1u)) *
       static_cast<double>(std::max<std::uint32_t>(vi_.HalfLinesPerFrame(), 1u)));

  std::fprintf(stderr,
               "GEKKOAOT_PERF_V49=1 vps=%.3f nominal_vps=%.3f guest_fps=%.3f "
               "present_fps=%.3f interp_fps=%.3f speed=%.2f target=%u drops=%llu\\n",
               vps, nominal_vps, guest_fps, present_fps, interp_fps, speed,
               host_fps_limit_hz_, static_cast<unsigned long long>(dropped_delta));

  perf_metrics_last_ = now;
  perf_last_hardware_cycles_ = perf_hardware_cycles_total_;
  perf_last_vi_boundaries_ = perf_vi_boundaries_total_;
  perf_last_guest_frames_ = perf_guest_frames_total_;
  perf_last_presents_ = host_present_count_;
  perf_last_interpolated_ = interpolated_present_count_;
  perf_last_dropped_ = host_present_dropped_;
}

void HostRuntime::PollLiveVideoConfig(std::uint64_t charged) {
  if (live_video_config_path_.empty()) return;
  live_video_config_poll_cycles_ += charged;
  constexpr std::uint64_t kPollCycles =
      GekkoAOT::HW::VI::NativeVI::CpuClockHz / 4u;  // ~250 ms hardware time.
  if (live_video_config_poll_cycles_ < kPollCycles) return;
  live_video_config_poll_cycles_ %= kPollCycles;

  std::error_code ec;
  const auto mtime = std::filesystem::last_write_time(live_video_config_path_, ec);
  if (ec) return;
  if (live_video_config_mtime_valid_ && mtime == live_video_config_mtime_) return;
  live_video_config_mtime_ = mtime;
  live_video_config_mtime_valid_ = true;

  std::ifstream file(live_video_config_path_);
  if (!file) return;
  std::string line;
  std::string aspect_mode;
  bool have_aspect = false;
  bool have_fps = false;
  std::uint32_t next_fps = host_fps_limit_hz_;
  while (std::getline(file, line)) {
    constexpr std::string_view fps_prefix = "fps_limit=";
    constexpr std::string_view aspect_prefix = "aspect_ratio=";
    if (line.starts_with(fps_prefix)) {
      char* end = nullptr;
      const auto value = std::strtoul(line.c_str() + fps_prefix.size(), &end, 10);
      if (end && *end == '\0' && value >= 30u && value <= 360u) {
        next_fps = static_cast<std::uint32_t>(value);
        have_fps = true;
      }
    } else if (line.starts_with(aspect_prefix)) {
      aspect_mode = line.substr(aspect_prefix.size());
      have_aspect = true;
    }
  }

  if (have_fps && next_fps != host_fps_limit_hz_) {
    host_fps_limit_hz_ = next_fps;
    host_present_started_ = false;
    interpolation_pair_valid_ = false;
    std::fprintf(stderr,
                 "GEKKOAOT_PC_FPS_SLIDER_V48=1 target_hz=%u source=live-config domain=host-present interpolation=%u\n",
                 host_fps_limit_hz_, frame_interpolation_enabled_ ? 1u : 0u);
  }
  if (have_aspect) native_gx_.SetAspectMode(aspect_mode);
}

void HostRuntime::AdvanceRuntimeCycles(std::uint64_t charged, RunResult& result,
                                       bool idle_wait) {
  if (charged > std::numeric_limits<std::uint64_t>::max() - result.cycles)
    result.cycles = std::numeric_limits<std::uint64_t>::max();
  else
    result.cycles += charged;

  const std::uint64_t hardware_cycles =
      TakeRealtimeHardwareCycles(charged, idle_wait);
  perf_hardware_cycles_total_ += hardware_cycles;

  // v136 hot-path collapse: most AOT dispatch boundaries occur between host
  // clock samples. No native device can advance without realtime hardware
  // cycles, and producer/presentation/quit polling tolerates the same <=125 us
  // sampling latency already used by the hardware clock. Returning here avoids
  // repeated DSO calls, frame-ready probes, metrics branches and device checks
  // on the overwhelmingly common zero-time boundary.
  if (hardware_cycles == 0u) return;

  bool odd_field_boundary = false;
  bool even_field_boundary = false;
  bool frame_boundary = false;
  const bool gx_ready = native_gx_.Ready();

  if (hardware_cycles != 0u) {
  if (di_async_completion_pending_) {
    // A guest may cancel/break the DI command before the deferred completion.
    // NativeDI clears TSTART in that case; drop the scheduled TCINT instead of
    // resurrecting a cancelled command.
    if ((di_.DMAControl() & 1u) == 0u) {
      static unsigned cancel_logs = 0u;
      if (cancel_logs++ < 32u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_DI_ASYNC_V58=1 phase=cancel seq=%llu offset=%08llx dma=%08x bytes=%u\n",
                     static_cast<unsigned long long>(di_async_sequence_),
                     static_cast<unsigned long long>(di_async_disc_offset_),
                     di_async_dma_address_, di_async_bytes_);
      di_async_completion_pending_ = false;
      di_async_cycles_remaining_ = 0u;
      di_async_staging_.clear();
    } else if (hardware_cycles >= di_async_cycles_remaining_) {
      const auto sequence = di_async_sequence_;
      const auto bytes = di_async_bytes_;
      const auto dma = di_async_dma_address_;
      const auto offset = di_async_disc_offset_;
      di_async_completion_pending_ = false;
      di_async_cycles_remaining_ = 0u;
      bool published = false;
      if (di_async_staging_.size() == bytes) {
        if (auto* target = memory_.Resolve(dma, bytes)) {
          std::memcpy(target, di_async_staging_.data(), bytes);
          published = true;
        }
      }
      di_async_staging_.clear();
      const bool completed = published && di_.CompletePendingFor(sequence, true, 0u, bytes, 0u);
      if (!published) {
        ++di_read_failures_;
        di_.CompletePendingFor(sequence, false, 0u, 0u, 0x00031100u);
      }
      SyncNativeDIInterrupt();
      static unsigned complete_logs = 0u;
      if (complete_logs++ < 128u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_DI_PARITY_V143=1 phase=%s seq=%llu offset=%08llx dma=%08x bytes=%u status=%08x visibility=completion\n",
                     completed ? "complete" : (published ? "stale-drop" : "dma-fault"),
                     static_cast<unsigned long long>(sequence),
                     static_cast<unsigned long long>(offset), dma, bytes, di_.Status());
    } else {
      di_async_cycles_remaining_ -= hardware_cycles;
    }
  }

  PollLiveVideoConfig(hardware_cycles);
  AdvanceCpuTimers(hardware_cycles);
  vi_.AdvanceCycles(hardware_cycles);
  const bool vi_interrupt_update = vi_.ConsumeInterruptUpdate();
  if (vi_interrupt_update)
    SyncNativeVIInterrupt();

  odd_field_boundary = vi_.ConsumeOddFieldBoundary();
  even_field_boundary = vi_.ConsumeEvenFieldBoundary();
  frame_boundary = vi_.ConsumeFrameBoundary();
  if (frame_boundary) ++perf_vi_boundaries_total_;

  // v160: only after a complete VI frame has elapsed can an already-handled
  // CP breakpoint be classified as a scheduler/video-flip deadlock.
  ServiceIdleCpBreakpointRecovery(frame_boundary);

  // Sample host input in its own GameCube hardware-time domain. Retail SI
  // polling is commonly around 120 Hz and, unlike presentation or the AOT CPU
  // loop, this clock remains real-time. Do not replay missed samples after a
  // long host stall: one fresh snapshot is sufficient, then retain phase.
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
  if (native_input_ready_) {
    constexpr std::uint64_t kNativeInputPollCycles =
        GekkoAOT::HW::VI::NativeVI::CpuClockHz / 120u;
    static std::uint64_t native_input_hw_phase = 0u;
    native_input_hw_phase += hardware_cycles;
    if (native_input_hw_phase >= kNativeInputPollCycles) {
      native_input_hw_phase %= kNativeInputPollCycles;
      RefreshNativeInput(true);
    }
  }
#endif

  } // hardware_cycles != 0

  // Always consume the producer boundary, even when direct host presentation
  // remains VI-driven. Frame interpolation and guest-frame cadence are sourced
  // from completed GXCopyDisp frames, while gx_present_on_copy_ only decides
  // whether that producer boundary also queues the direct scanout immediately.
  // Previously the VI mode left the frame-ready latch unconsumed, so
  // NoteProducedFrame() never armed the interpolation scheduler.
  if (gx_ready) {
    std::uint32_t ready_xfb = 0;
    if (native_gx_.ConsumeFrameReady(&ready_xfb) && ready_xfb != 0u) {
      NoteProducedFrame();
      if (gx_present_on_copy_) {
        QueueHostPresent(ready_xfb, ready_xfb);
        gx_frame_ready_since_vi_ = true;
      }
    }
  }

  // VI-driven presentation must follow fields, not just the full-frame wrap.
  // Dolphin/retail semantics select TOP for the odd field and BOTTOM for the
  // even field.  If both origins are identical (typical progressive/single-XFB
  // setup), retain one present per full frame instead of presenting duplicates.
  if (gx_ready && !gx_present_on_copy_) {
    const std::uint32_t top = vi_.XfbAddressTop();
    const std::uint32_t bottom = vi_.XfbAddressBottom();
    const bool split_fields = top != 0u && bottom != 0u && top != bottom;

    if (split_fields) {
      if (odd_field_boundary) {
        QueueHostPresent(top, top);
        static unsigned odd_logs = 0;
        if (odd_logs++ < 32u)
          std::fprintf(stderr,
                       "GEKKOAOT_NATIVE_VI_FIELD_SCANOUT_V50=1 field=odd xfb=%08x top=%08x bottom=%08x\n",
                       top, top, bottom);
      }
      if (even_field_boundary) {
        QueueHostPresent(bottom, bottom);
        static unsigned even_logs = 0;
        if (even_logs++ < 32u)
          std::fprintf(stderr,
                       "GEKKOAOT_NATIVE_VI_FIELD_SCANOUT_V50=1 field=even xfb=%08x top=%08x bottom=%08x\n",
                       bottom, top, bottom);
      }
    } else if (frame_boundary) {
      const std::uint32_t xfb = top != 0u ? top : bottom;
      if (xfb != 0u) {
        QueueHostPresent(xfb, xfb);
        static unsigned frame_logs = 0;
        if (frame_logs++ < 32u)
          std::fprintf(stderr,
                       "GEKKOAOT_NATIVE_VI_FIELD_SCANOUT_V50=1 field=frame xfb=%08x top=%08x bottom=%08x\n",
                       xfb, top, bottom);
      }
    }
  }

  if (frame_boundary) {
    if (gx_ready && gx_present_on_copy_ &&
        !gx_frame_ready_since_vi_ && gx_present_fallback_vi_) {
      QueueHostPresent(vi_.XfbAddressTop(), vi_.XfbAddressBottom(),
                       vi_.XfbWidthPixels(), vi_.XfbStrideBytes(),
                       vi_.XfbFieldHeight());
    }
    gx_frame_ready_since_vi_ = false;
  }

  // Presentation is serviced from the same monotonic sample used to advance
  // hardware time. Previously ServiceHostFrameScheduler() called steady_clock
  // on *every* AOT/HLE boundary, undoing TakeRealtimeHardwareCycles' sampling
  // gate and dominating runtime profiles. A fresh sample arrives at up to 8 kHz
  // (16 kHz while idle), far above the maximum 360 Hz presentation target.
  if (hardware_clock_sampled_last_call_)
    ServiceHostFrameScheduler(hardware_clock_last_);
  ServicePerformanceMetrics(hardware_cycles);

  if (hardware_cycles != 0u) {
  native_gx_.AdvanceCycles(hardware_cycles);
  // AID/DSP audio is clocked from the GameCube audio domain, not the host
  // presentation/VI rate. This keeps sound pitch stable when v44 accelerates
  // retrace waits or when presentation runs unlocked.
  const std::uint32_t guest_aid_hz = ai_.DspSampleRateHz();
  // AX GC renders a 32-sample/ms (32 kHz nominal) mix regardless of host
  // presentation rate.  Keep the standalone AID consumer in that same domain
  // while AX HLE owns the DSP task; otherwise a transient/incorrect 48 kHz AI
  // control state drains 5 ms AX buffers 1.5x faster than they are produced,
  // causing repeated/stale chunks and the characteristic harsh/metallic audio.
  const bool native_32k_mixer =
      dsp_.NativeAxHleActive() || dsp_.NativeJAudioHleActive();
  const std::uint32_t effective_aid_hz = native_32k_mixer ? 32028u : guest_aid_hz;
  dsp_.SetAudioSampleRateHz(effective_aid_hz);
  {
    static std::uint32_t last_guest_hz = 0u;
    static std::uint32_t last_effective_hz = 0u;
    if (guest_aid_hz != last_guest_hz || effective_aid_hz != last_effective_hz) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_AUDIO_RATE_V69 guest_aid_hz=%u effective_aid_hz=%u "
                   "ax_hle=%u jaudio_hle=%u policy=%s\n",
                   guest_aid_hz, effective_aid_hz, dsp_.NativeAxHleActive() ? 1u : 0u,
                   dsp_.NativeJAudioHleActive() ? 1u : 0u,
                   dsp_.NativeAxHleActive() ? "ax-fixed-32k"
                                            : (dsp_.NativeJAudioHleActive() ? "jaudio-fixed-32k" : "guest-ai"));
      last_guest_hz = guest_aid_hz;
      last_effective_hz = effective_aid_hz;
    }
  }
  dsp_.AdvanceCycles(hardware_cycles);
  ai_.AdvanceCycles(hardware_cycles);

  // CP/DI/EXI/SI interrupt state only changes through paths that already
  // synchronize immediately on MMIO/FIFO activity. Re-polling all of them for
  // every compiled guest slice was pure overhead. AI/DSP are time-driven, so
  // retain sub-microsecond delivery latency with a 256-cycle poll quantum.
  constexpr std::uint64_t kTimedIrqSyncQuantum = 256u;
  timed_irq_sync_cycles_ += hardware_cycles;
  if (timed_irq_sync_cycles_ >= kTimedIrqSyncQuantum || frame_boundary) {
    timed_irq_sync_cycles_ %= kTimedIrqSyncQuantum;
    SyncNativeDSPInterrupt();
    SyncNativeAIInterrupt();
  }

  } // hardware_cycles != 0

  if (gx_ready && native_gx_.QuitRequested())
    result.status = RunStatus::HostRequestedExit;
}

RunStatus HostRuntime::FaultStatus() const {
  switch (fault_.kind) {
  case FaultKind::None: return RunStatus::Completed;
  case FaultKind::InstructionFallback: return RunStatus::InstructionFallback;
  case FaultKind::MmioRead: return RunStatus::UnhandledMmioRead;
  case FaultKind::MmioWrite: return RunStatus::UnhandledMmioWrite;
  case FaultKind::SprRead: return RunStatus::UnhandledSprRead;
  case FaultKind::SprWrite: return RunStatus::UnhandledSprWrite;
  case FaultKind::HleFailure: return RunStatus::NativeHleFailure;
  case FaultKind::InvalidMemoryRead: return RunStatus::InvalidGuestMemoryRead;
  case FaultKind::InvalidMemoryWrite: return RunStatus::InvalidGuestMemoryWrite;
  }
  return RunStatus::NativeHleFailure;
}

void HostRuntime::RebuildPpcHaltIndex() {
  ppc_halt_pcs_.clear();
  const auto* descriptor=module_.Descriptor();
  if(!descriptor) return;
  static constexpr std::array<std::uint32_t,5> kPattern={
      0x7c0004acu,0x60000000u,0x38600000u,0x60000000u,0x4bfffff4u,
  };
  for(std::uint32_t r=0;r<descriptor->num_code_ranges;++r) {
    const auto range=descriptor->code_ranges[r];
    if(range.end<=range.start||range.end-range.start<kPattern.size()*4u) continue;
    for(std::uint32_t pc=range.start;pc<=range.end-kPattern.size()*4u;pc+=4u) {
      bool match=true;
      for(std::uint32_t i=0;i<kPattern.size();++i) {
        std::uint32_t word=0;
        if(!memory_.Read32(pc+i*4u,&word)||word!=kPattern[i]){match=false;break;}
      }
      if(!match) continue;
      for(std::uint32_t i=0;i<kPattern.size();++i)
        ppc_halt_pcs_.push_back({pc+i*4u,pc});
      pc+=static_cast<std::uint32_t>((kPattern.size()-1u)*4u);
    }
  }
  std::sort(ppc_halt_pcs_.begin(),ppc_halt_pcs_.end(),
            [](const PpcHaltPc& a,const PpcHaltPc& b){return a.pc<b.pc;});
  std::fprintf(stderr,"GEKKOAOT_NATIVE_PPCHALT_INDEX_V1=1 entries=%zu loops=%zu\n",
               ppc_halt_pcs_.size(),ppc_halt_pcs_.size()/5u);
}

bool HostRuntime::DetectSdkPpcHalt(std::uint32_t pc, std::uint32_t* entry) const {
  // PPCHalt is static SDK code. Pre-index its five PCs once when the DOL is
  // loaded instead of hashing and reading guest RAM at every dispatch boundary.
  const auto it=std::lower_bound(ppc_halt_pcs_.begin(),ppc_halt_pcs_.end(),pc,
      [](const PpcHaltPc& item,std::uint32_t value){return item.pc<value;});
  if(it==ppc_halt_pcs_.end()||it->pc!=pc) return false;
  if(entry) *entry=it->entry;
  return true;
}

void HostRuntime::RefreshSecondaryRelMappings() {
  ++rel_mapping_refreshes_;
  std::vector<ActiveRelSection> discovered;
  const auto& ram = memory_.Mem1();
  if (ram.size() < 0x40u) {
    active_rel_sections_.clear();
    return;
  }

  struct PendingRel {
    const GekkoAOTModuleDesc* descriptor = nullptr;
    const GekkoAOTRelModule* module = nullptr;
    bool found = false;
  };
  std::vector<PendingRel> pending;
  std::unordered_multimap<std::uint32_t, std::size_t> by_module_id;
  for (const auto& library : secondary_modules_) {
    const auto* descriptor = library ? library->Descriptor() : nullptr;
    if (!descriptor || !descriptor->rel_modules) continue;
    for (std::uint32_t module_index = 0; module_index < descriptor->num_rel_modules; ++module_index) {
      const auto& module = descriptor->rel_modules[module_index];
      if (module.section_count == 0u || module.section_count > 4096u || module.num_sections == 0u)
        continue;
      const std::size_t index = pending.size();
      pending.push_back({descriptor, &module, false});
      by_module_id.emplace(module.module_id, index);
    }
  }

  if (pending.empty()) {
    active_rel_sections_.clear();
    return;
  }

  // Scan MEM1 once regardless of the number of side modules. Do not require
  // sectionInfoOffset to equal the original on-disc REL value: OSLink variants
  // can relocate/copy the section table while keeping module id/count/version
  // stable. The live header is authoritative for where its section table sits.
  for (std::uint32_t candidate = 0; std::uint64_t(candidate) + 0x40u <= ram.size();
       candidate += 4u) {
    const auto* header = ram.data() + candidate;
    const std::uint32_t module_id = ReadGuestBE32(header + 0x00);
    const auto matches = by_module_id.equal_range(module_id);
    for (auto it = matches.first; it != matches.second; ++it) {
      auto& item = pending[it->second];
      if (item.found || !item.descriptor || !item.module) continue;
      const auto& module = *item.module;
      if (ReadGuestBE32(header + 0x0c) != module.section_count ||
          ReadGuestBE32(header + 0x1c) != module.version)
        continue;

      const std::uint32_t live_section_info = ReadGuestBE32(header + 0x10);
      const std::uint64_t table_bytes = std::uint64_t(module.section_count) * 8u;
      std::uint64_t table_physical = 0;
      bool have_table = false;

      // Normal REL: sectionInfoOffset is relative to the REL header.
      if (live_section_info >= 0x20u &&
          std::uint64_t(candidate) + live_section_info + table_bytes <= ram.size()) {
        table_physical = std::uint64_t(candidate) + live_section_info;
        have_table = true;
      }
      // Some loaders leave an absolute cached/physical pointer in the live
      // bookkeeping structure. Accept either form when it lands in MEM1.
      if (!have_table && live_section_info >= 0x80000000u && live_section_info < 0x81800000u) {
        const std::uint64_t physical = std::uint64_t(live_section_info - 0x80000000u);
        if (physical + table_bytes <= ram.size()) {
          table_physical = physical;
          have_table = true;
        }
      }
      if (!have_table && live_section_info < ram.size() &&
          std::uint64_t(live_section_info) + table_bytes <= ram.size()) {
        table_physical = live_section_info;
        have_table = true;
      }
      if (!have_table) continue;

      std::vector<ActiveRelSection> candidate_sections;
      bool valid = true;
      for (std::uint32_t section_index = 0; section_index < module.num_sections; ++section_index) {
        const auto& section = module.sections[section_index];
        const std::uint64_t entry_physical = table_physical + std::uint64_t(section.section_index) * 8u;
        if (entry_physical + 8u > ram.size()) { valid = false; break; }
        const auto* entry = ram.data() + entry_physical;
        std::uint32_t runtime_start = ReadGuestBE32(entry) & ~3u;
        const std::uint32_t runtime_size = ReadGuestBE32(entry + 4u);
        if (runtime_start == 0u || runtime_size < section.size) {
          valid = false;
          break;
        }

        // Before OSLink rewrites a REL section table, section addresses are
        // file-relative. Once linked they are ordinary MEM1 addresses. Both
        // forms can be observed while a module is being installed.
        if (runtime_start < module.file_size) {
          const std::uint64_t absolute = 0x80000000ull + candidate + runtime_start;
          if (absolute > 0xffffffffull) { valid = false; break; }
          runtime_start = static_cast<std::uint32_t>(absolute);
        } else if (runtime_start < ram.size()) {
          runtime_start |= 0x80000000u;
        }
        if (runtime_start < 0x80000000u || runtime_start >= 0x81800000u) {
          valid = false;
          break;
        }
        const std::uint64_t physical = std::uint64_t(runtime_start - 0x80000000u);
        if (physical + section.size > ram.size()) { valid = false; break; }
        candidate_sections.push_back({item.descriptor, module.module_id, section.section_index,
                                      section.linked_start, runtime_start, section.size});
      }
      if (valid && !candidate_sections.empty()) {
        discovered.insert(discovered.end(), candidate_sections.begin(), candidate_sections.end());
        item.found = true;
      }
    }
  }
  active_rel_sections_ = std::move(discovered);
}

const GekkoAOTModuleDesc* HostRuntime::SelectFixedExecutableModule(
    std::uint32_t runtime_address, bool* overlap) {
  if (overlap) *overlap = false;

  struct Candidate {
    const GekkoAOTModuleDesc* descriptor = nullptr;
    std::uint32_t chunk = 0u;
    int secondary_index = -1;
  };
  std::array<Candidate, 32> candidates{};
  std::size_t candidate_count = 0u;

  const auto add_candidate = [&](const GekkoAOTModuleDesc* descriptor,
                                 int secondary_index) {
    if (!descriptor || descriptor->num_rel_modules != 0u ||
        !descriptor->chunk_hashes || candidate_count >= candidates.size())
      return;
    std::uint32_t chunk = 0u;
    if (!FindModuleChunk(descriptor, runtime_address, &chunk)) return;
    if (descriptor->chunk_hashes[chunk] == 0u) return;
    candidates[candidate_count++] = {descriptor, chunk, secondary_index};
  };

  add_candidate(module_.Descriptor(), -1);
  for (std::size_t i = 0; i < secondary_modules_.size(); ++i) {
    const auto* descriptor = secondary_modules_[i] ? secondary_modules_[i]->Descriptor() : nullptr;
    add_candidate(descriptor, static_cast<int>(i));
  }

  if (candidate_count < 2u) {
    if (fixed_exec_route_cache_.valid &&
        (runtime_address < fixed_exec_route_cache_.chunk_start ||
         runtime_address >= fixed_exec_route_cache_.chunk_end))
      fixed_exec_route_cache_ = {};
    return nullptr;
  }
  if (overlap) *overlap = true;

  if (fixed_exec_route_cache_.valid &&
      runtime_address >= fixed_exec_route_cache_.chunk_start &&
      runtime_address < fixed_exec_route_cache_.chunk_end) {
    for (std::size_t i = 0; i < candidate_count; ++i)
      if (candidates[i].descriptor == fixed_exec_route_cache_.descriptor)
        return fixed_exec_route_cache_.descriptor;
  }
  fixed_exec_route_cache_ = {};

  // v155: fixed executable identity is an instruction-cache epoch, not a
  // property that needs to be rediscovered at every function boundary.  MKDD
  // spends a lot of time bouncing through OSSaveContext/OSLoadContext and the
  // thread scheduler during course transitions.  With safe host boundaries
  // enabled every AOT dispatch returns here, and v87 re-hashed the current
  // function (plus neighbours) every time it crossed a chunk boundary.
  //
  // Once an executable has been verified, keep routing all of its covered
  // chunks to that descriptor until the guest executes ICBI.  CacheControl()
  // already clears fixed_exec_active_descriptor_ and the route cache on ICBI,
  // which is the architectural publication boundary a loader must use before
  // executing newly written PowerPC instructions.  Startup/reset also begins
  // with no active descriptor, so the full hash resolver below still proves
  // the initial image and every post-ICBI replacement.
  if (fixed_exec_active_descriptor_) {
    for (std::size_t i = 0; i < candidate_count; ++i) {
      const auto& candidate = candidates[i];
      if (candidate.descriptor != fixed_exec_active_descriptor_) continue;
      const auto range = candidate.descriptor->chunk_ranges[candidate.chunk];
      fixed_exec_route_cache_ = {candidate.descriptor, range.start, range.end, true};
      static unsigned active_epoch_logs = 0u;
      if (active_epoch_logs++ < 64u)
        std::fprintf(stderr,
                     "GEKKOAOT_FIXED_EXEC_ROUTER_V155=1 action=keep-active-icache-epoch pc=%08x chunk=%08x-%08x candidates=%zu invalidations=%llu\n",
                     runtime_address, range.start, range.end, candidate_count,
                     static_cast<unsigned long long>(icbi_route_invalidations_));
      return candidate.descriptor;
    }
  }

  // First establish which images match the *current* guest chunk. Unlike v84,
  // do not immediately choose the first match: SDK/library code is often byte-
  // identical in main.dol and one or more overlays, which made `main` win even
  // after a secondary executable had replaced the surrounding image.
  std::array<std::size_t, 32> matching{};
  std::size_t matching_count = 0u;
  for (std::size_t i = 0; i < candidate_count; ++i) {
    const auto& candidate = candidates[i];
    const auto range = candidate.descriptor->chunk_ranges[candidate.chunk];
    const std::size_t size = static_cast<std::size_t>(range.end - range.start);
    const auto* bytes = memory_.Resolve(range.start, size);
    if (!bytes) continue;
    if (Fnv1a64(bytes, size) == candidate.descriptor->chunk_hashes[candidate.chunk])
      matching[matching_count++] = i;
  }

  const auto source_index = [&](const GekkoAOTModuleDesc* descriptor) -> int {
    if (descriptor == module_.Descriptor()) return -1;
    for (std::size_t i = 0; i < secondary_modules_.size(); ++i)
      if (secondary_modules_[i] && secondary_modules_[i]->Descriptor() == descriptor)
        return static_cast<int>(i);
    return -2;
  };

  const auto commit = [&](const Candidate& candidate, const char* reason,
                          int score, unsigned probes) -> const GekkoAOTModuleDesc* {
    const auto range = candidate.descriptor->chunk_ranges[candidate.chunk];
    const bool changed = fixed_exec_active_descriptor_ != candidate.descriptor;
    fixed_exec_active_descriptor_ = candidate.descriptor;
    fixed_exec_route_cache_ = {candidate.descriptor, range.start, range.end, true};
    if (changed) ++fixed_exec_route_switches_;
    static unsigned route_logs = 0u;
    if (route_logs++ < 192u) {
      const int index = source_index(candidate.descriptor);
      std::fprintf(stderr,
                   "GEKKOAOT_FIXED_EXEC_ROUTER_V87=1 action=%s pc=%08x chunk=%08x-%08x source=%s index=%d candidates=%zu matching=%zu score=%d probes=%u switches=%llu\\n",
                   reason, runtime_address, range.start, range.end,
                   index == -1 ? "main" : "secondary", index, candidate_count,
                   matching_count, score, probes,
                   static_cast<unsigned long long>(fixed_exec_route_switches_));
    }
    return candidate.descriptor;
  };

  if (matching_count == 1u)
    return commit(candidates[matching[0]], "select-unique", 0, 1u);

  if (matching_count > 1u) {
    struct Score {
      int value = std::numeric_limits<int>::min();
      unsigned probes = 0u;
      unsigned matches = 0u;
      unsigned mismatches = 0u;
    };
    std::array<Score, 32> scores{};

    // Score a small neighborhood around the current chunk. This is cold-path
    // work (only on entry to a new overlapping chunk) and lets unique portions
    // of the live executable disambiguate a shared SDK/library chunk.
    constexpr int kRadius = 8;
    for (std::size_t m = 0; m < matching_count; ++m) {
      const std::size_t ci = matching[m];
      const auto& candidate = candidates[ci];
      Score score{};
      score.value = 0;
      const int center = static_cast<int>(candidate.chunk);
      for (int distance = 0; distance <= kRadius; ++distance) {
        for (int side = distance == 0 ? 0 : -1; side <= 1; side += distance == 0 ? 2 : 2) {
          const int idx = center + (distance == 0 ? 0 : side * distance);
          if (idx < 0 || idx >= static_cast<int>(candidate.descriptor->num_chunk_ranges)) continue;
          const auto range = candidate.descriptor->chunk_ranges[idx];
          const auto expected = candidate.descriptor->chunk_hashes[idx];
          if (!expected || range.end <= range.start) continue;
          const std::size_t size = static_cast<std::size_t>(range.end - range.start);
          // Refuse absurd metadata from poisoning the diagnostic hash path.
          if (size > 4u * 1024u * 1024u) continue;
          const auto* bytes = memory_.Resolve(range.start, size);
          if (!bytes) continue;
          ++score.probes;
          if (Fnv1a64(bytes, size) == expected) {
            ++score.matches;
            score.value += 4;
          } else {
            ++score.mismatches;
            score.value -= 7;
          }
          if (score.probes >= 12u) break;
        }
        if (score.probes >= 12u) break;
      }
      scores[ci] = score;
    }

    int best_score = std::numeric_limits<int>::min();
    std::size_t best = matching[0];
    bool tie = false;
    for (std::size_t m = 0; m < matching_count; ++m) {
      const auto ci = matching[m];
      if (scores[ci].value > best_score) {
        best_score = scores[ci].value;
        best = ci;
        tie = false;
      } else if (scores[ci].value == best_score) {
        tie = true;
      }
    }

    if (!tie && scores[best].matches >= 2u)
      return commit(candidates[best], "select-neighborhood", best_score, scores[best].probes);

    // If all nearby chunks are genuinely identical, image identity cannot be
    // recovered from this PC alone. Preserve a previously verified image when
    // it is still one of the byte-matching candidates instead of reverting to
    // main.dol merely because it appears first in the descriptor list.
    if (fixed_exec_active_descriptor_) {
      for (std::size_t m = 0; m < matching_count; ++m) {
        const auto ci = matching[m];
        if (candidates[ci].descriptor == fixed_exec_active_descriptor_)
          return commit(candidates[ci], "keep-active-identical", scores[ci].value,
                        scores[ci].probes);
      }
    }
  }

  fixed_exec_active_descriptor_ = nullptr;
  static unsigned miss_logs = 0u;
  if (miss_logs++ < 64u)
    std::fprintf(stderr,
                 "GEKKOAOT_FIXED_EXEC_ROUTER_V87=0 action=identity-unresolved pc=%08x candidates=%zu matching=%zu fallback=legacy-address-routing\\n",
                 runtime_address, candidate_count, matching_count);
  return nullptr;
}

bool HostRuntime::DispatchFixedExecutableModule(const GekkoAOTModuleDesc* descriptor,
                                                std::uint32_t runtime_address,
                                                RunResult& result) {
  if (!descriptor || !descriptor->dispatch) return false;
  RecordCompatBreadcrumb(runtime_address, CompatDispatchKind::Secondary);
  cpu_.pc = runtime_address;
  cpu_.downcount = 0;
  cpu_.external_read_count = 0;
  cpu_.external_write_count = 0;
  if (!descriptor->dispatch(&cpu_, runtime_address)) {
    cpu_.pc = runtime_address;
    return false;
  }
  ++secondary_dispatches_;
  ++result.dispatches;
  if (cpu_.downcount < 0) {
    const std::uint64_t charged =
        static_cast<std::uint64_t>(-(cpu_.downcount + 1)) + 1u;
    AdvanceRuntimeCycles(charged, result);
  }
  return true;
}

bool HostRuntime::ResolveSecondaryAddress(std::uint32_t runtime_address,
                                          const GekkoAOTModuleDesc** descriptor,
                                          std::uint32_t* linked_address) {
  if (!descriptor || !linked_address) return false;
  *descriptor = nullptr;
  *linked_address = runtime_address;

  const auto resolve_active = [&]() -> bool {
    for (const auto& section : active_rel_sections_) {
      const std::uint64_t end = std::uint64_t(section.runtime_start) + section.size;
      if (runtime_address < section.runtime_start || std::uint64_t(runtime_address) >= end)
        continue;
      const std::uint32_t linked =
          section.linked_start + (runtime_address - section.runtime_start);
      // active_rel_sections_ also contains data/BSS in v37. Only executable
      // coverage may be entered through the dispatcher.
      if (!section.owner ||
          !AddressInRanges(section.owner->code_ranges, section.owner->num_code_ranges, linked))
        continue;
      *descriptor = section.owner;
      *linked_address = linked;
      return true;
    }
    return false;
  };
  if (resolve_active()) return true;

  // Fixed-address DOL/ELF side modules can be dispatched directly. REL modules
  // are normally resolved through OSLink mappings, but if a title happens to
  // link a REL at the same address chosen by AOT (SMB2 does this in some
  // layouts), the descriptor's exact coverage is also a safe direct mapping.
  const auto resolve_canonical = [&]() -> bool {
    for (const auto& library : secondary_modules_) {
      const auto* module = library ? library->Descriptor() : nullptr;
      if (module && AddressInRanges(module->code_ranges, module->num_code_ranges, runtime_address)) {
        *descriptor = module;
        *linked_address = runtime_address;
        return true;
      }
    }
    return false;
  };

  // Prefer live OSLink mappings over canonical fallback, then fixed/canonical
  // coverage. This lets dynamically relocated RELs win when both are present.
  RefreshSecondaryRelMappings();
  if (resolve_active()) return true;
  return resolve_canonical();
}

std::uint32_t HostRuntime::TranslateSecondaryAddress(const GekkoAOTModuleDesc* descriptor,
                                                     std::uint32_t linked_address) const {
  (void)descriptor;
  // A relocation in REL A may branch directly to the canonical linked address
  // of REL B. active_rel_sections_ now includes data too, so require native
  // code coverage for PC translation while the memory bridge below can use all
  // sections.
  for (const auto& section : active_rel_sections_) {
    const std::uint64_t end = std::uint64_t(section.linked_start) + section.size;
    if (linked_address < section.linked_start || std::uint64_t(linked_address) >= end)
      continue;
    if (!section.owner ||
        !AddressInRanges(section.owner->code_ranges, section.owner->num_code_ranges,
                         linked_address))
      continue;
    return section.runtime_start + (linked_address - section.linked_start);
  }
  return linked_address;
}

bool HostRuntime::TranslateCanonicalRelAddress(std::uint32_t canonical_address,
                                               std::uint32_t* runtime_address) const {
  if (!runtime_address) return false;
  for (const auto& section : active_rel_sections_) {
    const std::uint64_t end = std::uint64_t(section.linked_start) + section.size;
    if (canonical_address >= section.linked_start &&
        std::uint64_t(canonical_address) < end) {
      *runtime_address = section.runtime_start + (canonical_address - section.linked_start);
      return true;
    }
  }
  return false;
}

bool HostRuntime::DispatchSecondaryModule(std::uint32_t runtime_address, RunResult& result) {
  const GekkoAOTModuleDesc* resolved = nullptr;
  std::uint32_t linked_address = runtime_address;

  const auto commit_dispatch = [&](const GekkoAOTModuleDesc* descriptor,
                                   std::uint32_t address) -> bool {
    if (!descriptor || !descriptor->dispatch) return false;
    RecordCompatBreadcrumb(address, CompatDispatchKind::Secondary);
    cpu_.pc = address;
    cpu_.downcount = 0;
    cpu_.external_read_count = 0;
    cpu_.external_write_count = 0;
    if (!descriptor->dispatch(&cpu_, address)) return false;
    cpu_.pc = TranslateSecondaryAddress(descriptor, cpu_.pc);
    ++secondary_dispatches_;
    ++result.dispatches;
    if (cpu_.downcount < 0) {
      const std::uint64_t charged =
          static_cast<std::uint64_t>(-(cpu_.downcount + 1)) + 1u;
      AdvanceRuntimeCycles(charged, result);
    }
    return true;
  };

  if (ResolveSecondaryAddress(runtime_address, &resolved, &linked_address) && resolved &&
      commit_dispatch(resolved, linked_address))
    return true;

  for (const auto& library : secondary_modules_) {
    const auto* descriptor = library ? library->Descriptor() : nullptr;
    if (!descriptor || descriptor == resolved || descriptor->num_rel_modules != 0u)
      continue;
    if (!commit_dispatch(descriptor, runtime_address)) continue;

    static unsigned recovery_logs = 0;
    if (recovery_logs < 8u) {
      std::fprintf(stderr,
                   "GEKKOAOT_SECONDARY_PROBE_RECOVER=1 address=0x%08x game_id=%s\n",
                   runtime_address, descriptor->game_id);
      ++recovery_logs;
    }
    return true;
  }

  cpu_.pc = runtime_address;
  return false;
}

RunResult HostRuntime::Run(std::uint64_t dispatch_limit) {
  RunResult result{};
  if (!module_.Descriptor()) {
    result.status = RunStatus::ModuleNotLoaded;
    return result;
  }

  const auto* descriptor = module_.Descriptor();
  fault_ = {};

  // Device MMIO writes and AdvanceRuntimeCycles keep PI causes synchronized.
  // Seed the initial state once, rather than polling every device again before
  // every dispatch (the old loop duplicated the end-of-dispatch synchronization).
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_CP_FIFO_V88=1 drain=32-byte-step breakpoint=stop+irq fifo-top=base+size-4 wrap=exact\n");
  SyncNativeCPInterrupt();
  SyncNativeDSPInterrupt();
  SyncNativeAIInterrupt();
  SyncNativeDIInterrupt();
  SyncNativeEXIInterrupt();
  SyncNativeSIInterrupt();

  const std::uint32_t native_idle_sleep_us =
      RuntimeEnvU32("GEKKOAOT_NATIVE_IDLE_SLEEP_US", 0u, 1000u);
  std::fprintf(stderr,
               "GEKKOAOT_RUNTIME_PERF_V140=1 clock=tsc-or-steady zero-cycle=fast-return external-pointer=ram-first wgpipe=early-fastpath irq-probe=single-load nativeos=o1-best+priority-coherent safe-chain=%lld spin-yield=off context-drop=off idle-sleep-us=%u\n",
               static_cast<long long>(cpu_.cycle_budget), native_idle_sleep_us);

  while (dispatch_limit == 0 || result.dispatches < dispatch_limit) {
    if (cpu_.pc == 0u) {
      result.status = RunStatus::Completed;
      break;
    }

    std::uint32_t halt_entry = 0;
    if (DetectSdkPpcHalt(cpu_.pc, &halt_entry)) {
      RecordCompatBreadcrumb(cpu_.pc, CompatDispatchKind::Main);
      ReportCompatFailure("guest-halt", halt_entry, 0u, false);
      result.status = RunStatus::GuestHalted;
      result.fault_address = halt_entry;
      break;
    }

    auto& native_os = GekkoAOT::NativeOS::Service::Get();
    if (native_os.IsIdle() && cpu_.pc == native_os.IdlePc()) {
      RecordCompatBreadcrumb(cpu_.pc, CompatDispatchKind::Idle);
      // Native equivalent of Dolphin OS SelectThread's IdleContext loop. First
      // consume a thread made runnable by a previous interrupt callback. If the
      // run queue is still empty, allow already-pending asynchronous exceptions
      // to enter the real guest vector, otherwise advance native devices until
      // the next interrupt opportunity. No guest scheduler code or Dolphin
      // fallback participates in this path.
      if (native_os.ResumeIdleIfReady(&cpu_)) continue;

      if (native_os.Poll(&cpu_, cpu_.pc)) {
        ++hle_calls_;
        continue;
      }
      if (cpu_.pc != native_os.IdlePc()) continue;

      if (TryTakeExternalInterrupt() || TryTakeDecrementerInterrupt()) continue;

      constexpr std::uint64_t kNativeIdleMaxQuantum = 4096u;
      const std::uint64_t idle_quantum = std::max<std::uint64_t>(
          1u, std::min<std::uint64_t>(kNativeIdleMaxQuantum, vi_.TicksPerHalfLine()));
      // v136b: do not park by default. On Linux a nominal 50-us sleep may
      // overshoot enough to shave throughput/latency on titles that enter the
      // SDK idle context between render bursts. The O(1) NativeOS scheduler,
      // TSC clock and zero-cycle fast return retain most of v136's CPU savings
      // without sleeping. Parking remains an explicit power-saving opt-in.
      if (native_idle_sleep_us != 0u)
        std::this_thread::sleep_for(std::chrono::microseconds(native_idle_sleep_us));
      // v46 virtualizes only the VI wait gate while NativeOS is genuinely
      // idle. The guest clock itself is wall-paced to 1.0x, so a faster host
      // cannot make game logic/audio randomly run faster. CPU timebase/DEC,
      // DSP, AI and GX still receive the original idle_quantum below.
      AdvanceRuntimeCycles(idle_quantum, result, true);
      ++result.dispatches;
      if (result.status == RunStatus::HostRequestedExit) break;
      continue;
    }

    // At a dispatch boundary cpu_.pc is the next guest instruction, matching
    // the architectural SRR0 value for an asynchronous external interrupt.
    // Do this before NativeOS/HLE dispatch so the real low-memory 0x500 vector
    // owns interrupt delivery and can wake the guest scheduler normally.
    TryTakeExternalInterrupt();
    TryTakeDecrementerInterrupt();

    const std::uint32_t native_os_pc = cpu_.pc;
    if (GekkoAOT::NativeOS::Service::Get().Poll(&cpu_, native_os_pc)) {
      RecordCompatBreadcrumb(native_os_pc, CompatDispatchKind::NativeHle);
      ++hle_calls_;
      continue;
    }

    cpu_.downcount = 0;
    cpu_.external_read_count = 0;
    cpu_.external_write_count = 0;
    const std::uint32_t dispatch_pc = cpu_.pc;
    RecordCompatBreadcrumb(dispatch_pc, CompatDispatchKind::Main);

    std::uint64_t low_memory_cycles = 0;
    if (ExecuteLowMemoryInstruction(dispatch_pc, &low_memory_cycles)) {
      if (compat_diagnostics_enabled_ && compat_breadcrumb_sequence_ != 0u)
        compat_breadcrumbs_[(compat_breadcrumb_next_ + kCompatBreadcrumbCount - 1u) %
                            kCompatBreadcrumbCount].kind = CompatDispatchKind::LowMemory;
      if (fault_.kind != FaultKind::None) {
        result.status = FaultStatus();
        result.fault_address = fault_.address;
        result.fault_value = fault_.value;
        break;
      }
      ++result.dispatches;
      AdvanceRuntimeCycles(low_memory_cycles, result);
      if (result.status == RunStatus::HostRequestedExit) break;
      continue;
    }

    // v84: fixed DOL/ELF executables may replace each other at identical
    // GameCube addresses. Address coverage alone cannot identify the live image;
    // verify the current MEM1 chunk against build-time hashes before allowing a
    // stale main/side module to win an overlap. Unique-address code stays on the
    // existing zero-hash hot path.
    bool fixed_overlap = false;
    const auto* fixed_descriptor = SelectFixedExecutableModule(dispatch_pc, &fixed_overlap);
    if (fixed_overlap && fixed_descriptor && fixed_descriptor != descriptor) {
      if (DispatchFixedExecutableModule(fixed_descriptor, dispatch_pc, result)) {
        if (fault_.kind != FaultKind::None) {
          result.status = FaultStatus();
          result.fault_address = fault_.address;
          result.fault_value = fault_.value;
          break;
        }
        if (result.status == RunStatus::HostRequestedExit) break;
        continue;
      }
      std::fprintf(stderr,
                   "GEKKOAOT_FIXED_EXEC_ROUTER_V84=0 action=verified-dispatch-rejected pc=%08x fallback=legacy-address-routing\n",
                   dispatch_pc);
    }

    if (dispatch_pc == 0x80022584u && cpu_.lr == 0x8002f410u &&
        cpu_.gpr[20] == 0x803e1b50u && cpu_.gpr[30] == 0u &&
        std::getenv("GEKKOAOT_GX_DIAG_GLYPH_ENTRY"))
      std::fprintf(stderr,
                   "GEKKOAOT_GX_DIAG_GLYPH_ENTRY f1=%.9g f2=%.9g f3=%.9g f4=%.9g r3=%08x r23=%08x fpscr=%08x\n",
                   cpu_.fpr[1], cpu_.fpr[2], cpu_.fpr[3], cpu_.fpr[4],
                   cpu_.gpr[3], cpu_.gpr[23], cpu_.fpscr);
    if (!descriptor->dispatch(&cpu_, dispatch_pc)) {
      // Runtime-loaded RELs live at OSLink-selected addresses that are not part
      // of main.dol. Translate that live PC to the secondary module's stable
      // DolRecomp link address and run its native dispatcher first.
      if (DispatchSecondaryModule(dispatch_pc, result)) {
        if (fault_.kind != FaultKind::None) {
          result.status = FaultStatus();
          result.fault_address = fault_.address;
          result.fault_value = fault_.value;
          break;
        }
        if (result.status == RunStatus::HostRequestedExit) break;
        continue;
      }

      // A host call may have consumed the address. This path matters for older
      // generated modules that return false after a host interception.
      if (DispatchHostCall(dispatch_pc)) {
        if (fault_.kind != FaultKind::None) {
          result.status = FaultStatus();
          break;
        }
        ++result.dispatches;
        continue;
      }
      if (fault_.kind == FaultKind::None)
        ReportCompatFailure("uncompiled-address", dispatch_pc, 0u);
      result.status = fault_.kind == FaultKind::None ? RunStatus::UncompiledAddress : FaultStatus();
      result.fault_address = fault_.kind == FaultKind::None ? dispatch_pc : fault_.address;
      result.fault_value = fault_.kind == FaultKind::None ? 0u : fault_.value;
      break;
    }

    ++result.dispatches;
    if (cpu_.downcount < 0) {
      const std::uint64_t charged = static_cast<std::uint64_t>(-(cpu_.downcount + 1)) + 1u;
      AdvanceRuntimeCycles(charged, result);
      if (result.status == RunStatus::HostRequestedExit) break;
    }

    if (fault_.kind != FaultKind::None) {
      result.status = FaultStatus();
      result.fault_address = fault_.address;
      result.fault_value = fault_.value;
      break;
    }
  }

  if (dispatch_limit != 0 && result.dispatches >= dispatch_limit && cpu_.pc != 0u &&
      fault_.kind == FaultKind::None)
    result.status = RunStatus::DispatchLimitReached;
  result.program_counter = cpu_.pc;
  result.hle_calls = hle_calls_;
  result.secondary_dispatches = secondary_dispatches_;
  result.rel_mapping_refreshes = rel_mapping_refreshes_;
  result.rel_address_translations = rel_address_translations_;
  result.rel_address_misses = rel_address_misses_;
  result.rel_last_canonical = rel_last_canonical_;
  result.rel_last_runtime = rel_last_runtime_;
  result.link_register = cpu_.lr;
  result.msr = cpu_.msr;
  result.srr0 = cpu_.srr0;
  result.srr1 = cpu_.srr1;
  result.exception = cpu_.exception;
  result.pi_cause = pi_.InterruptCauseValue();
  result.pi_mask = pi_.InterruptMaskValue();
  result.timebase = cpu_.timebase;
  result.di_reads = di_reads_;
  result.di_read_bytes = di_read_bytes_;
  result.di_read_failures = di_read_failures_;
  result.di_last_opcode = di_.LastOpcode();
  result.di_command0 = di_.Command0();
  result.di_status = di_.Status();
  result.di_control = di_.DMAControl();
  result.di_last_error = di_.LastError();
  result.decrementer = spr_state_[kSprDec];
  result.decrementer_pending = decrementer_pending_;
  result.last_mmio_read = last_mmio_read_;
  result.mmio_reads = mmio_reads_;
  result.same_mmio_read_streak = same_mmio_read_streak_;
  result.dsp_control = dsp_.Control();
  result.dsp_mailbox = dsp_.DspMailbox();
  result.dsp_mail_pending = dsp_.DspMailboxPending();
  result.dsp_bootstrap_loaded = dsp_.SdkBootstrapLoaded();
  result.dsp_bootstrap_running = dsp_.SdkBootstrapRunning();
  result.dsp_bootstrap_completed = dsp_.SdkBootstrapCompleted();
  result.dsp_task_loader_stage = dsp_.SdkTaskLoaderStage();
  result.dsp_task_init_pending = dsp_.SdkTaskInitPending();
  result.dsp_task_init_count = dsp_.SdkTaskInitCount();
  result.dsp_cpu_mail = dsp_.LastCpuMailbox();
  result.dsp_cpu_mails = dsp_.CpuMailboxWriteCount();
  result.dsp_native_core = dsp_.NativeCoreAvailable();
  result.dsp_rom_loaded = dsp_.NativeCoreRomLoaded();
  result.dsp_native_pc = dsp_.NativeCorePc();
  result.dsp_native_last_opcode = dsp_.NativeCoreLastOpcode();
  result.dsp_native_instructions = dsp_.NativeCoreInstructions();
  result.dsp_audio_frames = dsp_.AudioFramesSubmitted();
  result.stack_pointer = cpu_.gpr[1];

  if (result.status == RunStatus::GuestHalted) {
    // Fatal SDK paths such as OSPanic preserve useful caller/argument data on
    // the guest stack even though PPCHalt itself is just an infinite loop.
    // Keep the fields generic: these offsets are diagnostics only and do not
    // alter guest state or assume a particular game symbol address.
    const std::uint32_t sp = result.stack_pointer;
    (void)memory_.Read32(sp + 0x08u, &result.halt_stack_word_08);
    (void)memory_.Read32(sp + 0x0cu, &result.halt_stack_word_0c);
    (void)memory_.Read32(sp + 0x10u, &result.halt_stack_word_10);
    (void)memory_.Read32(sp + 0x94u, &result.halt_stack_word_94);

    const auto read_guest_text = [this](std::uint32_t address) {
      std::string out;
      if (address == 0u) return out;
      out.reserve(96);
      for (std::uint32_t i = 0; i < 160u; ++i) {
        std::uint8_t ch = 0;
        if (!memory_.Read8(address + i, &ch)) {
          out.clear();
          break;
        }
        if (ch == 0u) break;
        if (ch == '\n' || ch == '\r' || ch == '\t' || (ch >= 0x20u && ch <= 0x7eu))
          out.push_back(static_cast<char>(ch));
        else {
          out.clear();
          break;
        }
      }
      return out;
    };
    result.halt_text_08 = read_guest_text(result.halt_stack_word_08);
    result.halt_text_10 = read_guest_text(result.halt_stack_word_10);
  }

  if (result.fault_address == 0 && fault_.kind != FaultKind::None)
    result.fault_address = fault_.address;
  if (result.fault_value == 0 && fault_.kind != FaultKind::None)
    result.fault_value = fault_.value;
  const double guest_fps = produced_frame_period_.count() > 0
      ? 1000000000.0 / static_cast<double>(produced_frame_period_.count())
      : 0.0;
  std::fprintf(stderr,
               "GEKKOAOT_HOST_FRAME_STATS_V48 target_hz=%u guest_fps=%.3f presents=%llu interpolated=%llu direct=%llu collapsed=%llu\n",
               host_fps_limit_hz_, guest_fps,
               static_cast<unsigned long long>(host_present_count_),
               static_cast<unsigned long long>(interpolated_present_count_),
               static_cast<unsigned long long>(direct_present_count_),
               static_cast<unsigned long long>(host_present_dropped_));
  return result;
}

} // namespace GekkoAOT::Native
