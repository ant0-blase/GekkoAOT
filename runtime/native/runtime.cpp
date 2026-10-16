// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdk/dvd_fst_service.h"
#include "sdk/gx_array_service.h"
#include "native/runtime.h"
#include "native/sdk_autoresolver.h"
#include "native/rel_identity.h"
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
#include "platform/sdl_input.h"
#endif

#include "core/time.h"
#include "gx/frustum_assist.h"
#include "os/sdk_idle_poll.h"
#include "NativeHW/SI/native_si.h"
#include "NativeHW/DI/native_di.h"
#include "NativeHW/VI/native_vi.h"
#include "NativeHW/AI/native_ai.h"
#include "NativeHW/PI/native_pi.h"
#include "NativeHW/MI/native_mi.h"
#include "NativeHW/CP/native_cp.h"
#include "NativeHW/DSP/native_dsp.h"
#include "NativeHW/events.h"
#include "video/native_vp6.h"

#ifdef GEKKOAOT_HAVE_NOD
#include <nod.h>
#endif

#include <algorithm>
#include <unordered_map>
#include <atomic>
#include <bit>
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
// v274: asked by generated mtmsr code when MSR[EE] is set (DolRecomp patch
// gekkoaot-ee-pending-query-v274); true means "leave AOT code now".
constexpr std::uint32_t kEeEnableQuery = 0xfffffff4u;

bool RelGprTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("GEKKOAOT_REL_GPR_TRACE");
    return value && value[0] == '1' && value[1] == '\0';
  }();
  return enabled;
}

void TraceRelGprs(const char* phase,
                  std::uint32_t runtime_address,
                  std::uint32_t linked_address,
                  const CPUState& cpu) {
  if (!RelGprTraceEnabled()) return;

  static std::atomic<unsigned> sequence{0u};
  // Once the bounded trace is full, avoid millions of locked increments.
  if (sequence.load(std::memory_order_relaxed) >= 256u) return;
  const unsigned seq =
      sequence.fetch_add(1u, std::memory_order_relaxed);

  if (seq >= 256u) return;

  std::fprintf(stderr,
               "GEKKOAOT_REL_GPR_TRACE_V209=1 seq=%u phase=%s "
               "runtime=%08x linked=%08x pc=%08x lr=%08x ctr=%08x "
               "r27=%08x r28=%08x r29=%08x r30=%08x r31=%08x\n",
               seq, phase,
               runtime_address, linked_address,
               cpu.pc, cpu.lr, cpu.ctr,
               cpu.gpr[27], cpu.gpr[28], cpu.gpr[29],
               cpu.gpr[30], cpu.gpr[31]);
}


// v277: REL/native-region dispatch scratch belongs to one HostRuntime/CPU, not
// to the host thread. Keeping it in thread_local storage forced the hot
// secondary-dispatch path through __tls_get_addr on ELF/glibc builds. The
// state now lives in HostRuntime itself, which also makes nested save/restore
// explicit and avoids process-global coupling between runtime instances.

// v70 compile-time SDK lowering.  DolRecomp emits these synthetic host-call
// tokens only for entrypoints proven by the immutable build-time SDK manifest.
// They bypass guest-address lookup while keeping the same native service ABI.
constexpr std::uint32_t kStaticDirectOsBase = 0xfffd0000u;
constexpr std::uint32_t kStaticDirectSimpleBase = 0xfffc0000u;
constexpr std::uint32_t kStaticDirectMask = 0xffffff00u;
constexpr std::uint8_t kRelAddressHandled = 0xfeu;
// DolRecomp v41 writes this host-scratch byte only when a statically proven
// pure RAM polling loop exhausts its short guard. It is consumed immediately
// after the native dispatch and never exposed to guest code.
constexpr std::uint8_t kAotPollParkMarker = 0xfdu;
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

// v320: per-call environment scans on hot service/DI paths (each getenv walks
// the whole environment). These switches are process policy: read them once.
bool EnvNativeVfs() { static const bool value = RuntimeEnvBool("GEKKOAOT_NATIVE_VFS", true); return value; }
bool EnvTraceDvd() { static const bool value = RuntimeEnvBool("GEKKOAOT_TRACE_DVD", false); return value; }
bool EnvVfsGcTiming() { static const bool value = RuntimeEnvBool("GEKKOAOT_VFS_GC_TIMING", false); return value; }
bool EnvFastDisc() { static const bool value = RuntimeEnvBool("GEKKOAOT_FAST_DISC", false); return value; }
bool EnvNativeDvdFst() { static const bool value = RuntimeEnvBool("GEKKOAOT_NATIVE_DVD_FST", true); return value; }
bool EnvNativePad() { static const bool value = RuntimeEnvBool("GEKKOAOT_NATIVE_PAD", true); return value; }
bool EnvGxMatrixHle() { static const bool value = RuntimeEnvBool("GEKKOAOT_GX_MATRIX_HLE", true); return value; }
bool EnvNativeGxNormalMatrix() { static const bool value = RuntimeEnvBool("GEKKOAOT_NATIVE_GX_NORMAL_MATRIX", true); return value; }

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
  // v277: keep safe-host-boundary mode bounded, but do not bounce through the
  // runtime after every tiny AOT function. 4096 Gekko cycles are only ~8.4 us,
  // well below the runtime's 125 us host-clock service quantum. MMIO/HLE,
  // exceptions, poll-park markers and host-effect epochs still terminate the
  // chain immediately, so this coalesces pure CPU/MEM1 work rather than hiding
  // observable device transitions. GEKKOAOT_SAFE_CHAIN_CYCLES=0 restores the
  // old single-dispatch compatibility mode.
  constexpr std::uint64_t kDefault = 4096u;
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

// v301: GEKKOAOT_NATIVE_SERVICE_DISABLE=memset,strlen,... (or "all") makes the
// named SDK leaf services decline so the original AOT guest body runs instead.
// A compatibility/bisect switch that needs no recompile of the module.
std::uint64_t DisabledSimpleServices() {
  static const std::uint64_t mask = [] {
    std::uint64_t bits = 0u;
    const char* list = std::getenv("GEKKOAOT_NATIVE_SERVICE_DISABLE");
    if (!list || !*list) return bits;
    std::string_view rest(list);
    while (!rest.empty()) {
      const auto comma = rest.find(',');
      const auto name = rest.substr(0, comma);
      for (unsigned kind = 0; kind < static_cast<unsigned>(SimpleHleKind::Count); ++kind)
        if (name == "all" || name == SimpleServiceName(kind)) bits |= std::uint64_t{1} << kind;
      if (comma == std::string_view::npos) break;
      rest.remove_prefix(comma + 1u);
    }
    std::fprintf(stderr, "GEKKOAOT_NATIVE_SERVICE_DISABLE_V301=1 mask=%016llx\n",
                 static_cast<unsigned long long>(bits));
    return bits;
  }();
  return mask;
}

// v301: static-direct libc tokens whose only effects are guest GPRs and plain
// guest memory (no clock, cache/FIFO publication, code, device or scheduler
// state).
bool IsChainTransparentLeaf(std::uint32_t token) {
  if ((token & kStaticDirectMask) != kStaticDirectSimpleBase) return false;
  switch (static_cast<SimpleHleKind>(token & 0xffu)) {
  case SimpleHleKind::Memcpy: case SimpleHleKind::Memmove: case SimpleHleKind::Memset:
  case SimpleHleKind::Memcmp: case SimpleHleKind::Bzero: case SimpleHleKind::Strlen:
  case SimpleHleKind::Strcmp: case SimpleHleKind::Strncmp: case SimpleHleKind::Strcpy:
  case SimpleHleKind::Strncpy: case SimpleHleKind::Strcat: case SimpleHleKind::Strchr:
  case SimpleHleKind::Strrchr: case SimpleHleKind::Memchr: case SimpleHleKind::Strstr:
  case SimpleHleKind::Strncat: case SimpleHleKind::Strnlen: case SimpleHleKind::Strcspn:
  case SimpleHleKind::Strpbrk:
    return true;
  default:
    return false;
  }
}

// v367: SDK matrix uploads (GXLoad*MtxImm) replace guest code whose only
// host-visible effect is WGPIPE bytes. Their HLE queues those bytes through
// the same per-32-byte-line epoch bumps as the guest stores, and an overflow
// IRQ taken at entry sets cpu->exception, so they need no extra boundary.
bool IsChainTransparentGxMatrix(std::uint32_t token) {
  if ((token & kStaticDirectMask) != kStaticDirectSimpleBase) return false;
  switch (static_cast<SimpleHleKind>(token & 0xffu)) {
  case SimpleHleKind::GxLoadPosMtxImm: case SimpleHleKind::GxLoadTexMtxImm:
  case SimpleHleKind::GxLoadNrmMtxImm: case SimpleHleKind::GxLoadNrmMtxImm3x3:
    return true;
  default:
    return false;
  }
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
constexpr std::uint32_t kPpcExcExternalUnwind = 0x00000040u;
constexpr std::uint32_t kExceptionVectorSpan = 0x100u;

constexpr bool IsLowExceptionVectorAddress(std::uint32_t physical) {
  // v182: this is queried before almost every normal AOT dispatch, while an
  // actual low-memory vector is exceptional.  The old implementation walked
  // fifteen ranges.  Exception vectors are 0x100-byte pages, so represent the
  // exact same set as a compile-time page mask.
  constexpr std::uint32_t kVectorPageMask = 0x0098b3feu;
  if (physical < 0x0100u || physical >= 0x1800u) return false;
  return (kVectorPageMask & (1u << (physical >> 8u))) != 0u;
}

void AcknowledgeSynchronousExceptionVector(CPUState* cpu, std::uint32_t physical) {
  if (!cpu || (physical & (kExceptionVectorSpan - 1u)) != 0u) return;

  // CPUState::exception is a host-side pending/unwind latch, not an
  // architectural PPC register. DolRecomp sets a bit when taking a
  // synchronous exception so generated code unwinds to the execution chassis.
  // Once the chassis has actually entered the corresponding low-memory vector,
  // that pending bit must be consumed. Leaving it set makes helpers such as
  // mftb immediately return after their `if (ctx->exception)` guard forever.
  // v366: synchronous guest exceptions are rare and usually end in a game's
  // own crash handler (JUTException, OSPanic); keep the first ones visible.
  if (physical == 0x0300u || physical == 0x0600u || physical == 0x0700u ||
      physical == 0x0200u) {
    static unsigned logs = 0u;
    if (logs++ < 16u)
      std::fprintf(stderr,
                   "GEKKOAOT_GUEST_EXCEPTION_V366 vector=%04x srr0=%08x srr1=%08x dar=%08x dsisr=%08x "
                   "program=%08x lr=%08x r1=%08x r3=%08x\n",
                   physical, cpu->srr0, cpu->srr1, cpu->dar, cpu->dsisr, cpu->program_exception,
                   cpu->lr, cpu->gpr[1], cpu->gpr[3]);
  }
  switch (physical) {
  case 0x0500u: cpu->exception &= ~kPpcExcExternalUnwind; break;
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
  // Private HostRuntime extension used by the patched DolRecomp tlbie helper.
  // The renderer never receives this operation.
  kCacheTlbie = 4,
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
  if (native_hw_.EXI()) {
    native_exi_host_.Enable();
    GekkoAOT::HW::EXI::NativeEXI::DeviceHooks exi_hooks{};
    exi_hooks.user = this;
    exi_hooks.select = &HostRuntime::NativeExiSelect;
    exi_hooks.present = &HostRuntime::NativeExiPresent;
    exi_hooks.immediate = &HostRuntime::NativeExiImmediate;
    exi_hooks.dma = &HostRuntime::NativeExiDma;
    exi_.SetDeviceHooks(exi_hooks);
  }
  runtime_parallel_workers_ = RuntimeEnvBool("GEKKOAOT_RUNTIME_PARALLEL", false);
  debug_fifo_overrun_ = RuntimeEnvBool("GEKKOAOT_DEBUG_FIFO_OVERRUN", false);
  fifo_irq_fence_enabled_ = RuntimeEnvBool("GEKKOAOT_FIFO_IRQ_FENCE", true);
  fast_wgpipe_enabled_ = RuntimeEnvBool("GEKKOAOT_FAST_WGPIPE", true);
  if (native_hw_.CP() && std::getenv("GEKKOAOT_FAST_WGPIPE") == nullptr)
    fast_wgpipe_enabled_ = true;
  fast_time_probe_enabled_ = RuntimeEnvBool("GEKKOAOT_FAST_TIME_PROBE", true);
  compat_diagnostics_enabled_ = RuntimeEnvBool("GEKKOAOT_COMPAT_DIAGNOSTICS", false);
  std::fprintf(stderr,
               "GEKKOAOT_RUNTIME_HOTPATHS_V172=1 wgpipe=%u time-probe=%u policy=backend-neutral-exact-fallback env=GEKKOAOT_FAST_WGPIPE,GEKKOAOT_FAST_TIME_PROBE\n",
               fast_wgpipe_enabled_ ? 1u : 0u, fast_time_probe_enabled_ ? 1u : 0u);
  std::fprintf(stderr,
               "GEKKOAOT_RUNTIME_PERF_V219=1 wgpipe-partial=1,2,4,8 paged-vmem=single-page-direct semantics=unchanged\n");
  if (native_hw_.Enabled()) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_HW_MAX_V4=1 di=vfs-event exi=host-card+sram+rtc si=deadline-autopoll+transfer-refresh vi=host-present+irq+frame-deadline ai=host-clock+pending-irq pi=host-irq-aggregate mi=host-address-space cp=aurora-direct-safe dsp=ax+jaudio+pending-irq router=page-cache poll=event-park\n");
  }
  std::fprintf(stderr,
               "GEKKOAOT_MEMORY_BOUNDS_V143=1 mem1-reported=%u mem1-backing=%u backing-end=%08x fakevmem=%08x+%u mem2=%u policy=dolphin-compatible-tail\n",
               AddressSpace::ReportedMem1Size(),
               static_cast<unsigned>(memory_.Mem1().size()),
               0x80000000u + static_cast<unsigned>(memory_.Mem1().size()),
               AddressSpace::FakeVmemBase, AddressSpace::RetailFakeVmemSize,
               static_cast<unsigned>(memory_.Mem2().size()));
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_ADDRESS_WINDOW_V279=1 enabled=%u virtual_bytes=%llu backing=shared-ram-aliases guard=mmio+unmapped translation=checked\n",
               memory_.NativeAddressBase() ? 1u : 0u,
               static_cast<unsigned long long>(memory_.NativeAddressBase() ? GuestAddressWindow::Size : 0u));
  safe_host_boundaries_enabled_ = RuntimeEnvBool("GEKKOAOT_SAFE_HOST_BOUNDARIES", true);
  compat_break_on_fault_ = RuntimeEnvBool("GEKKOAOT_COMPAT_BREAK_ON_FAULT", false);

  // v47: AOT execution itself is never throttled. The GameCube-visible clock
  // domain comes from steady_clock, while presentation has an independent cap.
  host_fps_limit_hz_ = RuntimeEnvU32("GEKKOAOT_HOST_FPS_LIMIT", 120u, 1000u);
  frame_interpolation_enabled_ = RuntimeEnvBool("GEKKOAOT_FRAME_INTERPOLATION", false);
  perf_metrics_enabled_ = RuntimeEnvBool("GEKKOAOT_PERF_METRICS", true);
  widescreen_frustum_assist_enabled_ =
      RuntimeEnvBool("GEKKOAOT_WIDESCREEN_FRUSTUM", true);
  cp_idle_breakpoint_recovery_enabled_ =
      RuntimeEnvBool("GEKKOAOT_CP_IDLE_BREAKPOINT_RECOVERY", false);
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
               "GEKKOAOT_WIDESCREEN_FRUSTUM_V177=%u mode=projection-correlated policy=gx-camera-signature+unique-ram-pair hardcode=none env=GEKKOAOT_WIDESCREEN_FRUSTUM,GEKKOAOT_WIDESCREEN_FRUSTUM_ASPECT_ADDR\n",
               widescreen_frustum_assist_enabled_ ? 1u : 0u);
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_INTERCEPT_INDEX_V10=1 policy=compile-time-pruned+sorted-exact\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_EVENT_HORIZON_V2=1 cycles=%lld max-chain=4096 barriers=mmio+hle+exceptions+host-epoch\n",
               static_cast<long long>(cpu_.cycle_budget));
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_VI_IRQ_TRACE_V81=1 stages=vi-latch+vi-ack+pi-level+pi-mask+ee+vector500 limit=bounded\n");
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_OS_READY_FASTPATH_V82=1 thread-snapshot=single-pointer active-next=batched ready=compact semantics=unchanged\n");
  std::fprintf(stderr,
               "GEKKOAOT_FIXED_EXEC_ROUTER_V178=1 identity=guest-chunk+neighborhood active-image=persistent route-cache=direct-8192-page+epoch negative-pc-cache=1024 cross-module-indirect=host-routed rel=oslink\n");
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
               "GEKKOAOT_UNIVERSAL_NATIVE_V32_SAFE=1 cpu=v28b-exact-default+directfp31-optin nativeos=direct-mem "
               "libc=mem+string gx-fifo-burst=4096 vfs=nod-file-lru audio=bulk-noheap "
#if defined(GEKKOAOT_HAVE_FFMPEG_SWSCALE)
               "vp6-rgba=libswscale\n");
#else
               "vp6-rgba=scalar-fallback\n");
#endif
  std::fprintf(stderr,
               "GEKKOAOT_DEBUG_CORPUS_NATIVE_V33=1 signatures=32 sdk-cache-range=batch32 "
               "libc=strstr+strncat+strnlen+strcspn+strpbrk vfs=host-file-lru resolver=direct-target-exact-hash\n");
  std::fprintf(stderr,
               "GEKKOAOT_SFA_SMB_UNIVERSAL_V34=1 psq=raw-f32-pair64 gx-matrix=native-sdk "
               "texture-hash=stable-rebind-only directfp=optin-safe\n");
  std::fprintf(stderr,
               "GEKKOAOT_RUNTIME_WORKERS_V36=1 policy=%s cpu=aot+nativeos+mmio+irq "
               "gx=%s dsp=cpu-ordered host-audio=async io=%s\n",
               runtime_parallel_workers_ ? "parallel" : "serial",
               runtime_parallel_workers_ ? "parser-worker+aurora-render-worker" : "inline-parser+aurora-render-worker",
               runtime_parallel_workers_ ? "async-disc" : "inline-disc");
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
               "GEKKOAOT_NATIVE_DTLB_V212=1 entries=128 ways=2 sets=64 "
               "fill=hpt-miss invalidate=tlbie-both-ways cbit=hpt-rewalk-v216 policy=real-visibility-boundary\n");
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
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_SI_V4=1 mode=deadline-autopoll host-sample=120hz "
                 "explicit-transfer=refresh sipoll-latch=retained idle-wake=si-deadline\n");
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
  QuiesceDiscIo();
  // Graphics owns asynchronous host callbacks, so quiesce it while guest
  // memory and the other host services are still alive. The explicit call in
  // native-run normally does this earlier; this is the idempotent fallback.
  ShutdownNativeGX();
  DetachDiscImage();
  if (native_low_vector_enabled_)
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_EXCEPTION_VECTOR_V294_STATS executions=%llu proofs=%llu cache=instruction-publication\n",
                 static_cast<unsigned long long>(native_low_vector_executions_),
                 static_cast<unsigned long long>(native_low_vector_proofs_));
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
  if (native_input_ready_) {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_SI_V4_STATS samples=%llu auto_polls=%llu transfer_samples=%llu\n",
                 static_cast<unsigned long long>(native_input_samples_),
                 static_cast<unsigned long long>(native_si_auto_polls_),
                 static_cast<unsigned long long>(native_si_transfer_samples_));
    GekkoAOT::Input::HostInput().Shutdown();
  }
#endif
}

void HostRuntime::WireCpuCallbacks() {
  cp_.SetReadBurstCallback(&HostRuntime::ConsumeGpuFifoBurst, this);
  cp_.SetReadBurstBatchCallback(&HostRuntime::ConsumeGpuFifoBurstBatch, this);
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
  QuiesceDiscIo();
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
  ClearDataTlb();
  data_tlb_hits_ = data_tlb_fills_ = data_tlb_invalidations_ = 0;
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
  RefreshVmemBatModes();
  InvalidateLowInstructionCache();
  timebase_cycle_remainder_ = 0u;
  decrementer_cycle_remainder_ = 0u;
  decrementer_pending_ = false;
  pe_notification_irq_active_ = false;
  timed_irq_sync_cycles_ = 0u;
  native_audio_effective_hz_ = 0u;
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
  native_vi_last_queued_frame_ = 0u;
  perf_last_hardware_cycles_ = 0u;
  perf_last_vi_boundaries_ = 0u;
  perf_last_guest_frames_ = 0u;
  perf_last_presents_ = 0u;
  perf_last_interpolated_ = 0u;
  perf_last_dropped_ = 0u;
  perf_metrics_poll_cycles_ = 0u;
  widescreen_frustum_manual_ = false;
  widescreen_frustum_ambiguous_logged_ = false;
  widescreen_frustum_selected_logged_ = false;
  widescreen_frustum_aspect_address_ = 0u;
  widescreen_frustum_fov_address_ = 0u;
  widescreen_frustum_pending_aspect_address_ = 0u;
  widescreen_frustum_pending_fov_address_ = 0u;
  widescreen_frustum_pending_confirm_frames_ = 0u;
  widescreen_frustum_scan_frames_ = 0u;
  widescreen_frustum_scan_offset_ = 0u;
  widescreen_frustum_scan_candidate_count_ = 0u;
  widescreen_frustum_scan_candidate_aspect_ = 0u;
  widescreen_frustum_scan_candidate_fov_ = 0u;
  widescreen_frustum_next_scan_frame_ = 30u;
  widescreen_frustum_last_target_aspect_ = 4.0f / 3.0f;
  widescreen_frustum_original_aspect_ = 4.0f / 3.0f;
  widescreen_frustum_observed_fov_ = 0.0f;
  widescreen_frustum_observed_aspect_ = 4.0f / 3.0f;
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
  rel_mapping_dirty_ = true;
  native_region_probe_capture_ = false;
  native_region_last_blocked_start_ = 0u;
  native_region_retry_start_ = 0u;
  native_region_secondary_owner_ = nullptr;
  secondary_dispatches_ = 0;
  fixed_exec_route_cache_ = {};
  fixed_exec_page_routes_.fill({});
  fixed_exec_route_epoch_ = 1u;
  ++fixed_exec_structure_epoch_;
  dispatch_code_probe_epoch_ = 0u;
  for (auto& [_, proof] : native_hook_code_proofs_) proof.epoch = 0u;
  native_region_probe_cache_.fill({});
  fixed_exec_page_cache_hits_ = 0u;
  fixed_exec_page_cache_fills_ = 0u;
  fixed_exec_active_descriptor_ = nullptr;
  fixed_exec_negative_pcs_.fill(0u);
  fixed_exec_route_memo_.fill({});
  fixed_exec_negative_cache_hits_ = 0;
  fixed_exec_route_switches_ = 0;
  global_indirect_dispatches_ = 0;
  icbi_route_invalidations_ = 0;
  icbi_targeted_invalidations_ = 0;
  sdk_idle_probe_pcs_.clear();
  idle_loop_ = {};
  idle_pages_.clear();
  rel_mapping_refreshes_ = 0;
  rel_address_translations_ = 0;
  rel_address_misses_ = 0;
  idle_event_waits_ = 0u;
  idle_event_requested_cycles_ = 0u;
  rel_last_canonical_ = 0;
  rel_last_runtime_ = 0;
  lc_.Reset();
  cp_.Reset();
  cp_.SetDeferredRegisterDrain(RuntimeEnvBool("GEKKOAOT_CP_DEFER_REGISTER_DRAIN", true));
  cp_poll_drain_enabled_ = RuntimeEnvBool("GEKKOAOT_CP_POLL_DRAIN", true);
  // Opt-in: THUG's GP watchdog loop parked rarely (no CPU change) and one of
  // two parked runs froze, so the default keeps CP status reads unparked.
  cp_poll_park_enabled_ = RuntimeEnvBool("GEKKOAOT_CP_POLL_PARK", false);
  cp_poll_drains_ = 0;
  si_.SetTransferEvents(RuntimeEnvBool("GEKKOAOT_NATIVE_SI_TRANSFER_EVENTS", false));
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
  dsp_.SetAuxiliaryAudioSource(&HostRuntime::MixDiscAudio, this);
  hle_calls_ = 0;
  last_mmio_read_ = 0;
  mmio_reads_ = 0;
  same_mmio_read_streak_ = 0;
  ee_pending_barrier_enabled_ = RuntimeEnvBool("GEKKOAOT_EE_PENDING_BARRIER", true);
  native_low_vector_enabled_ = RuntimeEnvBool("GEKKOAOT_NATIVE_EXCEPTION_VECTOR", false);
  native_low_vector_executions_ = native_low_vector_proofs_ = 0u;
  native_address_base_enabled_ = RuntimeEnvBool("GEKKOAOT_NATIVE_ADDRESS_BASE", true);
  native_flat_vmem_enabled_ = RuntimeEnvBool("GEKKOAOT_NATIVE_FLAT_VMEM", true);
  native_paged_view_enabled_ = RuntimeEnvBool("GEKKOAOT_NATIVE_PAGED_VIEW", true);
  native_locked_cache_enabled_ = RuntimeEnvBool("GEKKOAOT_NATIVE_LOCKED_CACHE",true);
  native_sdk_gx_packets_enabled_ = RuntimeEnvBool("GEKKOAOT_NATIVE_GX_SDK_PACKETS", true);
  native_sdk_gx_arrays_enabled_ = RuntimeEnvBool("GEKKOAOT_NATIVE_GX_ARRAY_SERVICE", true);
  native_sdk_gx_packets_ = 0u;
  std::fprintf(stderr,"GEKKOAOT_NATIVE_SDK_V306=1 gx_packets=%u normal_matrix=%u dvd_fst=%u pe_ordered=%u fallback=guest-aot+hardware\n",
               native_sdk_gx_packets_enabled_ ? 1u : 0u,
               EnvNativeGxNormalMatrix()?1u:0u,
               EnvNativeDvdFst()?1u:0u,
               RuntimeEnvBool("GEKKOAOT_GX_ORDERED_PE_IRQ",true)?1u:0u);
  native_address_publication_enabled_ = RuntimeEnvBool("GEKKOAOT_NATIVE_ADDRESS_PUBLICATION", false);
  published_native_address_base_ = native_address_base_enabled_ ? memory_.NativeAddressBase() : nullptr;
  std::fprintf(stderr, "GEKKOAOT_NATIVE_ADDRESS_PUBLICATION_V298=%u default=off owner=runtime republish=reset cpu-abi=unchanged\n",
               native_address_publication_enabled_ ? 1u : 0u);
  cpu_fifo_gather_.fill(0);
  cpu_fifo_gather_size_ = 0;
  cpu_fifo_gather_base_ = 0;
  cpu_fifo_gather_end_ = 0;
  cpu_fifo_capture_bursts_ = 0;
  cpu_fifo_capture_logged_ = false;
  gpu_fifo_gather_.fill(0);
  gpu_fifo_gather_size_ = 0;
  atomic_fifo_service_ = false;
  fifo_irq_fences_ = 0;
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
  if (module_.BindNativeHardware(NativeHardwareServices()))
    std::fprintf(stderr, "GEKKOAOT_NATIVE_HARDWARE_BIND_V243=1 scope=primary binding=available cpu-abi=unchanged\n");
  if (module_.BindNativeWgpipeTry(NativeWgpipeTryServices()))
    std::fprintf(stderr, "GEKKOAOT_NATIVE_WGPIPE_TRY_BIND_V244=1 scope=primary binding=available cpu-abi=unchanged\n");
  if (module_.BindNativeStableMmio(NativeStableMmioServices()))
    std::fprintf(stderr, "GEKKOAOT_NATIVE_STABLE_MMIO_BIND_V280=1 scope=primary devices=cp+pe+pi publication=epoch-only fallback=ordered\n");
  if (module_.BindNativeAddressWindow(NativeAddressWindowServices(native_address_publication_enabled_)))
    std::fprintf(stderr, "GEKKOAOT_NATIVE_ADDRESS_BIND_V287=1 scope=primary cpu-abi=unchanged window=%d\n", native_address_base_enabled_ && memory_.NativeAddressBase() ? 1 : 0);
  if (module_.BindNativeFlatVmem(NativeFlatVmemServices())) {
    std::fprintf(stderr, "GEKKOAOT_NATIVE_FLAT_VMEM_V304=%u base=7e000000 paging=revocable fallback=original cpu-abi=unchanged\n", native_flat_vmem_enabled_ ? 1u : 0u);
    std::fprintf(stderr, "GEKKOAOT_NATIVE_PAGED_VIEW_V322=%u source=dtlb-mirror lru=shared-mru\n",
                 native_flat_vmem_enabled_ && native_paged_view_enabled_ ? 1u : 0u);
  }
  if (module_.BindNativeLockedCache(NativeLockedCacheServices()))
    std::fprintf(stderr,"GEKKOAOT_NATIVE_LOCKED_CACHE_V308=%u scope=primary base=e0000000 bytes=262144 dma=shared fallback=original\n",native_locked_cache_enabled_?1u:0u);
  const auto* descriptor = module_.Descriptor();
  cpu_.pc = descriptor->entry_point;
  if (descriptor->on_state_loaded) descriptor->on_state_loaded(&cpu_);
  return ModuleStatus::Ok;
}

ModuleStatus HostRuntime::LoadSecondaryModule(const std::filesystem::path& path,
                                              const char* expected_game_id) {
  auto library = std::make_unique<ModuleLibrary>();
  // v367: REL side modules carry a native entry per guest instruction, so one
  // can hold 60-150 MiB of text and a title preloads every compiled variant
  // (Tales of Symphonia: 15 modules). Copying them all onto anonymous huge
  // pages made ~1.5 GiB permanently resident; keep them file-backed (demand
  // paged, shared, reclaimable). GEKKOAOT_HUGE_TEXT_SECONDARY=1 restores it.
  static const bool secondary_huge_text = RuntimeEnvBool("GEKKOAOT_HUGE_TEXT_SECONDARY", false);
  const auto status = library->Open(path, expected_game_id, secondary_huge_text);
  if (status != ModuleStatus::Ok) {
    secondary_module_last_error_ = library->LastError();
    return status;
  }
  if (memory_.PagedVmem() && !library->SupportsMemoryRestart()) {
    secondary_module_last_error_ = ModuleStatusName(ModuleStatus::MemoryRestartRequired);
    return ModuleStatus::MemoryRestartRequired;
  }
  if (library->BindNativeHardware(NativeHardwareServices()))
    std::fprintf(stderr, "GEKKOAOT_NATIVE_HARDWARE_BIND_V243=1 scope=secondary binding=available cpu-abi=unchanged\n");
  (void)library->BindNativeWgpipeTry(NativeWgpipeTryServices());
  (void)library->BindNativeStableMmio(NativeStableMmioServices());
  (void)library->BindNativeAddressWindow(NativeAddressWindowServices(native_address_publication_enabled_));
  (void)library->BindNativeFlatVmem(NativeFlatVmemServices());
  (void)library->BindNativeLockedCache(NativeLockedCacheServices());
  const auto* descriptor = library->Descriptor();
  if (descriptor->on_state_loaded) descriptor->on_state_loaded(&cpu_);
  if (descriptor->num_rel_modules == 0u && descriptor->chunk_hashes) {
    has_fixed_secondary_modules_ = true;
    // Adding a fixed executable changes the overlap candidate set. Drop all
    // epoch-local routing decisions so the next dispatch re-identifies the
    // live image with the expanded descriptor set.
    fixed_exec_route_cache_ = {};
    ++fixed_exec_route_epoch_;
    ++fixed_exec_structure_epoch_;
    ++dispatch_code_probe_generation_;
    fixed_exec_active_descriptor_ = nullptr;
    fixed_exec_negative_pcs_.fill(0u);
  }
  secondary_module_caps_[descriptor] = library->Capabilities();
  cached_secondary_caps_owner_ = nullptr;
  secondary_modules_.push_back(std::move(library));
  active_rel_sections_.clear();
  rel_mapping_dirty_ = true;
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
  // The IPL has already configured DVD audio when a retail DOL starts. Games
  // query streaming status without necessarily emitting E4 themselves.
  di_.ConfigureAudioBuffer(boot_bin[8] != 0, boot_bin[9]);
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

void HostRuntime::QuiesceDiscIo() {
  if (!di_io_inflight_) return;
  if (di_io_future_.valid()) {
    // Detach/cancel is a rare control path. Waiting here is preferable to
    // letting a worker retain nod/ifstream/VFS state past its lifetime.
    (void)di_io_future_.get();
  }
  di_io_inflight_ = false;
}

void HostRuntime::DetachDiscImage() {
  QuiesceDiscIo();
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

  if (GekkoAOT::NativeHW::DI::PreferVfs(
          native_hw_.DI(), EnvNativeVfs())) {
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
  di_.SetDeviceHooks({this, &HostRuntime::DiscCommand, &HostRuntime::DiscAudioRead});
  return true;
}

bool HostRuntime::ReadDiscImage(std::uint64_t offset, void* destination, std::uint32_t size) {
  std::lock_guard<std::mutex> io_lock(disc_io_mutex_);
  if (!destination) return false;
  if (offset > disc_image_size_ || size > disc_image_size_ - offset) return false;
  if (size == 0u) return true;

  if (GekkoAOT::NativeHW::DI::PreferVfs(
          native_hw_.DI(), EnvNativeVfs()) &&
      native_vfs_.Ready()) {
    const char* source = nullptr;
    if (native_vfs_.ReadDiscRange(offset, destination, size, &source)) {
      ++vfs_reads_;
      vfs_read_bytes_ += size;
      static unsigned vfs_logs = 0u;
      if (EnvTraceDvd() && vfs_logs++ < 256u)
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

bool HostRuntime::DiscAudioRead(void* user, std::uint64_t offset,
                                void* destination, std::uint32_t size) {
  auto* self = static_cast<HostRuntime*>(user);
  return self && self->ReadDiscImage(offset, destination, size);
}

void HostRuntime::MixDiscAudio(void* user, std::int16_t* pcm, std::size_t frames,
                               std::uint32_t output_hz) {
  auto* self = static_cast<HostRuntime*>(user);
  if (!self || !self->ai_.IsPlaying() || !self->di_.AudioStreaming()) return;
  static unsigned start_logs = 0;
  if (start_logs++ < 8u)
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_DTK_MIX_V1=1 output_hz=%u stream_hz=%u volume=%04x position=%08llx\n",
                 output_hz, self->ai_.StreamSampleRateHz(), self->ai_.Volume(),
                 static_cast<unsigned long long>(self->di_.AudioPosition()));
  self->di_.MixAudio(pcm, frames, output_hz,
                      self->ai_.StreamSampleRateHz(), self->ai_.Volume());
}

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
    self->di_async_staging_.clear();
    static const bool async_disc_io = RuntimeEnvBool("GEKKOAOT_ASYNC_DISC_IO", true);
    const bool async_host_io =
        async_disc_io && self->runtime_parallel_workers_ && transfer >= 4096u;
    if (async_host_io) {
      self->di_io_inflight_ = true;
      self->di_io_future_ = std::async(std::launch::async, [self, offset, transfer]() {
        DiscIoResult result;
        result.bytes.resize(transfer);
        result.ok = self->ReadDiscImage(offset, result.bytes.data(), transfer);
        if (!result.ok) result.bytes.clear();
        return result;
      });
    } else {
      self->di_async_staging_.resize(transfer);
      if (!self->ReadDiscImage(offset, self->di_async_staging_.data(), transfer)) {
        self->di_async_staging_.clear();
        ++self->di_read_failures_;
        response.error_code = 0x00031100u;
        return response;
      }
    }

    ++self->di_reads_;
    self->di_read_bytes_ += transfer;

    // Keep TSTART asserted until the drive-time model expires. The previous
    // bytes*2 shortcut completed a 32 KiB read in ~135 us and let streaming
    // state machines outrun retail hardware by orders of magnitude.
    self->di_async_completion_pending_ = true;
    const bool native_vfs_timing =
        GekkoAOT::NativeHW::DI::PreferVfs(
            self->native_hw_.DI(), EnvNativeVfs()) &&
        self->native_vfs_.Ready() &&
        (self->native_hw_.DI() || !EnvVfsGcTiming());
    if (native_vfs_timing) {
      // NativeVFS is a host filesystem service, not an optical-drive timing
      // model. Keep completion deferred until the next host hardware boundary
      // so DVDReadAsyncPrio cannot callback reentrantly from the initiating
      // MMIO write, but add no artificial seek/rotation/transfer delay.
      self->di_async_cycles_remaining_ = 0u;
    } else if (EnvFastDisc()) {
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
    if (EnvTraceDvd() && schedule_logs++ < 256u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_DI_PARITY_V35=1 phase=schedule seq=%llu offset=%08llx dma=%08x bytes=%u delay_cycles=%llu mode=%s host_io=%s visibility=completion\n",
                   static_cast<unsigned long long>(sequence),
                   static_cast<unsigned long long>(offset), request.dma_address, transfer,
                   static_cast<unsigned long long>(self->di_async_cycles_remaining_),
                   native_vfs_timing ? "native-vfs" :
                       (EnvFastDisc() ? "fast-optin" : "gc-drive"),
                   async_host_io ? "worker" : "inline");

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
    self->native_gx_.NotifyMemoryWrite(request.dma_address, kInquiryLength);
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
  RegisterNativeHookCodeProof(address);
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
  RegisterNativeHookCodeProof(address);
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
  // A GX copy can be a render target that VI has not selected. Presenting it
  // directly exposed gray intermediate buffers in HPCOS; NativeHW VI follows
  // only completed images selected by the game's VI registers.
  const bool requested_copy_present =
      !native_hw_.VI() && RuntimeEnvBool("GEKKOAOT_NATIVE_GX_PRESENT_ON_COPY", false);
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
  // In parallel mode Aurora parses FIFO commands on the GX worker.  Install
  // the atomic-mailbox PE policy before the worker starts so no callback can
  // race a sink transition.  Serial mode keeps the old direct event sink.
  if (runtime_parallel_workers_)
    native_gx_.SetPeEventSink(nullptr, nullptr);
  else
    native_gx_.SetPeEventSink(&HostRuntime::NativeGXPeEvent, this);
  native_gx_.SetFrameReadyFence(frame_interpolation_enabled_);
  native_gx_.ApplyGraphicsSettings(live_graphics_settings_);
  native_gx_.SetAsyncWritesEnabled(runtime_parallel_workers_);
  // Preserve any event produced during renderer initialization before the sink
  // policy was installed.
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
  static const bool trace_pe=RuntimeEnvBool("GEKKOAOT_TRACE_PE",false);
  static unsigned trace_pe_count=0;
  if(trace_pe && trace_pe_count++<128u) std::fprintf(stderr,"GEKKOAOT_TRACE_PE event=consume reg=%02x value=%06x control=%x pc=%08x lr=%08x\n",reg,value,self->pe_.Control(),self->cpu_.pc,self->cpu_.lr);
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
  ++native_input_samples_;
  if (poll_si) {
    si_.PollNow();
    ++native_si_auto_polls_;
    SyncNativeSIInterrupt();
  }
#else
  (void)poll_si;
#endif
}

bool HostRuntime::NativeExiSelect(void* user, std::uint32_t channel,
                                  std::uint8_t chip_select) {
  auto* self = static_cast<HostRuntime*>(user);
  if (self && self->native_hw_.EXI())
    return self->native_exi_host_.Select(channel, chip_select);
  return false;
}

bool HostRuntime::NativeExiPresent(void* user, std::uint32_t channel,
                                   std::uint8_t chip_select) {
  auto* self = static_cast<HostRuntime*>(user);
  return self && self->native_hw_.EXI() &&
         self->native_exi_host_.Present(channel, chip_select);
}

bool HostRuntime::NativeExiImmediate(void* user, std::uint32_t channel,
                                     std::uint8_t chip_select,
                                     std::uint8_t direction, std::uint8_t length,
                                     std::uint32_t* data) {
  auto* self = static_cast<HostRuntime*>(user);
  return self && self->native_hw_.EXI() &&
         self->native_exi_host_.Immediate(channel, chip_select, direction, length, data);
}

bool HostRuntime::NativeExiDma(void* user, std::uint32_t channel,
                               std::uint8_t chip_select, std::uint8_t direction,
                               std::uint32_t guest_address, std::uint32_t length) {
  auto* self = static_cast<HostRuntime*>(user);
  if (!self || !self->native_hw_.EXI()) return false;
  auto* bytes = self->memory_.Resolve(guest_address, length);
  if (!bytes) return false;
  const bool handled = self->native_exi_host_.Dma(channel, chip_select,
                                                  direction, bytes, length);
  if (handled && direction == 0u)
    self->native_gx_.NotifyMemoryWrite(guest_address, length);
  return handled;
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

  if (GekkoAOT::NativeHW::SI::RefreshOnTransfer(
          self->native_hw_.SI(), self->native_input_ready_)) {
    const auto now = std::chrono::steady_clock::now();
    if (self->native_si_last_transfer_sample_ == std::chrono::steady_clock::time_point{} ||
        now - self->native_si_last_transfer_sample_ >= std::chrono::milliseconds(1)) {
      self->RefreshNativeInput(false);
      ++self->native_si_transfer_samples_;
      self->native_si_last_transfer_sample_ = now;
    }
  }
  // The compatibility path consumes the already-latched SI state. Sampling SDL here
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
  if (!polling_enabled && self->native_pad_mode_[channel] != parameter2) {
    static unsigned mode_logs = 0;
    if (mode_logs++ < 16u)
      std::fprintf(stderr, "GEKKOAOT_NATIVE_SI_PAD_MODE channel=%u mode=%u previous=%u\n",
                   channel, unsigned(parameter2), unsigned(self->native_pad_mode_[channel]));
  }
  if (!polling_enabled) self->native_pad_mode_[channel] = parameter2;
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
  if (self->native_input_ready_)
    GekkoAOT::Input::HostInput().Rumble(channel, parameter1 <= 2u ? parameter1 : 0u);
#endif
}

void HostRuntime::ClearDataTlb() {
  for (auto& set : data_tlb_)
    for (auto& way : set) way = {};
  data_tlb_stamp_ = 0;
  ClearPagedView();
  auto mru = memory_.PagedViewMru();
  std::fill(mru.begin(), mru.end(), std::uint8_t{0});
}

void HostRuntime::InvalidateDataTlb(std::uint32_t address) {
  // Gekko tlbie selects a 2-way TLB set with EA[14:19] and invalidates both
  // ways.  A 64-call sweep therefore invalidates all 128 data-TLB entries.
  auto& set = data_tlb_[(address >> 12u) & 0x3fu];
  DropPagedView(set[0]);
  DropPagedView(set[1]);
  set[0].valid = false;
  set[1].valid = false;
  ++data_tlb_invalidations_;
}

// v322: generated code reads resident VM-aperture translations from this
// mirror instead of calling ExternalRead/Write for every paged access. It is
// exactly the modeled DTLB: published when an entry is filled (or hit again
// after a stale drop), removed when the entry is evicted or tlbie'd, and the
// write bit appears only once the PTE C bit has been set by the slow path.
void HostRuntime::ClearPagedView() {
  auto pages = memory_.PagedViewPagesTable();
  std::fill(pages.begin(), pages.end(), 0u);
}

void HostRuntime::DropPagedView(const DataTlbEntry& entry) {
  if (!entry.valid || !AddressSpace::IsVmem(entry.ea_page) ||
      entry.vsid != (memory_.PagedViewSr() & 0x00ffffffu))
    return;
  memory_.PagedViewPagesTable()[(entry.ea_page - AddressSpace::FakeVmemBase) >> 12u] = 0u;
}

void HostRuntime::PublishPagedView(const DataTlbEntry& entry, unsigned way) {
  if (!entry.valid || !native_flat_vmem_enabled_ || !native_paged_view_enabled_ ||
      !AddressSpace::IsVmem(entry.ea_page) ||
      (cpu_.msr & (kPpcMsrPr | 0x10u)) != 0x10u || (vmem_bat_modes_ & 2u) != 0u)
    return;
  const std::uint32_t segment = cpu_.sr[AddressSpace::FakeVmemBase >> 28u];
  if ((segment & 0x80000000u) != 0u || entry.vsid != (segment & 0x00ffffffu)) return;
  if (segment != memory_.PagedViewSr()) {
    ClearPagedView();
    memory_.SetPagedViewSr(segment);
  }
  const auto mem1 = memory_.Mem1().size();
  if (mem1 < 0x1000u || entry.physical_page > mem1 - 0x1000u) return;
  // Supervisor access (PR=0 is required by the generated-code admission).
  const bool key = (segment & 0x40000000u) != 0u;
  const unsigned pp = entry.pte_lower & 3u;
  const bool readable = !key || pp != 0u;
  const bool writable = pp == 2u || (!key && pp != 3u);
  memory_.PagedViewPagesTable()[(entry.ea_page - AddressSpace::FakeVmemBase) >> 12u] =
      readable ? (entry.physical_page | 1u |
                  ((writable && (entry.pte_lower & 0x80u) != 0u) ? 2u : 0u) | (way << 2u))
               : 0u;
}

void HostRuntime::RefreshVmemBatModes() {
  constexpr std::uint32_t aperture_variable_bits = AddressSpace::RetailFakeVmemSize - 1u;
  std::uint8_t modes = 0;
  for (unsigned index = 0; index < 4; ++index) {
    const auto upper = spr_state_[536u + index * 2u];
    const auto offset_mask = ((upper & 0x1ffcu) << 15u) | 0x1ffffu;
    const auto compared_bits = ~offset_mask & ~aperture_variable_bits;
    // Only bits fixed across the complete aligned VM aperture can exclude a
    // BAT. This stays conservative for partial overlaps and unusual BL masks.
    if (((upper ^ AddressSpace::FakeVmemBase) & compared_bits) == 0u)
      modes |= static_cast<std::uint8_t>(upper & 3u);
  }
  vmem_bat_modes_ = modes;
  // A supervisor BAT over the aperture takes precedence over the page table;
  // the generated-code DTLB mirror does not model BATs, so withdraw it.
  if ((modes & 2u) != 0u) ClearPagedView();
}

HostRuntime::DataAddressTranslation HostRuntime::TranslatePagedDataAddress(std::uint32_t address, bool write) {
  using Status = PageTranslation::Status;
  const bool user = (cpu_.msr & kPpcMsrPr) != 0u;
  const std::uint32_t segment = cpu_.sr[address >> 28u];
  if (segment & 0x80000000u) return {Status::DirectStore};
  const std::uint32_t vsid = segment & 0x00ffffffu;
  const std::uint32_t ea_page = address & 0xfffff000u;
  const std::uint32_t set_index = (address >> 12u) & 0x3fu;
  auto& set = data_tlb_[set_index];
  for (unsigned way = 0; way < 2u; ++way) {
    auto& entry = set[way];
    if (!entry.valid || entry.ea_page != ea_page || entry.vsid != vsid) continue;
    ++data_tlb_hits_;
    entry.stamp = ++data_tlb_stamp_;
    // 2-way LRU state, shared with generated-code mirror hits.
    memory_.PagedViewMru()[set_index] = static_cast<std::uint8_t>(way);
    if (AddressSpace::IsVmem(address) &&
        memory_.PagedViewPagesTable()[(ea_page - AddressSpace::FakeVmemBase) >> 12u] == 0u)
      PublishPagedView(entry, way);
    const bool key = (segment & (user ? 0x20000000u : 0x40000000u)) != 0u;
    const unsigned pp = entry.pte_lower & 3u;
    const bool readable = !key || pp != 0u;
    const bool writable = pp == 2u || (!key && pp != 3u);
    if (!readable || (write && !writable)) return {Status::Protection};

    // A read fill already set R in the backing PTE.  C is different: on a
    // first store through a resident C=0 entry, Gekko re-walks the HPT so the
    // page-table C bit becomes architecturally visible.  If software removed
    // the PTE before that store, the stale TLB entry must not silently dirty
    // the old physical frame; the access takes the normal page fault instead.
    //
    // Dolphin models this as TLBLookupResult::UpdateC followed by a page-table
    // walk.  CoD Finest Hour's VM pager depends on the same boundary when it
    // evicts allocator pages to ARAM: losing C lets a dirty page be discarded
    // and later restored with stale heap links.
    if (write && (entry.pte_lower & 0x80u) == 0u) {
      return WalkPagedDataAddress(address, true, &entry);
    }

    return {Status::Mapped, entry.physical_page | (address & 0xfffu)};
  }

  return WalkPagedDataAddress(address, write);
}

// Keep the rich PTE result and its stack temporary out of the resident DTLB
// path. Table walks and first-write C updates retain the same architectural
// effects, but ordinary cached loads/stores only return two host register words.
#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
HostRuntime::DataAddressTranslation HostRuntime::WalkPagedDataAddress(
    std::uint32_t address, bool write, DataTlbEntry* changed_entry) {
  using Status = PageTranslation::Status;
  const bool user = (cpu_.msr & kPpcMsrPr) != 0u;
  const std::uint32_t segment = cpu_.sr[address >> 28u];
  auto result = TranslatePage(memory_, address, segment, spr_state_[25u], user,
                              write ? PageAccess::Write : PageAccess::Read);
  if (changed_entry) {
    if (result.status == Status::Mapped) {
      changed_entry->pte_lower |= 0x180u;
      auto& changed_set = data_tlb_[(address >> 12u) & 0x3fu];
      PublishPagedView(*changed_entry, changed_entry == &changed_set[0] ? 0u : 1u);
    }
    return {result.status, result.physical};
  }
  if (result.status != Status::Mapped || result.pte_address == 0u)
    return {result.status, result.physical};

  const std::uint32_t vsid = segment & 0x00ffffffu;
  const std::uint32_t ea_page = address & 0xfffff000u;
  const std::uint32_t set_index = (address >> 12u) & 0x3fu;
  auto& set = data_tlb_[set_index];
  auto& mru = memory_.PagedViewMru()[set_index];
  unsigned way = !set[0].valid ? 0u : !set[1].valid ? 1u : (mru ^ 1u) & 1u;
  DataTlbEntry* victim = &set[way];
  DropPagedView(*victim);
  *victim = {true, ea_page, vsid, result.physical & 0xfffff000u,
             result.pte_address, result.pte_upper, result.pte_lower, ++data_tlb_stamp_};
  mru = static_cast<std::uint8_t>(way);
  PublishPagedView(*victim, way);
  ++data_tlb_fills_;
  return {result.status, result.physical};
}

HostRuntime::DataAddressTranslation HostRuntime::TranslateVmemAddress(std::uint32_t address, bool write) {
  using Status = PageTranslation::Status;
  if ((cpu_.msr & 0x10u) == 0u) return {Status::Mapped, address};
  const bool user = (cpu_.msr & kPpcMsrPr) != 0u;
  if ((vmem_bat_modes_ & (user ? 1u : 2u)) == 0u && AddressSpace::IsVmem(address))
    return TranslatePagedDataAddress(address, write);
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
  return TranslatePagedDataAddress(address, write);
}

bool HostRuntime::AccessPagedVmem(std::uint32_t address, std::uint64_t* value,
                                  std::uint8_t size, bool write) {
  if (!memory_.PagedVmem() || !AddressSpace::IsVmem(address)) return false;
  if (size != 1u && size != 2u && size != 4u && size != 8u) {
    SetFault(write ? FaultKind::InvalidMemoryWrite : FaultKind::InvalidMemoryRead, address, size);
    return true;
  }

  using Status = PageTranslation::Status;
  const auto translation_fault = [&](std::uint32_t ea, Status status) {
    if (status == Status::InvalidTable) {
      SetFault(write ? FaultKind::InvalidMemoryWrite : FaultKind::InvalidMemoryRead,
               ea, spr_state_[25u]);
      return;
    }
    const auto old_msr = cpu_.msr;
    last_dsi_pc_ = cpu_.pc;
    last_dsi_lr_ = cpu_.lr;
    last_dsi_msr_ = old_msr;
    // v321: with MSR[RI]=0 the SDK vector routes the DSI to
    // OSDefaultExceptionHandler ("Non-recoverable Exception 2"), which is fatal.
    if ((old_msr & 0x2u) == 0u && nonrecoverable_dsi_reports_ < 8u) {
      ++nonrecoverable_dsi_reports_;
      std::fprintf(stderr,
                   "GEKKOAOT_VM_DSI_NONRECOVERABLE_V321=1 pc=%08x lr=%08x msr=%08x ea=%08x write=%u r1=%08x\n",
                   cpu_.pc, cpu_.lr, old_msr, ea, write ? 1u : 0u, cpu_.gpr[1]);
    }
    cpu_.dar = ea;
    cpu_.dsisr = (write ? 0x02000000u : 0u) |
        (status == Status::Missing ? 0x40000000u :
         status == Status::DirectStore ? 0x04000000u : 0x08000000u);
    cpu_.srr0 = cpu_.pc;
    cpu_.srr1 = old_msr & kPpcMsrRfiMask;
    cpu_.msr = ((old_msr & ~kPpcMsrLe) |
                ((old_msr & kPpcMsrIle) ? kPpcMsrLe : 0u)) &
               ~kPpcExceptionEntryClearMask;
    cpu_.pc = (old_msr & 0x40u) ? 0xfff00300u : 0x300u;
    cpu_.exception |= kPpcExcDsi;
    NoteDsiTrace('R', last_dsi_pc_, cpu_.exception);
    BumpHostReadEpoch(&cpu_);
  };

  // v219: scalar VM traffic overwhelmingly stays inside one 4 KiB page.  The
  // old generic path built an array of up to eight host byte pointers and then
  // copied the value one byte at a time even after translation had already
  // proven the physical frame contiguous.  Keep the cross-page preflight below
  // for the rare boundary case, but make ordinary 1/2/4/8-byte accesses one
  // translation + one compact endian load/store.
  if (size <= 0x1000u - (address & 0xfffu)) {
    const auto translation = TranslateVmemAddress(address, write);
    if (translation.status != Status::Mapped) {
      translation_fault(address, translation.status);
      return true;
    }
    if (translation.physical >= memory_.Mem1().size() ||
        size > memory_.Mem1().size() - translation.physical) {
      SetFault(write ? FaultKind::InvalidMemoryWrite : FaultKind::InvalidMemoryRead,
               address, translation.physical);
      return true;
    }

    auto* p = memory_.Mem1().data() + translation.physical;
    if (write) {
      const std::uint64_t v = *value;
      switch (size) {
      case 1u:
        p[0] = static_cast<std::uint8_t>(v);
        break;
      case 2u:
        p[0] = static_cast<std::uint8_t>(v >> 8u);
        p[1] = static_cast<std::uint8_t>(v);
        break;
      case 4u:
        p[0] = static_cast<std::uint8_t>(v >> 24u);
        p[1] = static_cast<std::uint8_t>(v >> 16u);
        p[2] = static_cast<std::uint8_t>(v >> 8u);
        p[3] = static_cast<std::uint8_t>(v);
        break;
      case 8u:
        p[0] = static_cast<std::uint8_t>(v >> 56u);
        p[1] = static_cast<std::uint8_t>(v >> 48u);
        p[2] = static_cast<std::uint8_t>(v >> 40u);
        p[3] = static_cast<std::uint8_t>(v >> 32u);
        p[4] = static_cast<std::uint8_t>(v >> 24u);
        p[5] = static_cast<std::uint8_t>(v >> 16u);
        p[6] = static_cast<std::uint8_t>(v >> 8u);
        p[7] = static_cast<std::uint8_t>(v);
        break;
      }
    } else {
      switch (size) {
      case 1u:
        *value = p[0];
        break;
      case 2u:
        *value = (std::uint64_t(p[0]) << 8u) | std::uint64_t(p[1]);
        break;
      case 4u:
        *value = (std::uint64_t(p[0]) << 24u) |
                 (std::uint64_t(p[1]) << 16u) |
                 (std::uint64_t(p[2]) << 8u) |
                 std::uint64_t(p[3]);
        break;
      case 8u:
        *value = (std::uint64_t(p[0]) << 56u) |
                 (std::uint64_t(p[1]) << 48u) |
                 (std::uint64_t(p[2]) << 40u) |
                 (std::uint64_t(p[3]) << 32u) |
                 (std::uint64_t(p[4]) << 24u) |
                 (std::uint64_t(p[5]) << 16u) |
                 (std::uint64_t(p[6]) << 8u) |
                 std::uint64_t(p[7]);
        break;
      }
    }
    return true;
  }

  // A scalar that straddles a page boundary must preflight every participating
  // page before the first store. Adjacent virtual pages need not map to
  // adjacent physical frames, and a fault on page two must leave page one
  // untouched.
  std::array<std::uint8_t*, 8> bytes{};
  for (unsigned offset = 0; offset < size;) {
    const auto ea = address + offset;
    const auto translation = TranslateVmemAddress(ea, write);
    if (translation.status != Status::Mapped) {
      translation_fault(ea, translation.status);
      return true;
    }
    const unsigned count = std::min<unsigned>(size - offset, 0x1000u - (ea & 0xfffu));
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
    for (unsigned i = 0; i < size; ++i)
      *bytes[i] = std::uint8_t(*value >> ((size - 1u - i) * 8u));
  } else {
    *value = 0;
    for (unsigned i = 0; i < size; ++i) *value = (*value << 8u) | *bytes[i];
  }
  return true;
}

const GekkoAOTNativeHardware& HostRuntime::NativeHardwareServices() {
  static const GekkoAOTNativeHardware services{
      1u, sizeof(GekkoAOTNativeHardware), NativeCPRead16, NativeVIRead16,
      NativePIWrite32, NativeWgpipeWrite};
  return services;
}

const GekkoAOTNativeAddressWindow& HostRuntime::NativeAddressWindowServices(bool published) {
  static const GekkoAOTNativeAddressWindow services{1u, sizeof(GekkoAOTNativeAddressWindow), NativeAddressBase};
  static const GekkoAOTNativeAddressWindow direct{1u, sizeof(GekkoAOTNativeAddressWindow), PublishedNativeAddressBase};
  return published ? direct : services;
}
std::uint8_t* HostRuntime::NativeAddressBase(CPUState* cpu) {
  auto* self = Self(cpu);
  return self && self->native_address_base_enabled_ ? self->memory_.NativeAddressBase() : nullptr;
}
std::uint8_t* HostRuntime::PublishedNativeAddressBase(CPUState* cpu) {
  auto* self = Self(cpu);
  return self ? self->published_native_address_base_ : nullptr;
}

const GekkoAOTNativeLockedCache& HostRuntime::NativeLockedCacheServices() {
  static const GekkoAOTNativeLockedCache services{1u,sizeof(GekkoAOTNativeLockedCache),NativeLockedCacheBase};
  return services;
}
std::uint8_t* HostRuntime::NativeLockedCacheBase(CPUState* cpu) {
  auto* self=Self(cpu);
  return self && self->native_locked_cache_enabled_ ?
      static_cast<std::uint8_t*>(self->lc_.Pointer(HW::LC::NativeLC::Base,HW::LC::NativeLC::Size)):nullptr;
}

const GekkoAOTNativeFlatVmem& HostRuntime::NativeFlatVmemServices() {
  static const GekkoAOTNativeFlatVmem services{2u, sizeof(GekkoAOTNativeFlatVmem), NativeFlatVmemView};
  return services;
}
const GekkoAOTFlatVmemView* HostRuntime::NativeFlatVmemView(CPUState* cpu) {
  auto* self = Self(cpu);
  return self && self->native_flat_vmem_enabled_ ? self->memory_.FlatVmemView() : nullptr;
}

const GekkoAOTNativeStableMmio& HostRuntime::NativeStableMmioServices() {
  static const GekkoAOTNativeStableMmio services{
      1u, sizeof(GekkoAOTNativeStableMmio), NativeTryStableMmioRead};
  return services;
}

std::uint64_t HostRuntime::NativeTryStableMmioRead(CPUState* cpu, std::uint32_t address,
                                                std::uint8_t size) {
  auto* self = Self(cpu);
  const auto physical = AddressSpace::ToPhysical(address);
  // v300: only CP/PE reads observe GP progress; PI INTSR/INTMR need just the
  // already-published PE events (PeEventPending sends them to the slow path).
  const bool gp_progress = physical < 0x0c002000u;
  if (!self || !self->native_hw_.Enabled() || cpu->exception ||
      self->fault_.kind != FaultKind::None ||
      (gp_progress && self->native_gx_.WritesPending()) || self->native_gx_.PeEventPending() ||
      self->native_gx_.PeEventOverflow() || self->cp_.RegisterDrainPending()) return 0u;
  std::uint64_t value = 0u;
  bool handled = false;
  if (size == 2u && !(physical & 1u)) {
    if (self->native_hw_.CP() && physical >= 0x0c000000u && physical < 0x0c001000u) {
      handled = self->cp_.Read(address, size, &value);
      if (handled) {
        self->SyncNativeCPInterrupt();
        // v366: an unchanged CP status read in a tight loop waits for other
        // guest activity (THUG's GP watchdog thread spins on GXGetGPStatus
        // for the FIFO high watermark): park it like other device polls.
        if (physical == 0x0c000000u && self->cp_poll_park_enabled_) self->NoteMmioPoll(address, value);
      }
    } else if (physical >= 0x0c001000u && physical <= 0x0c00100eu) {
      // PE config/control/token are scalar mirrors. Bounding box and EFB
      // accesses need GPU work and cannot enter this state-free service.
      handled = self->pe_.Read(address, size, &value);
      if (handled) self->SyncNativePEInterrupts();
    }
  } else if (size == 4u && self->native_hw_.PI() &&
             (physical == 0x0c003000u || physical == 0x0c003004u)) {
    handled = self->pi_.Read(address, size, &value);
  }
  if (!handled) return 0u;
  self->CountHostEffect();
  self->RecordMmioRead(address);
  BumpHostReadEpoch(cpu);
  return (std::uint64_t{1} << 63u) | value;
}

std::uint64_t HostRuntime::NativeCPRead16(CPUState* cpu, std::uint32_t address,
                                         std::uint8_t size) {
  auto* self = Self(cpu);
  if (self) self->CountHostEffect();
  if (!self || size != 2u || (address & 1u) != 0u || !self->cp_.Handles(address))
    return ExternalRead(cpu, address, size);
  // Same CP/PE visibility boundary, event epoch, telemetry and IRQ as the bus.
  (void)self->native_gx_.FlushWrites(GX::HostBridge::SyncReason::MmioRead);
  self->SyncNativeGXInterrupts();
  BumpHostReadEpoch(cpu);
  self->RecordMmioRead(address);
  std::uint64_t value = 0u;
  if (self->cp_.Read(address, size, &value)) {
    self->SyncNativeCPInterrupt();
    return value;
  }
  self->SetFault(FaultKind::MmioRead, address);
  return 0u;
}

std::uint64_t HostRuntime::NativeVIRead16(CPUState* cpu, std::uint32_t address,
                                         std::uint8_t size) {
  auto* self = Self(cpu);
  if (self) self->CountHostEffect();
  const auto offset = AddressSpace::ToPhysical(address) - HW::VI::NativeVI::BasePhysical;
  if (!self || size != 2u || (offset & 1u) != 0u || offset > 0x74u)
    return ExternalRead(cpu, address, size);
  BumpHostReadEpoch(cpu);
  self->ServiceTimedMmioPoll(address);
  self->RecordMmioRead(address);
  std::uint64_t value = 0u;
  if (self->vi_.Read(address, size, &value)) return value;
  self->SetFault(FaultKind::MmioRead, address);
  return 0u;
}

void HostRuntime::NativePIWrite32(CPUState* cpu, std::uint32_t address,
                                  std::uint64_t value, std::uint8_t size) {
  auto* self = Self(cpu);
  if (self) self->CountHostEffect();
  if (!self || size != 4u || (address & 3u) != 0u || !self->pi_.Handles(address)) {
    ExternalWrite(cpu, address, value, size);
    return;
  }
  (void)self->native_gx_.FlushWrites(GX::HostBridge::SyncReason::MmioWrite);
  self->SyncNativeGXInterrupts();
  BumpHostWriteEpoch(cpu);
  if (self->pi_.Write(address, value, size)) return;
  self->SetFault(FaultKind::MmioWrite, address, static_cast<std::uint32_t>(value));
}

void HostRuntime::ServiceTimedMmioPoll(std::uint32_t address,bool counter) {
  const auto timed_mmio_physical = AddressSpace::ToPhysical(address);
  // Do not hit the host clock on every load.  Tight guest poll loops execute
  // this branch extremely often, so 1/64 reads is still far above any
  // GameCube device cadence while keeping the fix cheap.
  static std::uint32_t timed_mmio_poll_divider = 0u;
  // Native performance-counter reads are also time-visible boundaries. A
  // tiny elapsed-time wait can stay entirely inside one AOT function; leaving
  // its counter frozen until that function exits needlessly burns the core.
  const bool sample=counter ? (++native_counter_sample_divider_&7u)==0u :
                             (++timed_mmio_poll_divider&63u)==0u;
  if (sample) {
    constexpr std::uint64_t kTimedPollSampleCredit =
        (GekkoAOT::HW::VI::NativeVI::CpuClockHz / 16000u) + 1u;
    const std::uint64_t timed_cycles =
        TakeRealtimeHardwareCycles(kTimedPollSampleCredit, true);

    if (timed_cycles != 0u) {
      perf_hardware_cycles_total_ += timed_cycles;

      // Keep CPU timers and asynchronous DVD completion moving even if the
      // generated function never reaches a normal HostRuntime boundary.
      ServiceDiscCompletion(timed_cycles);
      AdvanceCpuTimers(timed_cycles);

      si_.AdvanceCycles(timed_cycles);
      SyncNativeSIInterrupt();

      vi_.AdvanceCycles(timed_cycles);
      if (vi_.ConsumeInterruptUpdate())
        SyncNativeVIInterrupt();

      // NativeSI has no per-cycle advance method.  Match the runtime's
      // existing ~120 Hz host-input cadence and publish a fresh SI sample.
      static std::uint64_t timed_si_phase = 0u;
      timed_si_phase += timed_cycles;
      constexpr std::uint64_t kTimedSiPollCycles =
          GekkoAOT::HW::VI::NativeVI::CpuClockHz / 120u;
      if (timed_si_phase >= kTimedSiPollCycles) {
        timed_si_phase %= kTimedSiPollCycles;
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
        if (native_input_ready_)
          RefreshNativeInput(true);
        else {
          si_.PollNow();
          SyncNativeSIInterrupt();
        }
#else
        si_.PollNow();
        SyncNativeSIInterrupt();
#endif
      }

      // Nemo's hot loop polls AI from inside one AOT wrapper.  Advancing
      // these two domains here lets the sample counter / IRQ state change
      // without waiting for that wrapper to return.
      dsp_.AdvanceCycles(timed_cycles);
      ai_.AdvanceCycles(timed_cycles);
      SyncNativeDSPInterrupt();
      SyncNativeAIInterrupt();

      static unsigned timed_mmio_logs = 0u;
      if (!counter && timed_mmio_logs++ < 64u)
        std::fprintf(stderr,
                     "GEKKOAOT_TIMED_MMIO_POLL_V203=1 ea=%08x physical=%08x cycles=%llu pc=%08x lr=%08x action=advance-realtime-devices-inside-aot-poll\n",
                     address, timed_mmio_physical,
                     static_cast<unsigned long long>(timed_cycles),
                     cpu_.pc, cpu_.lr);
      if(counter && runtime_profile_enabled_) {
        static unsigned counter_samples=0;
        if(counter_samples++<16u)
          std::fprintf(stderr,"GEKKOAOT_NATIVE_COUNTER_CLOCK_V318=1 pc=%08x elapsed_cycles=%llu source=host-monotonic within=aot\n",
            address,static_cast<unsigned long long>(timed_cycles));
      }
    }
  }
}

void HostRuntime::RecordMmioRead(std::uint32_t address) {
  ++mmio_reads_;
  if (address == last_mmio_read_)
    ++same_mmio_read_streak_;
  else {
    last_mmio_read_ = address;
    same_mmio_read_streak_ = 1;
  }
}

std::uint64_t HostRuntime::ExternalRead(CPUState* cpu, std::uint32_t address, std::uint8_t size) {
  if (auto* self = Self(cpu)) {
    self->CountHostEffect();
    if (self->runtime_profile_enabled_) {
      ++self->external_reads_[ExternalClass(address)];
      self->NoteExternalOther(address, false);
    }
    std::uint64_t value = 0;
    const std::uint32_t physical = AddressSpace::ToPhysical(address);
    const bool flipper_mmio = physical >= 0x0c000000u && physical < 0x0c007000u;
    // v159: ExternalRead is overwhelmingly Flipper MMIO in retail hot loops.
    // Once the physical page is in the fixed hardware aperture it cannot alias
    // MEM1/MEM2, paged VMEM or locked cache, so skip those generic probes.
    if (!flipper_mmio) {
      if (self->AccessPagedVmem(address, &value, size, false)) return value;

      // DolRecomp resolves the normal cached/uncached MEM1 aliases directly,
      // but low physical addresses used by Nintendo's exception/OS code fall
      // through to the external bus. Treat any address that the standalone
      // AddressSpace can actually resolve as RAM before trying hardware MMIO.
      if (ReadAddressSpaceValue(self->memory_, address, size, &value)) return value;
    }

    // v161 WGPIPE host batching may defer renderer parsing for a handful of
    // completed gather lines. Guest reads of CP/PE are ordering points: a
    // token, draw-done or FIFO status produced by those commands must be
    // visible before the load completes. VI/AI/etc do not depend on renderer
    // command consumption and therefore do not force a flush.
    //
    // v300: PI reads are not GP progress observations. INTSR/INTMR show the
    // interrupts already raised (PE events published by the GX worker are
    // folded in by SyncNativeGXInterrupts) and the CPU-FIFO pointers are host
    // state. The OS external-interrupt dispatcher reads INTSR on every VI/AI/
    // DSP/SI interrupt; draining the whole GX queue there stalled the guest
    // for ~1 ms a few hundred times per second.
    if (flipper_mmio) {
      const auto page = physical & 0xfffff000u;
      if (page == HW::CP::NativeCP::BasePhysical ||
          page == HW::PE::NativePE::BasePhysical) {
        (void)self->native_gx_.FlushWrites(GX::HostBridge::SyncReason::MmioRead);
        self->SyncNativeGXInterrupts();
      } else if (page == HW::PI::NativePI::BasePhysical) {
        self->SyncNativeGXInterrupts();
      }
    }

    // Any access that leaves ordinary MEM1/MEM2 is a timing-visible host
    // boundary.  The in-module superchain checks this epoch after the current
    // native wrapper returns and yields to HostRuntime before chaining again.
    BumpHostReadEpoch(cpu);

    // v203: a compiled PPC function can contain an entire MMIO polling loop.
    // In that case the wrapper never returns to AdvanceRuntimeCycles(), so the
    // realtime device the guest is waiting for never advances.  Nemo hits this
    // on AI sample/status polling; Rayman 3 can sit on SI status polling.
    //
    // Sample host time from inside time-driven MMIO loops and advance only the
    // standalone devices whose state is allowed to change with elapsed time.
    // TakeRealtimeHardwareCycles() remains the single owner of the host clock,
    // so the next normal runtime boundary observes only the remaining delta
    // (no double advancement).
    const std::uint32_t timed_mmio_physical = AddressSpace::ToPhysical(address);
    const bool timed_mmio_poll =
        // VI beam / retrace / interrupt state.
        (timed_mmio_physical >= 0x0c002000u &&
         timed_mmio_physical <  0x0c003000u) ||
        // DSP/ARAM/audio-DMA state.
        (timed_mmio_physical >= 0x0c005000u &&
         timed_mmio_physical <  0x0c005100u) ||
        // DI deferred command completion.
        (timed_mmio_physical >= 0x0c006000u &&
         timed_mmio_physical <  0x0c006028u) ||
        // SI controller/status polling.
        (timed_mmio_physical >= 0x0c006400u &&
         timed_mmio_physical <  0x0c006500u) ||
        // AI control/sample counter/interrupt timing.
        (timed_mmio_physical >= 0x0c006c00u &&
         timed_mmio_physical <  0x0c006c10u);

    if (timed_mmio_poll) self->ServiceTimedMmioPoll(address);

    // Keep low-cost polling telemetry for black-screen diagnosis.  A tight
    // SDK hardware wait is valid guest code, so never terminate it here; the
    // final run result merely reports which MMIO register dominated the tail.
    self->RecordMmioRead(address);

    if (!flipper_mmio && self->lc_.Read(address, size, &value)) return value;

    // v159: Flipper MMIO blocks are non-overlapping fixed physical windows.
    // The old generic path asked every device whether it owned each address,
    // which turned hot CP/VI/PI polling into 6-10 range checks plus calls per
    // guest load. Decode the 4 KiB hardware page once and only enter the owner.
    switch (physical & 0xfffff000u) {
    case HW::CP::NativeCP::BasePhysical:
      // v366: a released CP breakpoint defers the GP drain to the next host
      // boundary (the release usually happens in the CP interrupt handler).
      // A thread that then polls CP state (EE on, same register again) waits
      // for that drain: the hardware GP keeps reading during the wait. THUG
      // spun ~12M CP status reads/s until the AOT cycle guard reached a host
      // boundary. Serve the pending drain to such a poll.
      if (self->cp_poll_drain_enabled_ && self->cp_.RegisterDrainPending() &&
          (cpu->msr & kPpcMsrEe) != 0u && self->same_mmio_read_streak_ > 1u) {
        self->cp_.ServiceRegisterDrain();
        self->SyncNativeCPInterrupt();
        ++self->cp_poll_drains_;
        if (self->cp_poll_drains_ <= 4u)
          std::fprintf(stderr, "GEKKOAOT_CP_POLL_DRAIN_V366=1 ea=%08x pc=%08x drains=%llu action=serve-deferred-gp-drain\n",
                       address, cpu->pc, static_cast<unsigned long long>(self->cp_poll_drains_));
      }
      if (self->cp_.Read(address, size, &value)) {
        self->SyncNativeCPInterrupt();
        if (physical == 0x0c000000u && self->cp_poll_park_enabled_) self->NoteMmioPoll(address, value);
        // Diagnostic: GEKKOAOT_CP_POLL_TRACE=1 samples hot CP register polls.
        static const bool cp_trace = RuntimeEnvBool("GEKKOAOT_CP_POLL_TRACE", false);
        if (cp_trace) {
          static std::uint64_t cp_reads = 0; static unsigned window = 0, windows = 0;
          if ((++cp_reads & 0xfffffu) == 0u && windows < 24u) { window = 12u; ++windows; }
          if (window) {
            --window;
            std::fprintf(stderr, "GEKKOAOT_CP_POLL_TRACE ea=%08x value=%llx pc=%08x lr=%08x msr=%x "
                         "rw=%08x rp=%08x wp=%08x ctl=%04x\n", address,
                         static_cast<unsigned long long>(value), cpu->pc, cpu->lr, cpu->msr,
                         self->cp_.FifoReadWriteDistance(), self->cp_.FifoReadPointer(),
                         self->cp_.FifoWritePointer(), self->cp_.Control());
          }
        }
        return value;
      }
      break;
    case HW::PE::NativePE::BasePhysical:
      // SDK IRQ handlers consume a single notification before acknowledging
      // it. Repeated token polling with EE disabled instead observes the
      // renderer's latest register, without requiring interrupt delivery.
      if (physical == 0x0c00100eu && self->same_mmio_read_streak_ > 1u &&
          !(cpu->msr & kPpcMsrEe) && !self->pe_notification_irq_active_)
        self->SyncNativeGXInterrupts(true);
      if (physical >= 0x0c001010u && physical <= 0x0c001016u &&
          ((size == 2u && (physical & 1u) == 0u) ||
           (size == 4u && (physical & 3u) == 0u))) {
        std::array<std::uint16_t, 4> bounds;
        if (self->native_gx_.ReadBoundingBox(&bounds)) self->pe_.SetBoundingBox(bounds);
      }
      if (self->pe_.Read(address, size, &value)) {
        static const bool trace_pe=RuntimeEnvBool("GEKKOAOT_TRACE_PE",false);
        static unsigned reads=0;
        if(trace_pe&&physical==0x0c00100eu&&reads++<64u)
          std::fprintf(stderr,"GEKKOAOT_TRACE_PE event=read-token value=%llx pc=%08x lr=%08x\n",static_cast<unsigned long long>(value),cpu->pc,cpu->lr);
        self->SyncNativePEInterrupts();
        return value;
      }
      break;
    case HW::VI::NativeVI::BasePhysical:
      if (self->vi_.Read(address, size, &value)) return value;
      break;
    case HW::PI::NativePI::BasePhysical:
      if (self->pi_.Read(address, size, &value)) return value;
      break;
    case HW::MI::NativeMI::BasePhysical:
      if (self->mi_.Read(address, size, &value)) return value;
      break;
    case DSP::NativeDSP::BasePhysical:
      if (self->dsp_.Read(address, size, &value)) {
        self->SyncNativeDSPInterrupt();
        self->NoteMmioPoll(address, value);
        return value;
      }
      break;
    case 0x0c006000u:
      // DI/SI/EXI/AI share the final Flipper MMIO page but occupy disjoint
      // sub-windows. Use the high byte within the page as a second-level key.
      switch (physical & 0x00000f00u) {
      case 0x000u:
        if (self->di_.Read(address, size, &value)) {
          self->SyncNativeDIInterrupt();
          self->NoteMmioPoll(address, value);
          return value;
        }
        break;
      case 0x400u:
        if (self->si_.Read(address, size, &value)) {
          self->SyncNativeSIInterrupt();
          self->NoteMmioPoll(address, value);
          return value;
        }
        break;
      case 0x800u:
        if (self->exi_.ReadRegister(address, size, &value)) {
          self->SyncNativeEXIInterrupt();
          self->NoteMmioPoll(address, value);
          return value;
        }
        break;
      case 0xc00u:
        if (self->ai_.Read(address, size, &value)) {
          self->SyncNativeAIInterrupt();
          self->NoteMmioPoll(address, value);
          return value;
        }
        break;
      default:
        break;
      }
      break;
    default:
      break;
    }
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

bool HostRuntime::ConsumeGpuFifoBurstBatch(void* user, std::uint32_t address,
                                               std::uint32_t burst_count) {
  auto* self = static_cast<HostRuntime*>(user);
  if (!self || burst_count == 0u) return false;
  if (!self->native_gx_.Ready()) return true;

  // v161: NativeCP guarantees that a batch never crosses the guest FIFO wrap
  // point and is never used while a CP breakpoint is active.  That makes the
  // corresponding MEM1 span contiguous and lets the renderer consume up to
  // 4096 bytes with one DSO transition / parser entry instead of one per line.
  constexpr std::uint32_t kBurstBytes = 32u;
  constexpr std::uint32_t kMaxBatchBursts = 128u;
  const std::uint32_t count = std::min(burst_count, kMaxBatchBursts);
  const std::uint32_t bytes_count = count * kBurstBytes;
  const auto* bytes = self->memory_.Resolve(address, bytes_count);
  if (!bytes || !self->native_gx_.WriteBurst(bytes, bytes_count, self->cpu_.pc)) {
    self->SetFault(FaultKind::MmioWrite, address, bytes_count);
    return false;
  }
  return true;
}

bool HostRuntime::QueueGpuFifoGather(std::uint64_t value, std::uint8_t size,
                                     std::uint32_t guest_pc) {
  if (size != 1u && size != 2u && size != 4u && size != 8u) return false;
  if (!native_gx_.Ready()) return false;
  if (gpu_fifo_gather_size_ == 0u) gpu_fifo_first_pc_ = guest_pc;

  // v172: backend-neutral WGPIPE append fast path. Only consume a store when it
  // is guaranteed to remain strictly inside the current 32-byte gather line.
  // The store that completes a line (or could straddle it) always falls through
  // to the original path below, preserving PI/CP visibility, breakpoints, burst
  // accounting, renderer submission and host-write epoch semantics exactly.
  if (fast_wgpipe_enabled_) {
    const std::size_t room = gpu_fifo_gather_.size() - gpu_fifo_gather_size_;
    if (size == 4u && room > 4u) {
      auto* out = gpu_fifo_gather_.data() + gpu_fifo_gather_size_;
      const auto word = static_cast<std::uint32_t>(value);
      out[0] = static_cast<std::uint8_t>(word >> 24u);
      out[1] = static_cast<std::uint8_t>(word >> 16u);
      out[2] = static_cast<std::uint8_t>(word >> 8u);
      out[3] = static_cast<std::uint8_t>(word);
      gpu_fifo_gather_size_ += 4u;
      return true;
    }
    if (size == 8u && room > 8u) {
      auto* out = gpu_fifo_gather_.data() + gpu_fifo_gather_size_;
      out[0] = static_cast<std::uint8_t>(value >> 56u);
      out[1] = static_cast<std::uint8_t>(value >> 48u);
      out[2] = static_cast<std::uint8_t>(value >> 40u);
      out[3] = static_cast<std::uint8_t>(value >> 32u);
      out[4] = static_cast<std::uint8_t>(value >> 24u);
      out[5] = static_cast<std::uint8_t>(value >> 16u);
      out[6] = static_cast<std::uint8_t>(value >> 8u);
      out[7] = static_cast<std::uint8_t>(value);
      gpu_fifo_gather_size_ += 8u;
      return true;
    }
  }

  // Materialize the big-endian store once, then copy contiguous spans into the
  // 32-byte gather line. This removes the branch/shift/store loop from the
  // common 32/64-bit WGPIPE path while preserving line-straddling stores.
  std::uint8_t ordered[8];
  // v159: 32-bit WGPIPE stores dominate retail GX traffic. Avoid the generic
  // shift loop on that path; the compiler can fold these fixed shifts into one
  // register sequence and the following memcpy becomes a single 32-bit store.
  if (size == 4u) {
    const auto word = static_cast<std::uint32_t>(value);
    ordered[0] = static_cast<std::uint8_t>(word >> 24u);
    ordered[1] = static_cast<std::uint8_t>(word >> 16u);
    ordered[2] = static_cast<std::uint8_t>(word >> 8u);
    ordered[3] = static_cast<std::uint8_t>(word);
  } else if (size == 8u) {
    ordered[0] = static_cast<std::uint8_t>(value >> 56u);
    ordered[1] = static_cast<std::uint8_t>(value >> 48u);
    ordered[2] = static_cast<std::uint8_t>(value >> 40u);
    ordered[3] = static_cast<std::uint8_t>(value >> 32u);
    ordered[4] = static_cast<std::uint8_t>(value >> 24u);
    ordered[5] = static_cast<std::uint8_t>(value >> 16u);
    ordered[6] = static_cast<std::uint8_t>(value >> 8u);
    ordered[7] = static_cast<std::uint8_t>(value);
  } else if (size == 2u) {
    ordered[0] = static_cast<std::uint8_t>(value >> 8u);
    ordered[1] = static_cast<std::uint8_t>(value);
  } else {
    ordered[0] = static_cast<std::uint8_t>(value);
  }

  return QueueGpuFifoBytes(ordered,size,guest_pc);
}

bool HostRuntime::QueueGpuFifoBytes(const std::uint8_t* ordered,std::size_t size,
                                    std::uint32_t guest_pc) {
  if (!ordered || !size || !native_gx_.Ready()) return false;
  if (gpu_fifo_gather_size_ == 0u) gpu_fifo_first_pc_ = guest_pc;
  std::size_t consumed = 0u;
  while (consumed < size) {
    const std::size_t room = gpu_fifo_gather_.size() - gpu_fifo_gather_size_;
    const std::size_t chunk = std::min<std::size_t>(room, size - consumed);
    std::memcpy(gpu_fifo_gather_.data() + gpu_fifo_gather_size_, ordered + consumed, chunk);
    gpu_fifo_gather_size_ += static_cast<std::uint32_t>(chunk);
    consumed += chunk;
    if (gpu_fifo_gather_size_ != gpu_fifo_gather_.size()) continue;

    if (pi_.FifoBase() != 0u || pi_.FifoEnd() != 0u) {
      const auto destination = pi_.FifoWritePointer();
      if (cp_.GPLinkEnabled() && cp_.FifoEnd() >= cp_.FifoBase() &&
          cp_.FifoReadWriteDistance() >= cp_.FifoEnd() - cp_.FifoBase() + 32u &&
          debug_fifo_overrun_) {
        std::fprintf(stderr,
                     "GEKKOAOT_FIFO_OVERRUN pc=%08x msr=%08x read=%08x write=%08x "
                     "distance=%08x control=%04x status=%04x pi_cause=%08x pi_mask=%08x "
                     "pe_token=%04x action=stop-before-overwrite\n",
                     guest_pc, cpu_.msr, cp_.FifoReadPointer(), destination,
                     cp_.FifoReadWriteDistance(), unsigned(cp_.Control()), unsigned(cp_.Status()),
                     pi_.InterruptCauseValue(), pi_.InterruptMaskValue(), unsigned(pe_.Token()));
        gpu_fifo_gather_size_ = 0u;
        SetFault(FaultKind::MmioWrite, destination, 32u);
        return true;
      }
      auto* bytes = memory_.Resolve(destination, gpu_fifo_gather_.size());
      if (!bytes) {
        gpu_fifo_gather_size_ = 0u;
        SetFault(FaultKind::MmioWrite, destination, 32u);
        return true;
      }
      const bool direct_cp = GekkoAOT::NativeHW::CP::CanDirectSubmit(
          native_hw_.CP(), native_gx_.Ready(), cp_.GPLinkEnabled(), cp_.GPReadEnabled(),
          cp_.BreakpointActiveForHost() || cp_.RegisterDrainPending(), cp_.FifoReadWriteDistance(),
          cp_.FifoReadPointer() == cp_.FifoWritePointer());

      // Keep the guest FIFO mirror exact even on the direct path. That makes
      // GX diagnostics/display-list tools and unusual guest reads observe the
      // same RAM bytes while avoiding the normal write->resolve->read callback
      // round trip into Aurora for the overwhelmingly common empty-FIFO case.
      std::memcpy(bytes, gpu_fifo_gather_.data(), gpu_fifo_gather_.size());
      pi_.AdvanceCpuFifoWritePointer();

      if (direct_cp) {
        if (!native_gx_.WriteBurst(gpu_fifo_gather_.data(), 32u, gpu_fifo_first_pc_)) {
          gpu_fifo_gather_size_ = 0u;
          SetFault(FaultKind::MmioWrite, destination, 32u);
          return true;
        }
        if (!cp_.NotifyDirectGatherWrite(32u)) {
          // State changed between policy evaluation and accounting. This is a
          // single-threaded guest boundary, but fail closed rather than silently
          // desynchronizing CP. The exact path handles the next gather line.
          SetFault(FaultKind::MmioWrite, destination, 32u);
          gpu_fifo_gather_size_ = 0u;
          return true;
        }
        SyncNativeCPInterrupt();
        static bool direct_cp_logged = false;
        if (!direct_cp_logged) {
          direct_cp_logged = true;
          std::fprintf(stderr,
                       "GEKKOAOT_NATIVE_HW_CP_DIRECT_V2=1 mode=wgpipe->aurora mirror=guest-fifo fallback=backlog+breakpoint\n");
        }
      } else if (cp_.NotifyGatherWrite(32u)) {
        SyncNativeCPInterrupt();
        if (cp_.BreakpointActiveForHost()) (void)native_gx_.FlushWrites();
      }
    } else {
      if (!native_gx_.WriteBurst(gpu_fifo_gather_.data(), 32u, gpu_fifo_first_pc_)) {
        gpu_fifo_gather_size_ = 0u;
        SetFault(FaultKind::MmioWrite, 0x0c008000u, 32u);
        return true;
      }
    }
    ++gpu_fifo_bursts_;
    gpu_fifo_gather_size_ = 0u;
    gpu_fifo_first_pc_ = guest_pc;
    if (!gpu_fifo_burst_logged_) {
      gpu_fifo_burst_logged_ = true;
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_GX_GPU_WG_BURST_V14=1 burst=32 abi=bulk mode=chunk-copy\n");
    }
  }
  return true;
}

#if defined(_MSC_VER)
__forceinline
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline)) inline
#endif
bool HostRuntime::TryGatherPartial(std::uint64_t value, std::uint8_t size, std::uint32_t pc) {
  if (!fast_wgpipe_enabled_ || !native_gx_.Ready() || cpu_fifo_gather_size_ != 0u ||
      !CpuFifoTargetsGpu() ||
      (size != 1u && size != 2u && size != 4u && size != 8u) ||
      gpu_fifo_gather_size_ + size >= gpu_fifo_gather_.size() ||
      (size == 4u && (value == 0xc274d554u ||
                     (pc == 0x800228c0u && value == 0x80000000u))))
    return false;
  if (gpu_fifo_gather_size_ == 0u) gpu_fifo_first_pc_ = pc;
  auto* out = gpu_fifo_gather_.data() + gpu_fifo_gather_size_;
  switch (size) {
  case 1u:
    out[0] = static_cast<std::uint8_t>(value);
    break;
  case 2u:
    out[0] = static_cast<std::uint8_t>(value >> 8u);
    out[1] = static_cast<std::uint8_t>(value);
    break;
  case 4u:
    out[0] = static_cast<std::uint8_t>(value >> 24u);
    out[1] = static_cast<std::uint8_t>(value >> 16u);
    out[2] = static_cast<std::uint8_t>(value >> 8u);
    out[3] = static_cast<std::uint8_t>(value);
    break;
  case 8u:
    out[0] = static_cast<std::uint8_t>(value >> 56u);
    out[1] = static_cast<std::uint8_t>(value >> 48u);
    out[2] = static_cast<std::uint8_t>(value >> 40u);
    out[3] = static_cast<std::uint8_t>(value >> 32u);
    out[4] = static_cast<std::uint8_t>(value >> 24u);
    out[5] = static_cast<std::uint8_t>(value >> 16u);
    out[6] = static_cast<std::uint8_t>(value >> 8u);
    out[7] = static_cast<std::uint8_t>(value);
    break;
  }
  gpu_fifo_gather_size_ += size;
  return true;
}

bool HostRuntime::QueueSdkFifoPacket(const std::uint8_t* bytes,std::size_t size) {
  if (!native_gx_.Ready() || !bytes || size==0u || size>53u) return false;
  ++native_sdk_gx_packets_;
  const auto bump_lines=[&](std::uint64_t before,std::uint64_t after) {
    while (before++<after) BumpHostWriteEpoch(&cpu_);
  };
  if (!CpuFifoTargetsGpu()) {
    for (std::size_t off=0;off<size;) {
      const auto remaining=size-off;
      const std::uint8_t chunk=remaining>=8u?8u:remaining>=4u?4u:remaining>=2u?2u:1u;
      std::uint64_t word=0u;
      for (unsigned i=0;i<chunk;++i) word=(word<<8u)|bytes[off+i];
      const auto before=cpu_fifo_capture_bursts_;
      if (!CaptureWriteGatherToCpuFifo(word,chunk)) return false;
      bump_lines(before,cpu_fifo_capture_bursts_);
      if (fault_.kind!=FaultKind::None) return false;
      off+=chunk;
    }
    return true;
  }
  if (cpu_fifo_gather_size_) {
    if (gpu_fifo_gather_size_) {
      SetFault(FaultKind::MmioWrite,0xcc008000u,cpu_fifo_gather_size_);return false;
    }
    std::copy_n(cpu_fifo_gather_.begin(),cpu_fifo_gather_size_,gpu_fifo_gather_.begin());
    gpu_fifo_gather_size_=cpu_fifo_gather_size_;gpu_fifo_first_pc_=cpu_.pc;
    cpu_fifo_gather_size_=0u;
  }
  const auto before=gpu_fifo_bursts_;
  const auto accepted=QueueGpuFifoBytes(bytes,size,cpu_.pc);
  bump_lines(before,gpu_fifo_bursts_);
  return accepted && fault_.kind==FaultKind::None;
}

const GekkoAOTNativeWgpipeTry& HostRuntime::NativeWgpipeTryServices() {
  static const GekkoAOTNativeWgpipeTry services{
      1u, sizeof(GekkoAOTNativeWgpipeTry), NativeTryWgpipeWrite};
  return services;
}

std::uint32_t HostRuntime::NativeTryWgpipeWrite(CPUState* cpu, std::uint32_t address,
    std::uint64_t value, std::uint8_t size, std::uint32_t pc) {
  auto* self = Self(cpu);
  if (self) self->CountHostEffect();
  if (!self || cpu->exception != 0u ||
      !GekkoAOT::GX::HostBridge::IsWriteGatherPipe(address)) return 0u;
  // Only owner and pending-exception fields are read; there is no state publication.
  // In particular the current instruction's PC is an explicit argument.
  return self->TryGatherPartial(value, size, pc) ? 1u : 0u;
}

template<bool ProvenWgpipe>
void HostRuntime::WriteGatherOrExternal(CPUState* cpu, std::uint32_t address, std::uint64_t value,
                                      std::uint8_t size) {
  auto* self = Self(cpu);
  if (self) self->CountHostEffect();
  if (self && (ProvenWgpipe || GekkoAOT::GX::HostBridge::IsWriteGatherPipe(address)) &&
      self->TryGatherPartial(value, size, cpu->pc)) return;
  ExternalWriteSlow(cpu, address, value, size);
}

void HostRuntime::ExternalWrite(CPUState* cpu, std::uint32_t address, std::uint64_t value,
                                std::uint8_t size) {
  WriteGatherOrExternal<false>(cpu, address, value, size);
}

void HostRuntime::NativeWgpipeWrite(CPUState* cpu, std::uint32_t address,
                                    std::uint64_t value, std::uint8_t size) {
  // The LLVM proof admits only scalar stores in the fixed WG aperture.
  // Queue completion, RAM display-list capture and IRQ restart use the exact
  // slow path. No paired-store atomicity or quantisation is assumed here.
  WriteGatherOrExternal<true>(cpu, address, value, size);
}

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void HostRuntime::ExternalWriteSlow(CPUState* cpu, std::uint32_t address, std::uint64_t value,
                                std::uint8_t size) {
  if (auto* self = Self(cpu)) {
    if (self->runtime_profile_enabled_) {
      ++self->external_writes_[ExternalClass(address)];
      self->NoteExternalOther(address, true);
    }
    // v136b hot path: WGPIPE is a fixed MMIO aperture and cannot alias MEM1,
    // MEM2, paged VMEM or locked cache. Most draw-heavy ExternalWrite calls
    // land here, so handle it before generic address-space probes.
    // Partial lines remain inside the native superchain; only a complete
    // 32-byte gather line becomes a host-visible CP/GPU boundary.
    if (GekkoAOT::GX::HostBridge::IsWriteGatherPipe(address)) {
      // The generated memory-service boundary has materialized the current
      // instruction and all dirty state. Exit BEFORE accepting any bytes so
      // RFI retries exactly this store, retaining the partial physical gather.
      if (self->fifo_irq_fence_enabled_ &&
          self->cp_.FifoReadWriteDistance() > self->cp_.FifoHiWatermark() &&
          self->TryTakeFifoWriteInterrupt()) return;
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

      // v184: the overwhelmingly common retail GX store is a 32-bit WGPIPE
      // write that does not complete the current 32-byte gather line. Keep
      // that partial-line case in this already-hot ExternalWrite path instead
      // of entering the much larger QueueGpuFifoGather() helper. The store
      // that completes the line still uses the authoritative helper below, so
      // PI/CP visibility, host-write epochs, breakpoints and renderer ordering
      // remain byte-for-byte identical.
      if (self->fast_wgpipe_enabled_ && self->native_gx_.Ready() &&
          (size == 1u || size == 2u || size == 4u || size == 8u)) {
        const std::size_t room =
            self->gpu_fifo_gather_.size() - self->gpu_fifo_gather_size_;
        if (room > size) {
          if (self->gpu_fifo_gather_size_ == 0u) self->gpu_fifo_first_pc_ = cpu->pc;
          auto* out = self->gpu_fifo_gather_.data() + self->gpu_fifo_gather_size_;
          // v219: CoD Finest Hour's hottest renderer loop uses `sth` to the
          // write-gather pipe.  The old v184 shortcut only covered 32-bit
          // writes, sending every 8/16/64-bit partial store through the larger
          // QueueGpuFifoGather() path. Keep each scalar width branchless after
          // selection so the existing 32-bit path does not regress.
          switch (size) {
          case 1u:
            out[0] = static_cast<std::uint8_t>(value);
            break;
          case 2u:
            out[0] = static_cast<std::uint8_t>(value >> 8u);
            out[1] = static_cast<std::uint8_t>(value);
            break;
          case 4u:
            out[0] = static_cast<std::uint8_t>(value >> 24u);
            out[1] = static_cast<std::uint8_t>(value >> 16u);
            out[2] = static_cast<std::uint8_t>(value >> 8u);
            out[3] = static_cast<std::uint8_t>(value);
            break;
          case 8u:
            out[0] = static_cast<std::uint8_t>(value >> 56u);
            out[1] = static_cast<std::uint8_t>(value >> 48u);
            out[2] = static_cast<std::uint8_t>(value >> 40u);
            out[3] = static_cast<std::uint8_t>(value >> 32u);
            out[4] = static_cast<std::uint8_t>(value >> 24u);
            out[5] = static_cast<std::uint8_t>(value >> 16u);
            out[6] = static_cast<std::uint8_t>(value >> 8u);
            out[7] = static_cast<std::uint8_t>(value);
            break;
          }
          self->gpu_fifo_gather_size_ += size;
          return;
        }
      }

      const std::uint64_t before = self->gpu_fifo_bursts_;
      if (self->QueueGpuFifoGather(value, size, cpu->pc)) {
        // A complete write-gather line is the actual hardware visibility
        // boundary. Partial lines stay inside the native superchain.
        if (self->gpu_fifo_bursts_ != before) BumpHostWriteEpoch(cpu);
        return;
      }
    }
    const std::uint32_t physical = AddressSpace::ToPhysical(address);
    const bool flipper_mmio = physical >= 0x0c000000u && physical < 0x0c007000u;
    // v159: fixed Flipper MMIO pages cannot alias guest RAM or locked cache.
    // Skip three generic mapping probes on every CP/PE/VI/PI/MI/DSP/DI/SI/EXI/AI write.
    if (!flipper_mmio) {
      if (self->AccessPagedVmem(address, &value, size, true)) return;
      // See ExternalRead(): compiled OS interrupt handlers may carry physical
      // MEM1 pointers out of the low-memory vector while translation is restored.
      if (WriteAddressSpaceValue(self->memory_, address, value, size)) return;
      if (self->lc_.Write(address, value, size)) {
        BumpHostWriteEpoch(cpu);
        return;
      }
    }

    if (flipper_mmio) {
      const auto page = physical & 0xfffff000u;
      if (page == HW::CP::NativeCP::BasePhysical ||
          page == HW::PE::NativePE::BasePhysical ||
          page == HW::PI::NativePI::BasePhysical) {
        (void)self->native_gx_.FlushWrites(GX::HostBridge::SyncReason::MmioWrite);
        self->SyncNativeGXInterrupts();
      }
    }

    BumpHostWriteEpoch(cpu);
    switch (physical & 0xfffff000u) {
    case HW::CP::NativeCP::BasePhysical:
      if (self->cp_.Write(address, value, size)) {
        self->SyncNativeCPInterrupt();
        return;
      }
      break;
    case HW::PE::NativePE::BasePhysical:
      if (self->pe_.Write(address, value, size)) {
        self->SyncNativePEInterrupts();
        return;
      }
      break;
    case HW::VI::NativeVI::BasePhysical:
      if (self->vi_.Write(address, value, size)) {
        if (self->vi_.ConsumeInterruptUpdate()) self->SyncNativeVIInterrupt();
        return;
      }
      break;
    case HW::PI::NativePI::BasePhysical:
      if (self->pi_.Write(address, value, size)) return;
      break;
    case HW::MI::NativeMI::BasePhysical:
      if (self->mi_.Write(address, value, size)) return;
      break;
    case DSP::NativeDSP::BasePhysical:
      if (self->dsp_.Write(address, value, size)) {
        self->SyncNativeDSPInterrupt();
        return;
      }
      break;
    case 0x0c006000u:
      switch (physical & 0x00000f00u) {
      case 0x000u:
        if (self->di_.Write(address, value, size)) {
          if (self->di_async_completion_pending_ &&
              (self->di_.DMAControl() & 1u) == 0u) {
            self->di_async_completion_pending_ = false;
            self->di_async_cycles_remaining_ = 0u;
            self->di_async_staging_.clear();
            static unsigned cancel_logs = 0u;
            if (EnvTraceDvd() && cancel_logs++ < 32u)
              std::fprintf(stderr,
                           "GEKKOAOT_NATIVE_DI_ASYNC_V98=1 phase=cancel-immediate seq=%llu pc=%08x\n",
                           static_cast<unsigned long long>(self->di_async_sequence_), cpu->pc);
          }
          self->SyncNativeDIInterrupt();
          return;
        }
        break;
      case 0x400u:
        if (self->si_.Write(address, value, size)) {
          self->SyncNativeSIInterrupt();
          return;
        }
        break;
      case 0x800u:
        if (self->exi_.WriteRegister(address, value, size)) {
          self->SyncNativeEXIInterrupt();
          return;
        }
        break;
      case 0xc00u:
        if (self->ai_.Write(address, value, size)) {
          self->SyncNativeAIInterrupt();
          return;
        }
        break;
      default:
        break;
      }
      break;
    default:
      break;
    }
    if (physical >= 0x08000000u && physical < 0x0c000000u) {
      if (size == 4u && (physical & 0x00800000u) == 0u &&
          self->native_gx_.Ready()) {
        const auto x = static_cast<std::uint16_t>((physical & 0xfffu) >> 2u);
        const auto y = static_cast<std::uint16_t>((physical >> 12u) & 0x3ffu);
        if (self->native_gx_.PokeEfb(x, y, static_cast<std::uint32_t>(value),
                                     (physical & 0x00400000u) != 0u)) {
          static unsigned efb_poke_logs = 0u;
          if (efb_poke_logs++ < 16u)
            std::fprintf(stderr,
                         "GEKKOAOT_EFB_POKE_V164=1 ea=%08x x=%u y=%u depth=%u value=%08x pc=%08x\n",
                         address, unsigned(x), unsigned(y),
                         (physical & 0x00400000u) != 0u ? 1u : 0u,
                         static_cast<std::uint32_t>(value), cpu->pc);
          BumpHostWriteEpoch(cpu);
          return;
        }
      }
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
  self->CountHostEffect();

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

void HostRuntime::RegisterNativeHookCodeProof(std::uint32_t address) {
  native_hook_code_proofs_.erase(address);
  const auto* descriptor = module_.Descriptor();
  std::uint32_t chunk = 0u;
  if (!descriptor || !descriptor->chunk_hashes ||
      !FindModuleChunk(descriptor, address, &chunk) ||
      descriptor->chunk_hashes[chunk] == 0u) return;
  const auto range = descriptor->chunk_ranges[chunk];
  native_hook_code_proofs_[address] = {range.start, range.end,
                                     descriptor->chunk_hashes[chunk], 0u, false};
}

bool HostRuntime::NativeHookCodeMatches(std::uint32_t address) {
  if (!has_fixed_secondary_modules_) return true;
  const auto found = native_hook_code_proofs_.find(address);
  if (found == native_hook_code_proofs_.end()) return true;
  auto& proof = found->second;
  if (proof.epoch != fixed_exec_route_epoch_) {
    const auto size = static_cast<std::size_t>(proof.end - proof.start);
    const auto* bytes = memory_.Resolve(proof.start, size);
    proof.active = bytes && Fnv1a64(bytes, size) == proof.hash;
    proof.epoch = fixed_exec_route_epoch_;
    static unsigned stale_hook_logs = 0u;
    if (!proof.active && stale_hook_logs++ < 16u)
      std::fprintf(stderr,
                   "GEKKOAOT_OVERLAY_HLE_CODE_PROOF=1 address=%08x active=0 reason=bootstrap-code-replaced\n",
                   address);
  }
  return proof.active;
}

void HostRuntime::RebuildNativeInterceptIndex() {
  native_region_probe_cache_.fill({});
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
  ++os_hle_kind_calls_[static_cast<std::size_t>(kind) & 63u];
  static const bool trace_os_hle = RuntimeEnvBool("GEKKOAOT_TRACE_OS_HLE", false);
  if (trace_os_hle && (hle_calls_ <= 64u || (hle_calls_ & (hle_calls_ - 1u)) == 0u))
    std::fprintf(stderr, "GEKKOAOT_OS_HLE_ENTRY calls=%llu kind=%u address=%08x pc=%08x lr=%08x msr=%08x\n",
                 static_cast<unsigned long long>(hle_calls_), static_cast<unsigned>(kind),
                 address, cpu_.pc, cpu_.lr, cpu_.msr);

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

  // Diagnostic: unbounded trace of OSThread lifecycle services (rare calls).
  static const bool trace_threads = RuntimeEnvBool("GEKKOAOT_TRACE_OS_THREADS", false);
  if (trace_threads) {
    using K = GekkoAOT::NativeOS::Kind;
    switch (kind) {
    case K::CreateThread: case K::ExitThread: case K::CancelThread: case K::JoinThread:
    case K::DetachThread: case K::ResumeThread: case K::SuspendThread: case K::LoadContext: {
      std::uint32_t current = 0;
      memory_.Read32(0x800000e4u, &current);
      std::fprintf(stderr, "GEKKOAOT_OS_THREAD_TRACE kind=%s r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x r8=%08x lr=%08x current=%08x tb=%llx\n",
                   GekkoAOT::NativeOS::Name(kind), cpu_.gpr[3], cpu_.gpr[4], cpu_.gpr[5], cpu_.gpr[6],
                   cpu_.gpr[7], cpu_.gpr[8], cpu_.lr, current, static_cast<unsigned long long>(cpu_.timebase));
      break;
    }
    default: break;
    }
  }
  auto& native_os = GekkoAOT::NativeOS::Service::Get();
  if (GekkoAOT::NativeOS::Service::IsInlineFastKind(kind)) {
    if (native_os.DispatchInlineFast(kind, &cpu_)) return true;
  } else if (kind == GekkoAOT::NativeOS::Kind::YieldThread && yield_spin_enabled_) {
    // v344: a yield with no ready peer of equal or higher priority returns to
    // its caller on the same thread; count it for ParkYieldSpin.
    const auto lr = cpu_.lr;
    std::uint32_t before = 0, after = 0;
    const bool known = memory_.Read32(0x800000e4u, &before);
    if (native_os.Dispatch(kind, &cpu_)) {
      if (known && cpu_.pc == lr && cpu_.exception == 0u &&
          memory_.Read32(0x800000e4u, &after) && after == before) {
        // A yield is a scheduling point: deliver an interrupt that became
        // pending while this AOT chain spun (SRR0 = the yield's return
        // address), instead of letting the loop run until the chain ends.
        if ((cpu_.msr & kPpcMsrEe) != 0u && (pi_.InterruptPending() || decrementer_pending_)) {
          yield_noop_count_ = 0u;
          if (!TryTakeExternalInterrupt()) TryTakeDecrementerInterrupt();
          return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (lr != yield_noop_lr_ || now - yield_spin_start_ > std::chrono::microseconds(64)) {
          yield_noop_lr_ = lr; yield_noop_count_ = 0u; yield_spin_start_ = now;
        }
        if (++yield_noop_count_ >= 64u) {
          yield_noop_count_ = 0u;
          ParkYieldSpin();
          yield_spin_start_ = std::chrono::steady_clock::now();
        }
      }
      return true;
    }
  } else if (native_os.Dispatch(kind, &cpu_)) {
    return true;
  }

  // v320: OSInitThreadQueue is two plain stores, and byte-identical game
  // leaves exist (HPPOA initializes its own lists in paged VM through such a
  // leaf). The native service rejects a pointer it cannot resolve before
  // modifying anything, so run the original compiled body for exactly this
  // call (one-shot region-probe admission) instead of failing the game.
  if (kind == GekkoAOT::NativeOS::Kind::InitThreadQueue && cpu_.exception == 0u &&
      cpu_.pc == address) {
    hle_decline_entry_ = address;
    ++hle_guest_declines_;
    return false;
  }

  // Native mode is fail-closed: there is no guest/Dolphin scheduler fallback.
  // Blocking with an empty run queue is handled by NativeOS::EnterIdle(); a
  // false return now means a genuine native scheduler/ABI failure.
  std::fprintf(stderr,
               "GEKKOAOT_OS_HLE_FAILURE kind=%u entry=%08x pc=%08x lr=%08x "
               "r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x r8=%08x r9=%08x\n",
               static_cast<unsigned>(kind), address, cpu_.pc, cpu_.lr,
               cpu_.gpr[3], cpu_.gpr[4], cpu_.gpr[5], cpu_.gpr[6],
               cpu_.gpr[7], cpu_.gpr[8], cpu_.gpr[9]);
  SetFault(FaultKind::HleFailure, address);
  return false;
}

HostRuntime::GuestRangeStatus HostRuntime::TranslateGuestRange(
    std::uint32_t address, std::uint32_t size, bool write, std::vector<GuestSpan>& spans) {
  spans.clear();
  if (size == 0u) return GuestRangeStatus::Ok;
  if (std::uint64_t(address) + size > 0x100000000ull) return GuestRangeStatus::Invalid;
  std::uint32_t done = 0u;
  while (done < size) {
    const std::uint32_t ea = address + done;
    const std::uint32_t remaining = size - done;
    if (memory_.PagedVmem() && AddressSpace::IsVmem(ea)) {
      const auto translation = TranslateVmemAddress(ea, write);
      // v301: report, never raise -- the caller declines to the guest code,
      // which takes the architectural DSI itself at the exact access.
      if (translation.status != PageTranslation::Status::Mapped)
        return GuestRangeStatus::NotResident;
      const std::uint32_t bytes = std::min<std::uint32_t>(0x1000u - (ea & 0xfffu), remaining);
      if (translation.physical >= memory_.Mem1().size() ||
          bytes > memory_.Mem1().size() - translation.physical)
        return GuestRangeStatus::Invalid;
      spans.push_back({memory_.Mem1().data() + translation.physical, bytes});
      done += bytes;
      continue;
    }
    // v365: the locked-cache scratchpad is plain memory for libc services
    // (Spyro: A Hero's Tail memsets 0xE0001000); it has its own backing.
    if (ea >= HW::LC::NativeLC::Base && ea - HW::LC::NativeLC::Base < HW::LC::NativeLC::Size) {
      const std::uint32_t bytes = std::min<std::uint32_t>(
          remaining, HW::LC::NativeLC::Size - (ea - HW::LC::NativeLC::Base));
      auto* host = static_cast<std::uint8_t*>(lc_.Pointer(ea, bytes));
      if (!host) return GuestRangeStatus::Invalid;
      spans.push_back({host, bytes});
      done += bytes;
      continue;
    }
    const std::size_t available = memory_.ContiguousAvailable(ea);
    if (available == 0u) return GuestRangeStatus::Invalid;
    const auto bytes = static_cast<std::uint32_t>(std::min<std::size_t>(remaining, available));
    auto* host = memory_.Resolve(ea, bytes);
    if (!host) return GuestRangeStatus::Invalid;
    spans.push_back({host, bytes});
    done += bytes;
  }
  return GuestRangeStatus::Ok;
}

bool HostRuntime::DispatchSimpleHle(SimpleHleKind kind, std::uint32_t address) {
  if ((DisabledSimpleServices() >> static_cast<unsigned>(kind)) & 1u) {
    hle_decline_entry_ = address;  // admit the guest body once (see probe)
    ++hle_guest_declines_;
    return false;
  }
  ++hle_calls_;
  ++simple_hle_kind_calls_[static_cast<std::size_t>(kind)];
  const auto finish=[&]() { cpu_.pc=cpu_.lr; return true; };
  // Simple libc string HLEs may receive pointers in the SDK VM aperture.
  // AddressSpace::Resolve() intentionally returns nullptr for paged VMEM, so
  // treating every C string as one host-contiguous span makes valid calls such
  // as CoD Finest Hour's strpbrk() fail as a native-HLE error. Keep a few
  // per-dispatch scratch buffers so two-argument string helpers can hold both
  // translated strings at once. The guest address remains authoritative for
  // return values; the copied bytes are only a host view of the string.
  std::array<std::vector<std::uint8_t>,4> c_string_scratch{};
  std::size_t c_string_scratch_used=0u;
  bool c_string_not_resident=false;
  const auto c_string=[&](std::uint32_t guest,const std::uint8_t** data,std::size_t* length)->bool {
    if(!data||!length)return false;
    if(memory_.PagedVmem() && AddressSpace::IsVmem(guest)) {
      if(c_string_scratch_used>=c_string_scratch.size()) return false;
      auto& copy=c_string_scratch[c_string_scratch_used++];
      std::uint32_t ea=guest;
      const std::uint32_t vmem_end=
          AddressSpace::FakeVmemBase+AddressSpace::RetailFakeVmemSize;
      while(AddressSpace::IsVmem(ea) && ea<vmem_end) {
        const auto translation=TranslateVmemAddress(ea,false);
        if(translation.status!=PageTranslation::Status::Mapped) {
          c_string_not_resident=true;  // v301: decline to the guest code
          return false;
        }
        const std::uint32_t bytes=std::min<std::uint32_t>(
            0x1000u-(ea&0xfffu),vmem_end-ea);
        if(translation.physical>=memory_.Mem1().size() ||
           bytes>memory_.Mem1().size()-translation.physical) return false;
        const auto* begin=memory_.Mem1().data()+translation.physical;
        const auto* end=static_cast<const std::uint8_t*>(std::memchr(begin,0,bytes));
        copy.insert(copy.end(),begin,end ? end : begin+bytes);
        if(end) {
          copy.push_back(0u);
          *data=copy.data();
          *length=copy.size()-1u;
          return true;
        }
        ea+=bytes;
      }
      return false;
    }
    const std::size_t available=memory_.ContiguousAvailable(guest);
    if(available==0u)return false;
    const auto* ptr=memory_.Resolve(guest,1u);
    if(!ptr)return false;
    const void* end=std::memchr(ptr,0,available);
    if(!end)return false;
    *data=ptr;
    *length=static_cast<const std::uint8_t*>(end)-ptr;
    return true;
  };
  // v301: a paged-VM operand is not resident. Nothing has been modified yet:
  // admit the original AOT body for exactly this entry once (the generated
  // static-HLE stub falls through to its region probe when the service
  // declines) so the guest loop faults page by page with resumable state.
  const auto decline_to_guest=[&]() -> bool {
    hle_decline_entry_=address;
    ++hle_guest_declines_;
    return false;
  };
  const auto string_hle_failure=[&]() -> bool {
    if(c_string_not_resident) return decline_to_guest();
    SetFault(FaultKind::HleFailure,address);
    return false;
  };
  // v300: VM-correct memory helpers (TranslateGuestRange).
  const auto range_failure=[&](GuestRangeStatus status) -> bool {
    // v365: a range a native service cannot translate (an unusual mapping)
    // runs the original guest body, which performs the exact architectural
    // accesses, instead of stopping the game.
    static unsigned decline_logs=0;
    if(status!=GuestRangeStatus::NotResident && decline_logs++<16u)
      std::fprintf(stderr,"GEKKOAOT_NATIVE_HLE_RANGE_DECLINE_V365 pc=%08x kind=%u r3=%08x r4=%08x r5=%08x action=guest-body\n",
                   address,static_cast<unsigned>(kind),cpu_.gpr[3],cpu_.gpr[4],cpu_.gpr[5]);
    return decline_to_guest();
  };
  const auto gather=[](const std::vector<GuestSpan>& spans,std::uint8_t* out) {
    for(const auto& span:spans){std::memcpy(out,span.host,span.length);out+=span.length;}
  };
  const auto scatter=[](const std::vector<GuestSpan>& spans,const std::uint8_t* in) {
    for(const auto& span:spans){std::memmove(span.host,in,span.length);in+=span.length;}
  };
  // v301: bounded C-string read with guest access fidelity -- page by page,
  // never touching a byte past the terminator or the limit. *length excludes
  // the terminator; *terminated reports whether one was found within limit.
  // A null buffer only measures.
  const auto bounded_string=[&](std::uint32_t base,std::uint32_t limit,std::vector<std::uint8_t>* buf,
                                std::uint32_t* length,bool* terminated)->GuestRangeStatus {
    if(buf) buf->clear();
    *length=0u; *terminated=false;
    auto& span=hle_spans_[0];
    while(*length<limit) {
      const std::uint32_t ea=base+*length;
      const std::uint32_t chunk=std::min<std::uint32_t>(limit-*length,0x1000u-(ea&0xfffu));
      if(const auto st=TranslateGuestRange(ea,chunk,false,span);st!=GuestRangeStatus::Ok) return st;
      for(const auto& piece:span) {
        const auto* zero=static_cast<const std::uint8_t*>(std::memchr(piece.host,0,piece.length));
        const std::uint32_t take=zero?static_cast<std::uint32_t>(zero-piece.host):piece.length;
        if(buf) buf->insert(buf->end(),piece.host,piece.host+take);
        *length+=take;
        if(zero){*terminated=true;return GuestRangeStatus::Ok;}
      }
    }
    return GuestRangeStatus::Ok;
  };
  switch(kind) {
  case SimpleHleKind::GxSetArray: {
    bool leaf=false;
    if(!native_sdk_gx_arrays_enabled_ ||
       !GekkoAOTSdk::ProveGxSetArray(memory_.Resolve(address,140u),140u,&leaf)) return decline_to_guest();
    std::array<std::uint8_t,12> packet{};
    if(!GekkoAOTSdk::GxVertexArrayPacket(cpu_.gpr[3],cpu_.gpr[4],cpu_.gpr[5],packet,leaf)) return decline_to_guest();
    if(!native_gx_.Ready()) return decline_to_guest();
    const auto index=(cpu_.gpr[3]==25u?10u:cpu_.gpr[3])-9u;
    const auto stride=cpu_.gpr[5]&0xffu;
    const auto input_attribute=cpu_.gpr[3],input_base=cpu_.gpr[4];
    if(TryTakeFifoWriteInterrupt(true)) return true;
    struct AtomicService {
      bool& active; bool previous;
      explicit AtomicService(bool& f):active(f),previous(f){active=true;}
      ~AtomicService(){active=previous;}
    } atomic(atomic_fifo_service_);
    if(native_sdk_gx_packets_enabled_) {
      // v313: publish a typed host operation at its exact transport position.
      // The guest FIFO mirror/accounting remain observable; the renderer skips
      // decoding these two CP commands only after checking the transport and
      // a complete-command boundary. Backlogged/breakpoint/custom paths retain
      // their original decoder. No guest pointer or CPUState crosses this ABI.
      const bool direct = CpuFifoTargetsGpu() &&
          ((!pi_.FifoBase() && !pi_.FifoEnd()) || GekkoAOT::NativeHW::CP::CanDirectSubmit(
            native_hw_.CP(), native_gx_.Ready(), cp_.GPLinkEnabled(), cp_.GPReadEnabled(),
            cp_.BreakpointActiveForHost() || cp_.RegisterDrainPending(), cp_.FifoReadWriteDistance(),
            cp_.FifoReadPointer() == cp_.FifoWritePointer()));
      if(direct && native_gx_.HasTypedArrayCommands() &&
          !native_gx_.StageArrayBinding(index,input_base,stride,cpu_fifo_gather_size_+gpu_fifo_gather_size_)) {
        SetFault(FaultKind::MmioWrite,0xcc008000u,12u);return false;
      }
      if(!QueueSdkFifoPacket(packet.data(),packet.size())) return false;
    } else {
      for(unsigned off=0;off<12;off+=6) {
        ExternalWrite(&cpu_,0xcc008000,packet[off],1);
        ExternalWrite(&cpu_,0xcc008000,packet[off+1],1);
        const auto word=(std::uint32_t(packet[off+2])<<24)|(std::uint32_t(packet[off+3])<<16)|
                        (std::uint32_t(packet[off+4])<<8)|packet[off+5];
        ExternalWrite(&cpu_,0xcc008000,word,4);
        if(fault_.kind!=FaultKind::None || cpu_.exception)return false;
      }
    }
    // Preserve even the leaf's volatile register/CR/XER effects. The admitted
    // vertex path ends at bltlr after its second addic. (index - 12 < 0).
    if(leaf) {
      cpu_.gpr[0]=stride;cpu_.gpr[3]=index|0xb0u;cpu_.gpr[4]=input_base&0x3fffffffu;
      cpu_.gpr[6]=0xcc010000u;cpu_.gpr[7]=8u;
      cpu_.cr=(cpu_.cr&0x0fffffffu)|(input_attribute==25u?0x20000000u:0x80000000u)|((cpu_.xer&0x80000000u)>>3);
    } else {
      cpu_.gpr[0]=index-12u;cpu_.gpr[3]=0xcc010000u;cpu_.gpr[4]=stride;cpu_.gpr[6]=index;
      cpu_.xer&=~0x20000000u;
      cpu_.cr=(cpu_.cr&0x0fffffffu)|0x80000000u|((cpu_.xer&0x80000000u)>>3);
    }
    return finish();
  }
  case SimpleHleKind::DvdFastOpen: {
    // Use the SDK's live SDA globals, not a guessed low-memory FST mirror.
    // All reads/writes are admitted before mutation; paged/invalid/aliased
    // operands decline to the original resumable AOT body.
    if (!EnvNativeDvdFst()) return decline_to_guest();
    const auto proof=GekkoAOTSdk::ProveDvdFastOpen(memory_.Resolve(address,116u),116u);
    if (!proof) return decline_to_guest();
    const auto entry=cpu_.gpr[3];
    if (static_cast<std::int32_t>(entry)<0) {cpu_.gpr[3]=0;return finish();}
    const auto count_addr=cpu_.gpr[13]+static_cast<std::uint32_t>(proof->count_offset);
    const auto fst_addr=cpu_.gpr[13]+static_cast<std::uint32_t>(proof->fst_offset);
    auto read_word=[&](std::uint32_t ea,std::uint32_t& word) {
      auto& spans=hle_spans_[0];
      if (TranslateGuestRange(ea,4u,false,spans)!=GuestRangeStatus::Ok) return false;
      std::array<std::uint8_t,4> bytes{};gather(spans,bytes.data());
      word=(std::uint32_t(bytes[0])<<24)|(std::uint32_t(bytes[1])<<16)|
           (std::uint32_t(bytes[2])<<8)|bytes[3];return true;
    };
    std::uint32_t count=0,fst=0;
    if (!read_word(count_addr,count)) return decline_to_guest();
    if (entry>=count) {cpu_.gpr[3]=0;return finish();}
    if (!read_word(fst_addr,fst) || entry>(UINT32_MAX-8u)/12u ||
        fst>UINT32_MAX-entry*12u-8u) return decline_to_guest();
    const auto item=fst+entry*12u;
    std::uint32_t name=0,start=0,length=0;
    if (!read_word(item,name)) return decline_to_guest();
    if (name&0xff000000u) {cpu_.gpr[3]=0;return finish();}
    if (!read_word(item+4u,start) || !read_word(item+8u,length)) return decline_to_guest();
    const auto info=cpu_.gpr[4];
    if (info>UINT32_MAX-0x3bu) return decline_to_guest();
    std::array<std::vector<GuestSpan>,3> inputs;
    for (unsigned i=0;i<3;++i) {
      const auto src=i==0?count_addr:i==1?fst_addr:item;
      if (TranslateGuestRange(src,i==2?12u:4u,false,inputs[i])!=GuestRangeStatus::Ok)
        return decline_to_guest();
    }
    constexpr std::array<std::uint32_t,4> offsets{0x30,0x34,0x38,0x0c};
    const std::array<std::uint32_t,4> values{start,length,0,0};
    for (std::size_t i=0;i<4;++i) {
      const auto dst=info+offsets[i];
      const auto overlaps=[&](std::uint32_t src,std::uint32_t bytes) {
        return std::uint64_t(dst)<std::uint64_t(src)+bytes && std::uint64_t(src)<std::uint64_t(dst)+4u;
      };
      if (overlaps(count_addr,4)||overlaps(fst_addr,4)||overlaps(item,12) ||
          TranslateGuestRange(dst,4u,true,hle_spans_[i])!=GuestRangeStatus::Ok)
        return decline_to_guest();
      // Cached/uncached aliases and translated VM pages can overlap despite
      // distinct EAs. Preserve the original leaf's write/re-read ordering.
      for (const auto& out:hle_spans_[i]) {
        const auto overlaps_host=[&](const GuestSpan& in) {
          const auto a=reinterpret_cast<std::uintptr_t>(out.host);
          const auto b=reinterpret_cast<std::uintptr_t>(in.host);
          return a<b+in.length && b<a+out.length;
        };
        for (const auto& input:inputs)
          for (const auto& in:input) if (overlaps_host(in)) return decline_to_guest();
        for (std::size_t j=0;j<i;++j)
          for (const auto& in:hle_spans_[j]) if (overlaps_host(in)) return decline_to_guest();
      }
    }
    for (std::size_t i=0;i<4;++i) {
      const auto v=values[i];
      const std::array<std::uint8_t,4> bytes{std::uint8_t(v>>24),std::uint8_t(v>>16),std::uint8_t(v>>8),std::uint8_t(v)};
      scatter(hle_spans_[i],bytes.data());
    }
    cpu_.gpr[3]=1;return finish();
  }
  case SimpleHleKind::PadRead: {
    if (!EnvNativePad()) return decline_to_guest();
    // v300 native PAD service: PADRead(PADStatus status[4]) from a host input
    // snapshot taken at call time -- no SI poll/latch latency. Retail 12-byte
    // layout and conventions (big-endian buttons, centred s8 sticks, err -1 for
    // an absent controller) via Input::Encode; returns the motor-capable
    // channel mask (PAD_CHAN0_BIT >> chan) as the SDK does.
    auto& out=hle_spans_[1];
    if(const auto st=TranslateGuestRange(cpu_.gpr[3],48u,true,out);st!=GuestRangeStatus::Ok) return range_failure(st);
    std::array<std::uint8_t,48> status{};
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
    if(native_input_ready_) {
      GekkoAOT::Input::HostInput().Poll(native_pads_);
      ++native_input_samples_;
    }
    const std::uint32_t motor=GekkoAOT::Input::Encode(native_pads_,status.data());
#else
    const std::uint32_t motor=GekkoAOT::Input::Encode({},status.data());
#endif
    scatter(out,status.data());
    ++native_pad_reads_;
    cpu_.gpr[3]=motor;
    return finish();
  }
  case SimpleHleKind::OsGetTime: NoteClockCall(true); return GekkoAOT::Time::GetTime(&cpu_);
  case SimpleHleKind::OsGetTick: NoteClockCall(false); return GekkoAOT::Time::GetTick(&cpu_);
  case SimpleHleKind::Memcpy:
  case SimpleHleKind::Memmove: {
    // memmove semantics for both: an overlapping memcpy is undefined in C.
    const std::uint32_t dst_addr=cpu_.gpr[3],src_addr=cpu_.gpr[4],size=cpu_.gpr[5];
    auto& src=hle_spans_[0]; auto& dst=hle_spans_[1];
    if(const auto st=TranslateGuestRange(src_addr,size,false,src);st!=GuestRangeStatus::Ok) return range_failure(st);
    if(const auto st=TranslateGuestRange(dst_addr,size,true,dst);st!=GuestRangeStatus::Ok) return range_failure(st);
    if(src.size()<=1u && dst.size()<=1u) {
      if(size) std::memmove(dst[0].host,src[0].host,size);
    } else {
      hle_scratch_.resize(size); gather(src,hle_scratch_.data()); scatter(dst,hle_scratch_.data());
    }
    cpu_.gpr[3]=dst_addr;
    return finish();
  }
  case SimpleHleKind::Memset:
  case SimpleHleKind::Bzero: {
    const bool bzero=kind==SimpleHleKind::Bzero;
    const std::uint32_t dst_addr=cpu_.gpr[3],size=bzero?cpu_.gpr[4]:cpu_.gpr[5];
    const int value=bzero?0:static_cast<int>(cpu_.gpr[4]&0xffu);
    auto& dst=hle_spans_[1];
    if(const auto st=TranslateGuestRange(dst_addr,size,true,dst);st!=GuestRangeStatus::Ok) return range_failure(st);
    for(const auto& span:dst) std::memset(span.host,value,span.length);
    if(!bzero) cpu_.gpr[3]=dst_addr;
    return finish();
  }
  case SimpleHleKind::Memcmp: {
    const std::uint32_t lhs_addr=cpu_.gpr[3],rhs_addr=cpu_.gpr[4],size=cpu_.gpr[5];
    auto& lhs=hle_spans_[0]; auto& rhs=hle_spans_[1];
    if(const auto st=TranslateGuestRange(lhs_addr,size,false,lhs);st!=GuestRangeStatus::Ok) return range_failure(st);
    if(const auto st=TranslateGuestRange(rhs_addr,size,false,rhs);st!=GuestRangeStatus::Ok) return range_failure(st);
    const std::uint8_t *a=nullptr,*b=nullptr;
    if(lhs.size()<=1u && rhs.size()<=1u) {
      if(size){a=lhs[0].host;b=rhs[0].host;}
    } else {
      hle_scratch_.resize(std::size_t(size)*2u);
      gather(lhs,hle_scratch_.data()); gather(rhs,hle_scratch_.data()+size);
      a=hle_scratch_.data(); b=hle_scratch_.data()+size;
    }
    // Retail libc returns the difference of the first mismatching bytes.
    std::int32_t result=0;
    for(std::uint32_t i=0;i<size;++i) if(a[i]!=b[i]){result=int(a[i])-int(b[i]);break;}
    cpu_.gpr[3]=static_cast<std::uint32_t>(result); return finish();
  }
  case SimpleHleKind::Strlen: {
    const std::uint8_t* src=nullptr; std::size_t length=0;
    if(!c_string(cpu_.gpr[3],&src,&length)) return string_hle_failure();
    cpu_.gpr[3]=static_cast<std::uint32_t>(length); return finish();
  }
  case SimpleHleKind::Strcmp: {
    const std::uint8_t *lhs=nullptr,*rhs=nullptr; std::size_t ll=0,rl=0;
    if(!c_string(cpu_.gpr[3],&lhs,&ll)||!c_string(cpu_.gpr[4],&rhs,&rl)) return string_hle_failure();
    std::size_t i=0; const std::size_t common=std::min(ll,rl);
    for(;i<common&&lhs[i]==rhs[i];++i){}
    const int result = int(lhs[i]) - int(rhs[i]);
    cpu_.gpr[3]=static_cast<std::uint32_t>(static_cast<std::int32_t>(result)); return finish();
  }
  case SimpleHleKind::Strncmp: {
    // Lockstep page walk over both strings: touches exactly the bytes the
    // guest loop reads (up to the first difference or terminator), so paged
    // VM operands raise the same DSI and RFI retries the whole call.
    const std::uint32_t lhs_addr=cpu_.gpr[3],rhs_addr=cpu_.gpr[4],limit=cpu_.gpr[5];
    auto& ls=hle_spans_[0]; auto& rs=hle_spans_[1];
    std::int32_t result=0; bool done=false;
    for(std::uint32_t i=0u;i<limit&&!done;) {
      const std::uint32_t la=lhs_addr+i,ra=rhs_addr+i;
      const std::uint32_t chunk=std::min({limit-i,0x1000u-(la&0xfffu),0x1000u-(ra&0xfffu)});
      if(const auto st=TranslateGuestRange(la,chunk,false,ls);st!=GuestRangeStatus::Ok) return range_failure(st);
      if(const auto st=TranslateGuestRange(ra,chunk,false,rs);st!=GuestRangeStatus::Ok) return range_failure(st);
      const std::uint32_t n=std::min(ls[0].length,rs[0].length);
      const std::uint8_t *a=ls[0].host,*b=rs[0].host;
      for(std::uint32_t k=0u;k<n;++k) {
        if(a[k]!=b[k]){result=int(a[k])-int(b[k]);done=true;break;}
        if(a[k]==0u){done=true;break;}
      }
      i+=n;
    }
    cpu_.gpr[3]=static_cast<std::uint32_t>(result); return finish();
  }
  case SimpleHleKind::Strcpy: {
    const std::uint32_t dst_addr=cpu_.gpr[3],src_addr=cpu_.gpr[4];
    const std::uint8_t* src=nullptr; std::size_t length=0;
    if(!c_string(src_addr,&src,&length)) return string_hle_failure();
    if(length>=std::numeric_limits<std::uint32_t>::max()){SetFault(FaultKind::HleFailure,address);return false;}
    auto& dst=hle_spans_[1];
    if(const auto st=TranslateGuestRange(dst_addr,static_cast<std::uint32_t>(length+1u),true,dst);st!=GuestRangeStatus::Ok) return range_failure(st);
    scatter(dst,src); cpu_.gpr[3]=dst_addr; return finish();
  }
  case SimpleHleKind::Strncpy: {
    // Copy up to the terminator, then NUL-pad to exactly `limit` bytes.
    const std::uint32_t dst_addr=cpu_.gpr[3],src_addr=cpu_.gpr[4],limit=cpu_.gpr[5];
    std::uint32_t length=0u; bool terminated=false;
    if(const auto st=bounded_string(src_addr,limit,&hle_scratch_,&length,&terminated);st!=GuestRangeStatus::Ok) return range_failure(st);
    auto& dst=hle_spans_[1];
    if(const auto st=TranslateGuestRange(dst_addr,limit,true,dst);st!=GuestRangeStatus::Ok) return range_failure(st);
    hle_scratch_.resize(limit,0u);
    scatter(dst,hle_scratch_.data()); cpu_.gpr[3]=dst_addr; return finish();
  }
  case SimpleHleKind::Strcat: {
    const std::uint32_t dst_addr=cpu_.gpr[3],src_addr=cpu_.gpr[4];
    const std::uint8_t *dst_ro=nullptr,*src=nullptr; std::size_t dl=0,sl=0;
    if(!c_string(dst_addr,&dst_ro,&dl)||!c_string(src_addr,&src,&sl)) return string_hle_failure();
    if(dl+sl+1u>=0x100000000ull){SetFault(FaultKind::HleFailure,address);return false;}
    auto& dst=hle_spans_[1];
    if(const auto st=TranslateGuestRange(dst_addr+static_cast<std::uint32_t>(dl),static_cast<std::uint32_t>(sl+1u),true,dst);st!=GuestRangeStatus::Ok) return range_failure(st);
    scatter(dst,src); cpu_.gpr[3]=dst_addr; return finish();
  }
  case SimpleHleKind::Strchr:
  case SimpleHleKind::Strrchr: {
    const std::uint32_t src_addr=cpu_.gpr[3]; const std::uint8_t needle=static_cast<std::uint8_t>(cpu_.gpr[4]);
    const std::uint8_t* src=nullptr; std::size_t length=0;
    if(!c_string(src_addr,&src,&length)) return string_hle_failure();
    const std::size_t count=length+1u; const std::uint8_t* found=nullptr;
    if(kind==SimpleHleKind::Strchr){ found=static_cast<const std::uint8_t*>(std::memchr(src,needle,count)); }
    else { for(std::size_t i=count;i>0u;--i)if(src[i-1u]==needle){found=src+i-1u;break;} }
    cpu_.gpr[3]=found?src_addr+static_cast<std::uint32_t>(found-src):0u; return finish();
  }
  case SimpleHleKind::Memchr: {
    // Page at a time: like the guest loop, never touch bytes past the match.
    const std::uint32_t src_addr=cpu_.gpr[3],size=cpu_.gpr[5]; const std::uint8_t needle=static_cast<std::uint8_t>(cpu_.gpr[4]);
    std::uint32_t done=0u;
    while(done<size) {
      const std::uint32_t ea=src_addr+done;
      const std::uint32_t chunk=std::min<std::uint32_t>(size-done,0x1000u-(ea&0xfffu));
      auto& span=hle_spans_[0];
      if(const auto st=TranslateGuestRange(ea,chunk,false,span);st!=GuestRangeStatus::Ok) return range_failure(st);
      std::uint32_t offset=0u;
      for(const auto& piece:span) {
        if(const auto* hit=static_cast<const std::uint8_t*>(std::memchr(piece.host,needle,piece.length))) {
          cpu_.gpr[3]=ea+offset+static_cast<std::uint32_t>(hit-piece.host); return finish();
        }
        offset+=piece.length;
      }
      done+=chunk;
    }
    cpu_.gpr[3]=0u; return finish();
  }
  case SimpleHleKind::Strstr: {
    const std::uint32_t hay_addr=cpu_.gpr[3],needle_addr=cpu_.gpr[4];
    const std::uint8_t *hay=nullptr,*needle=nullptr; std::size_t hl=0,nl=0;
    if(!c_string(needle_addr,&needle,&nl)) return string_hle_failure();
    if(nl==0u){cpu_.gpr[3]=hay_addr;return finish();}
    if(!c_string(hay_addr,&hay,&hl)) return string_hle_failure();
    const auto* found=std::search(hay,hay+hl,needle,needle+nl);
    cpu_.gpr[3]=found==hay+hl?0u:hay_addr+static_cast<std::uint32_t>(found-hay); return finish();
  }
  case SimpleHleKind::Strncat: {
    // Append at most `limit` source bytes (stopping at its terminator) and
    // always terminate, exactly like the retail loop.
    const std::uint32_t dst_addr=cpu_.gpr[3],src_addr=cpu_.gpr[4],limit=cpu_.gpr[5];
    const std::uint8_t* dst_ro=nullptr; std::size_t dl=0;
    if(!c_string(dst_addr,&dst_ro,&dl)) return string_hle_failure();
    if(dl>=0xffffffffull){SetFault(FaultKind::HleFailure,address);return false;}
    std::uint32_t length=0u; bool terminated=false;
    if(const auto st=bounded_string(src_addr,limit,&hle_scratch_,&length,&terminated);st!=GuestRangeStatus::Ok) return range_failure(st);
    hle_scratch_.push_back(0u);
    auto& dst=hle_spans_[1];
    if(const auto st=TranslateGuestRange(dst_addr+static_cast<std::uint32_t>(dl),length+1u,true,dst);st!=GuestRangeStatus::Ok) return range_failure(st);
    scatter(dst,hle_scratch_.data()); cpu_.gpr[3]=dst_addr; return finish();
  }
  case SimpleHleKind::Strnlen: {
    std::uint32_t length=0u; bool terminated=false;
    if(const auto st=bounded_string(cpu_.gpr[3],cpu_.gpr[4],nullptr,&length,&terminated);st!=GuestRangeStatus::Ok) return range_failure(st);
    cpu_.gpr[3]=length; return finish();
  }
  case SimpleHleKind::Strcspn:
  case SimpleHleKind::Strpbrk: {
    const std::uint32_t src_addr=cpu_.gpr[3],set_addr=cpu_.gpr[4];
    const std::uint8_t *src=nullptr,*set=nullptr; std::size_t sl=0,setl=0;
    if(!c_string(src_addr,&src,&sl)||!c_string(set_addr,&set,&setl)) return string_hle_failure();
    std::array<bool,256> table{}; for(std::size_t i=0;i<setl;++i)table[set[i]]=true;
    std::size_t i=0; while(i<sl&&!table[src[i]])++i;
    if(kind==SimpleHleKind::Strcspn) cpu_.gpr[3]=static_cast<std::uint32_t>(i);
    else cpu_.gpr[3]=i<sl?src_addr+static_cast<std::uint32_t>(i):0u;
    return finish();
  }
  case SimpleHleKind::DcFlushRange:
  case SimpleHleKind::DcFlushRangeNoSync:
  case SimpleHleKind::DcStoreRange:
  case SimpleHleKind::DcStoreRangeNoSync:
  case SimpleHleKind::DcInvalidateRange:
  case SimpleHleKind::DcZeroRange:
  case SimpleHleKind::DcTouchRange:
  case SimpleHleKind::IcInvalidateRange: {
    const std::uint32_t addr=cpu_.gpr[3],size=cpu_.gpr[4];
    if(size==0u)return finish();
    const std::uint64_t base=std::uint64_t(addr&~31u);
    const std::uint64_t end=std::uint64_t(addr)+std::uint64_t(size);
    if(end>0x100000000ull){SetFault(FaultKind::HleFailure,address);return false;}
    const std::uint64_t rounded_end=(end+31ull)&~31ull;
    if(rounded_end>0x100000000ull||rounded_end<base){SetFault(FaultKind::HleFailure,address);return false;}
    const std::uint32_t range_addr=static_cast<std::uint32_t>(base);
    const std::uint32_t range_size=static_cast<std::uint32_t>(rounded_end-base);

    if(kind==SimpleHleKind::DcZeroRange){
      auto& dst=hle_spans_[1];
      if(const auto st=TranslateGuestRange(range_addr,range_size,true,dst);st!=GuestRangeStatus::Ok) return range_failure(st);
      for(const auto& span:dst) std::memset(span.host,0,span.length);
      native_gx_.NotifyMemoryWrite(range_addr,range_size);
      return finish();
    }
    if(kind==SimpleHleKind::DcTouchRange) return finish(); // host prefetch is unnecessary
    if(kind!=SimpleHleKind::IcInvalidateRange){
      // Host RAM is coherent; the architectural D-cache operation is the
      // publication boundary for Aurora's cached arrays/textures. Publish the
      // whole SDK range in one DSO call rather than one callback per 32-byte line.
      native_gx_.NotifyMemoryWrite(range_addr,range_size);
      return finish();
    }

    // AOT instruction-cache invalidation: invalidate the low-memory I-cache
    // lines that are actually modeled and drop executable/REL route caches once
    // for the whole SDK range instead of once per guest icbi instruction.
    InvalidateInstructionRange(range_addr,range_size);
    return finish();
  }
  case SimpleHleKind::GxLoadPosMtxImm:
  case SimpleHleKind::GxLoadNrmMtxImm:
  case SimpleHleKind::GxLoadNrmMtxImm3x3:
  case SimpleHleKind::GxLoadTexMtxImm: {
    if (cpu_.gqr[0]!=0u || !(cpu_.msr&0x2000u) ||
        !EnvGxMatrixHle()) return decline_to_guest();
    const bool normal=kind==SimpleHleKind::GxLoadNrmMtxImm || kind==SimpleHleKind::GxLoadNrmMtxImm3x3;
    if (normal && !EnvNativeGxNormalMatrix()) return decline_to_guest();
    // An intercepted SDK packet has no resumable instruction for each chunk.
    // Deliver an already pending overflow IRQ at its entry, then publish the
    // entire bounded packet atomically with respect to this host-side fence.
    if (TryTakeFifoWriteInterrupt(true)) return true;
    struct AtomicFifoService {
      bool& active;
      bool previous;
      explicit AtomicFifoService(bool& flag) : active(flag), previous(flag) { active = true; }
      ~AtomicFifoService() { active = previous; }
    } atomic_service(atomic_fifo_service_);
    // v34: SFA, SMB and the debug ELF corpus all contain the same retail SDK
    // matrix-upload leaves.  They do no CPU-visible work beyond writing an XF
    // command and raw big-endian f32 matrix bytes to WGPIPE.  Feed those bytes
    // into the existing gather/CP path directly so PI/CP breakpoints and FIFO
    // accounting stay authoritative while removing the guest PSQ + MMIO call
    // chain.
    if(!native_gx_.Ready()){SetFault(FaultKind::HleFailure,address);return false;}
    const std::uint32_t matrix_addr=cpu_.gpr[3];
    const std::uint32_t id=cpu_.gpr[4];
    std::uint32_t xf_addr=0u;
    std::uint32_t payload_bytes=0u;
    if(kind==SimpleHleKind::GxLoadPosMtxImm){
      xf_addr=0x000b0000u|(id*4u); payload_bytes=48u;
    }else if(normal){
      xf_addr=0x00080000u|((id*3u)+0x400u);payload_bytes=36u;
    }else{
      const std::uint32_t type=cpu_.gpr[5];
      if(type>1u){SetFault(FaultKind::HleFailure,address);return false;}
      const std::uint32_t count=type==1u?8u:12u;
      const std::uint32_t base=id>=64u?(id-64u)*4u+0x500u:id*4u;
      xf_addr=base|((count-1u)<<16u); payload_bytes=count*4u;
    }
    // GX matrix sources may live in the Gekko locked-cache aperture. SMB1
    // keeps its current position matrix at 0xE0000000, which ordinary MEM1
    // resolution does not cover. Use the same guest-pointer resolver as the
    // compiled CPU's external pointer path.
    const auto source_bytes=kind==SimpleHleKind::GxLoadNrmMtxImm?44u:payload_bytes;
    std::array<std::uint8_t,48> matrix_copy{};
    const auto* matrix=static_cast<const std::uint8_t*>(lc_.Pointer(matrix_addr,source_bytes));
    if (!matrix) {
      auto& spans=hle_spans_[0];
      if (TranslateGuestRange(matrix_addr,source_bytes,false,spans)!=GuestRangeStatus::Ok)
        return decline_to_guest();
      gather(spans,matrix_copy.data());matrix=matrix_copy.data();
    }
    std::array<std::uint8_t,53> fifo{};
    std::size_t n=0u;
    fifo[n++]=0x10u;
    fifo[n++]=static_cast<std::uint8_t>(xf_addr>>24u);
    fifo[n++]=static_cast<std::uint8_t>(xf_addr>>16u);
    fifo[n++]=static_cast<std::uint8_t>(xf_addr>>8u);
    fifo[n++]=static_cast<std::uint8_t>(xf_addr);
    // WriteMTXPS4x3/4x2 use qr0 raw-f32 PSQ stores.  Reproduce the
    // architectural raw-f32 load/store round trip: denormals flush to +0 and
    // signaling NaNs are quieted; all ordinary matrix values remain bit exact.
    for(std::uint32_t off=0;off<payload_bytes;off+=4u){
      const auto src=kind==SimpleHleKind::GxLoadNrmMtxImm?(off/12u)*16u+off%12u:off;
      std::uint32_t bits=(std::uint32_t(matrix[src])<<24u)|
                         (std::uint32_t(matrix[src+1u])<<16u)|
                         (std::uint32_t(matrix[src+2u])<<8u)|matrix[src+3u];
      const std::uint32_t exponent=bits&0x7f800000u;
      const std::uint32_t fraction=bits&0x007fffffu;
      const bool scalar=normal && (kind==SimpleHleKind::GxLoadNrmMtxImm3x3 ? off==32u : off%12u==8u);
      if(!scalar&&exponent==0u&&fraction!=0u) bits=0u;
      else if(exponent==0x7f800000u&&fraction!=0u) bits|=0x00400000u;
      fifo[n++]=static_cast<std::uint8_t>(bits>>24u);
      fifo[n++]=static_cast<std::uint8_t>(bits>>16u);
      fifo[n++]=static_cast<std::uint8_t>(bits>>8u);
      fifo[n++]=static_cast<std::uint8_t>(bits);
    }
    if (native_sdk_gx_packets_enabled_) {
      // v317: a recognized SDK matrix upload is an ordered native renderer
      // operation. The compatibility mirror still receives the canonical bytes
      // for guest FIFO reads/recording; the renderer does not decode XF packets.
      const auto words=payload_bytes/4u;
      const auto destination=xf_addr&0xffffu;
      const bool direct = native_gx_.HasDirectMatrixCommands() &&
          (xf_addr>>16u)+1u==words && GekkoAOT::GX::NativeMatrixRange(destination,words) &&
          CpuFifoTargetsGpu() && ((!pi_.FifoBase() && !pi_.FifoEnd()) ||
          GekkoAOT::NativeHW::CP::CanDirectSubmit(native_hw_.CP(),native_gx_.Ready(),
            cp_.GPLinkEnabled(),cp_.GPReadEnabled(),
            cp_.BreakpointActiveForHost()||cp_.RegisterDrainPending(),cp_.FifoReadWriteDistance(),
            cp_.FifoReadPointer()==cp_.FifoWritePointer()));
      if(direct && !native_gx_.StageMatrix(destination,words,
                                         cpu_fifo_gather_size_+gpu_fifo_gather_size_)) {
        SetFault(FaultKind::MmioWrite,0xcc008000u,n);return false;
      }
      if (!QueueSdkFifoPacket(fifo.data(),n)) return false;
      return finish();
    }
    // Queue up to eight bytes at a time through the normal WGPIPE entrypoint.
    // GXBeginDisplayList may have selected a CPU recording FIFO; submitting
    // directly to the GPU queue would split one physical gather line between
    // the CPU and GPU buffers and fault on the next guest store.
    for(std::size_t off=0;off<n;){
      const std::size_t remaining=n-off;
      const std::uint8_t chunk=remaining>=8u?8u:remaining>=4u?4u:remaining>=2u?2u:1u;
      std::uint64_t value=0u;
      for(std::uint8_t i=0;i<chunk;++i)value=(value<<8u)|fifo[off+i];
      ExternalWrite(&cpu_,0xcc008000u,value,chunk);
      if(fault_.kind!=FaultKind::None || cpu_.exception!=0u)return false;
      off+=chunk;
    }
    return finish();
  }
  case SimpleHleKind::Count: break;
  }
  return false;
}

bool HostRuntime::DispatchNativeRegionProbe() {
  const std::uint32_t start = cpu_.external_addr, end = cpu_.external_value;

  // v301: a native service declined this exact entry (non-resident paged-VM
  // operand). The stub stored the entry PC before calling the service; admit
  // the original guest body once so it faults page by page with exact state.
  if (hle_decline_entry_ != 0u && cpu_.pc == hle_decline_entry_ &&
      start <= hle_decline_entry_ && hle_decline_entry_ < end) {
    hle_decline_entry_ = 0u;
    cpu_.external_rid = 0xffu;
    return false;
  }

  // v205d: when DolRecomp asks whether a native wrapper range is available
  // from inside a secondary module, a range wholly owned by that same REL is
  // already compiled guest code. Do not bounce to HostRuntime merely because
  // a process-wide intercept numerically falls inside the range. A genuine
  // import/main-DOL/HLE target leaves this descriptor's code ranges and still
  // takes the normal host-routing path.
  if (native_region_secondary_owner_ && start < end &&
      AddressInRanges(native_region_secondary_owner_->code_ranges,
                      native_region_secondary_owner_->num_code_ranges, start) &&
      AddressInRanges(native_region_secondary_owner_->code_ranges,
                      native_region_secondary_owner_->num_code_ranges, end - 1u)) {
    cpu_.external_rid = 0xffu;
    static unsigned rel_owned_guard_logs = 0u;
    if (rel_owned_guard_logs++ < 64u)
      std::fprintf(stderr,
                   "GEKKOAOT_REL_REGION_GUARD_V205D=1 start=%08x end=%08x pc=%08x action=allow-secondary-owned-aot\n",
                   start, end, cpu_.pc);
    return false;
  }

  // v204c: one-shot admission for the exact range that produced a proven
  // zero-progress secondary dispatch.
  if (native_region_retry_start_ != 0u &&
      start == native_region_retry_start_) {
    native_region_retry_start_ = 0u;
    cpu_.external_rid = 0xffu;
    static unsigned retry_probe_logs = 0u;
    if (retry_probe_logs++ < 64u)
      std::fprintf(stderr,
                   "GEKKOAOT_REL_REGION_RETRY_V204C=1 start=%08x end=%08x action=allow-one-shot-native-entry\n",
                   start, end);
    return false;
  }

  const std::size_t slot =
      ((start >> 2u) ^ (start >> 13u) ^ (end >> 5u)) &
      (kNativeRegionProbeCacheSize - 1u);
  auto& cached = native_region_probe_cache_[slot];
  bool blocked = false;
  if (cached.valid && cached.start == start && cached.end == end &&
      cached.epoch == fixed_exec_route_epoch_) {
    blocked = cached.blocked;
  } else {
    auto it =
        std::lower_bound(native_intercepts_.begin(), native_intercepts_.end(), start);
    // Fixed ELF/DOL overlays reuse addresses from the bootstrap executable.
    // A bootstrap SDK hook is applicable only while its original code still
    // occupies that address. Otherwise it can reject an entire overlay chunk
    // forever before a single guest instruction executes.
    for (; start < end && it != native_intercepts_.end() && *it < end; ++it) {
      if (NativeHookCodeMatches(*it)) { blocked = true; break; }
    }
    cached = {start, end, blocked, true, fixed_exec_route_epoch_};
  }

  if (blocked && native_region_probe_capture_)
    native_region_last_blocked_start_ = start;

  cpu_.external_rid = 0xffu;
  return blocked;
}

bool HostRuntime::DispatchHostCall(std::uint32_t address) {
  // v274: a handled real HLE call is a host-visible event for the idle-loop
  // prover; admission, REL-translation and indirect-routing probes are not
  // (the guest code an indirect route executes reports its own effects).
  const bool handled = DispatchHostCallImpl(address);
  if (handled && track_host_effects_ && address != kNativeRegionProbe &&
      address != kRelAddressProbe && address != kGlobalIndirectProbe)
    ++host_effects_;
  return handled;
}

bool HostRuntime::DispatchHostCallImpl(std::uint32_t address) {
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
    if (raw < static_cast<std::uint8_t>(SimpleHleKind::Count))
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
    // REL-generated memory accesses can request canonical->runtime translation
    // before control returns through ResolveSecondaryAddress().  Honor the same
    // publication epoch here so they never translate through a stale OSLink map.
    if (rel_mapping_dirty_) RefreshSecondaryRelMappings();
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
  if (address == kNativeRegionProbe) return DispatchNativeRegionProbe();

  // Exact direct-mapped positive cache. Hot OS/libc HLE no longer pays an
  // unordered_map lookup at every call; collisions simply fall through to the
  // authoritative maps below and refresh this slot.
  const auto fast_slot=((address>>2u)^(address>>13u))&(kHleFastCacheSize-1u);
  const auto fast=hle_fast_cache_[fast_slot];
  if(fast.address==address) {
    if (!NativeHookCodeMatches(address)) return false;
    if(fast.type==HleFastType::Os)
      return DispatchOsHle(static_cast<GekkoAOT::NativeOS::Kind>(fast.kind),address);
    if(fast.type==HleFastType::Simple)
      return DispatchSimpleHle(static_cast<SimpleHleKind>(fast.kind),address);
  }

  const auto negative_slot=((address>>2u)^(address>>14u))&(kHostCallNegativeCacheSize-1u);
  if(hostcall_negative_cache_[negative_slot]==address) return false;

  if(auto it=os_hooks_.find(address);it!=os_hooks_.end()) {
    if (!NativeHookCodeMatches(address)) return false;
    hle_fast_cache_[fast_slot]={address,HleFastType::Os,static_cast<std::uint8_t>(it->second)};
    return DispatchOsHle(it->second,address);
  }
  if(auto it=simple_hle_hooks_.find(address);it!=simple_hle_hooks_.end()) {
    if (!NativeHookCodeMatches(address)) return false;
    hle_fast_cache_[fast_slot]={address,HleFastType::Simple,static_cast<std::uint8_t>(it->second)};
    return DispatchSimpleHle(it->second,address);
  }
  hostcall_negative_cache_[negative_slot]=address;
  return false;
}

bool HostRuntime::HostCall(CPUState* cpu, std::uint32_t address) {
  auto* self = Self(cpu);
  if (!self) return false;
  // v274: idle-loop effects are counted by DispatchHostCall (real HLE only);
  // admission, REL-translation and indirect-routing probes are not events.
  // v232: clock getters are proven scalar SDK leaves in the static manifest.
  // Keep the generated callback ABI and host-boundary epoch, but avoid entering
  // the general SDK dispatcher and its libc/VM scratch-buffer machinery. The
  // guest timebase stays authoritative; advancing events is still the runner's
  // responsibility at the same boundary as before.
  if (address == (kStaticDirectSimpleBase | static_cast<std::uint32_t>(SimpleHleKind::OsGetTick))) {
    ++self->hle_calls_;
    ++self->simple_hle_kind_calls_[static_cast<std::size_t>(SimpleHleKind::OsGetTick)];
    self->NoteClockCall(false);
    GekkoAOT::Time::GetTick(&self->cpu_);
    BumpHostWriteEpoch(cpu);
    return true;
  }
  if (address == (kStaticDirectSimpleBase | static_cast<std::uint32_t>(SimpleHleKind::OsGetTime))) {
    ++self->hle_calls_;
    ++self->simple_hle_kind_calls_[static_cast<std::size_t>(SimpleHleKind::OsGetTime)];
    self->NoteClockCall(true);
    GekkoAOT::Time::GetTime(&self->cpu_);
    BumpHostWriteEpoch(cpu);
    return true;
  }
  // v301: strcmp is a pure leaf that EA titles call ~1M times per second.
  // With both strings in cached MEM1 compare in place: no dispatcher, no
  // scratch copies, and no host-write epoch (nothing but r3/PC changes, so
  // timer/interrupt state is unaffected). Anything else -- VM pages, other
  // apertures, a string running off MEM1 -- takes the general VM-safe path.
  static const bool strcmp_fast_path =
      RuntimeEnvBool("GEKKOAOT_STRCMP_FAST_PATH", true) &&
      !((DisabledSimpleServices() >> static_cast<unsigned>(SimpleHleKind::Strcmp)) & 1u);
  if (address == (kStaticDirectSimpleBase | static_cast<std::uint32_t>(SimpleHleKind::Strcmp)) &&
      strcmp_fast_path) {
    const std::uint32_t a = self->cpu_.gpr[3], b = self->cpu_.gpr[4];
    const auto& mem1 = self->memory_.Mem1();
    if ((a & 0xc0000000u) == 0x80000000u && (b & 0xc0000000u) == 0x80000000u) {
      const std::uint32_t oa = a & 0x3fffffffu, ob = b & 0x3fffffffu;
      if (oa < mem1.size() && ob < mem1.size()) {
        const std::size_t limit = mem1.size() - std::max(oa, ob);
        const std::uint8_t* pa = mem1.data() + oa;
        const std::uint8_t* pb = mem1.data() + ob;
        for (std::size_t i = 0; i < limit; ++i) {
          const std::uint8_t ca = pa[i], cb = pb[i];
          if (ca != cb || ca == 0u) {
            self->cpu_.gpr[3] = static_cast<std::uint32_t>(int(ca) - int(cb));
            self->cpu_.pc = self->cpu_.lr;
            ++self->hle_calls_;
            ++self->simple_hle_kind_calls_[static_cast<std::size_t>(SimpleHleKind::Strcmp)];
            return true;
          }
        }
      }
    }
  }
  if (address == kNativeRegionProbe) return self->DispatchNativeRegionProbe();
  if (address == kEeEnableQuery) {
    if (cpu->msr & kPpcMsrEe) self->pe_notification_irq_active_ = false;
    const bool pending = self->EeEnableNeedsBoundary();
    // A native mtmsr side exit publishes the exact next PPC instruction, but
    // module_dispatch can otherwise redispatch that continuation in the same
    // DSO superchain before Run gets a chance to deliver the interrupt. It can
    // even clear EE again first. Mark only an actually requested boundary as
    // an event; the common no-interrupt query remains entirely state-free.
    if (pending && self->ee_pending_barrier_enabled_) BumpHostWriteEpoch(cpu);
    return pending;
  }
  const bool handled = self->DispatchHostCall(address);
  // Compiler/runtime admission probes are not guest-visible events.  Real HLE
  // calls are: stop the native superchain after the current wrapper so timer
  // and interrupt state is settled before more guest work runs.
  // v301: libc leaves only touch guest GPRs and plain memory (a raised DSI
  // already ends the chain through cpu->exception) -- exactly like the guest
  // code they replace, so they do not end the superchain either.
  if (handled && address != kNativeRegionProbe && address != kRelAddressProbe &&
      address != kGlobalIndirectProbe && !IsChainTransparentLeaf(address) &&
      !(self->gx_matrix_chain_enabled_ && IsChainTransparentGxMatrix(address)))
    BumpHostWriteEpoch(cpu);
  return handled;
}


bool HostRuntime::DispatchGlobalIndirect(std::uint32_t runtime_address) {
  if (!runtime_address) return false;

  // A nested native call can cross the module boundary before the outer REL
  // wrapper returns. Publish its caller's LR before the callee can save it.
  if (native_region_secondary_owner_)
    PublishSecondaryControlState(native_region_secondary_owner_);

  TraceRelGprs("global-enter",
               runtime_address, runtime_address, cpu_);

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
  bool secondary_dispatched = false;
  if (ResolveSecondaryAddress(runtime_address, &secondary, &linked) && secondary &&
      secondary->dispatch) {
    TraceRelGprs("global-secondary-enter", runtime_address, linked, cpu_);
    const auto* previous_owner = native_region_secondary_owner_;
    native_region_secondary_owner_ = secondary;
    secondary_dispatched = secondary->dispatch(&cpu_, linked);
    native_region_secondary_owner_ = previous_owner;
  }
  if (secondary_dispatched) {
    PublishSecondaryControlState(secondary);
    cpu_.pc = TranslateSecondaryAddress(secondary, cpu_.pc);
    TraceRelGprs("global-secondary-return",
                 runtime_address, linked, cpu_);
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

bool HostRuntime::TryTakeFifoWriteInterrupt(bool sdk_entry) {
  if (!fifo_irq_fence_enabled_ || atomic_fifo_service_ ||
      cp_.FifoReadWriteDistance() <= cp_.FifoHiWatermark() ||
      (cp_.Control() & 0x0004u) == 0u || !cp_.GPReadEnabled() ||
      (pi_.InterruptCauseValue() & pi_.InterruptMaskValue() &
       GekkoAOT::HW::PI::NativePI::CommandProcessor) == 0u ||
      (cpu_.msr & kPpcMsrEe) == 0u) return false;
  if (!sdk_entry) {
    // Multi-store instructions (stmw, quantized pairs, string stores) can
    // issue several callbacks at one PC. Retrying after a later callback
    // would duplicate their prefix. Update forms can also expose a modified
    // base register. Admit only non-update, single scalar store instructions.
    std::uint32_t instruction = 0u;
    if (!memory_.Read32(cpu_.pc, &instruction)) return false;
    const auto opcode = instruction >> 26u;
    bool single_store = opcode == 36u || opcode == 38u || opcode == 44u ||
                        opcode == 52u || opcode == 54u;
    if (opcode == 31u) {
      const auto xo = (instruction >> 1u) & 0x3ffu;
      single_store = xo == 151u || xo == 215u || xo == 407u ||
                     xo == 663u || xo == 727u || xo == 662u || xo == 918u;
    }
    if (!single_store) return false;
  }
  if (!TryTakeExternalInterrupt()) return false;
  // Same unwind contract as a synchronous memory-service exception. The
  // execution chassis consumes this private latch on entry to vector 0x500.
  cpu_.exception |= kPpcExcExternalUnwind;
  ++fifo_irq_fences_;
  if (fifo_irq_fences_ <= 8u || (fifo_irq_fences_ & (fifo_irq_fences_ - 1u)) == 0u)
    std::fprintf(stderr,
                 "GEKKOAOT_FIFO_IRQ_FENCE_V235=1 count=%llu resume=%08x distance=%08x "
                 "high=%08x staged=%u boundary=%s action=vector500-before-write\n",
                 static_cast<unsigned long long>(fifo_irq_fences_), cpu_.srr0,
                 cp_.FifoReadWriteDistance(), cp_.FifoHiWatermark(),
                 gpu_fifo_gather_size_, sdk_entry ? "sdk-entry" : "scalar-store");
  return true;
}

bool HostRuntime::TryTakeExternalInterrupt() {
  // External interrupts are level-sensitive at the PI boundary. The device/PI
  // cause remains asserted until guest software acknowledges it; this method
  // only performs the CPU-side exception entry when interrupts are enabled.
  // v182: NativePI caches the derived cause&mask level.  Reject the normal
  // no-IRQ case with one boolean load before touching the MMIO register pair.
  if (!pi_.InterruptPending()) return false;

  // Read the cause/mask pair only for an actually pending interrupt.  These
  // values are still used for exact vector/logging semantics below.
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

  ++irq_external_taken_;
  irq_external_causes_ |= pending;
  for (std::uint32_t bits = pending & 0xffffu; bits != 0u; bits &= bits - 1u)
    ++irq_cause_counts_[static_cast<unsigned>(std::countr_zero(bits))];
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
  pe_notification_irq_active_ = true;
  CountHostEffect();
  return true;
}


bool HostRuntime::TryTakeDecrementerInterrupt() {
  // DEC is an EE-masked asynchronous exception just like PI delivery, but it
  // vectors through 0x900 and its CPU-side pending latch is consumed on entry.
  // The architectural DEC register itself keeps counting independently.
  if (!decrementer_pending_ || (cpu_.msr & kPpcMsrEe) == 0u) return false;

  decrementer_pending_ = false;
  ++irq_decrementer_taken_;
  const std::uint32_t old_msr = cpu_.msr;
  cpu_.srr0 = cpu_.pc;
  cpu_.srr1 = old_msr & kPpcMsrRfiMask;

  std::uint32_t exception_msr = old_msr;
  exception_msr = (exception_msr & ~kPpcMsrLe) |
                  ((old_msr & kPpcMsrIle) != 0u ? kPpcMsrLe : 0u);
  exception_msr &= ~kPpcExceptionEntryClearMask;
  cpu_.msr = exception_msr;
  cpu_.pc = 0x00000900u;
  CountHostEffect();
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

void HostRuntime::SyncNativeGXInterrupts(bool polling_token) {
  if(native_gx_.PeEventOverflow()) {
    SetFault(FaultKind::MmioWrite,0xcc00100eu);
    return;
  }
  if(!native_gx_.PeEventPending()) return;
  // Renderer notifications cross a scheduling boundary, not a GPU fence.
  // Keep enabled notifications ordered even inside an EE-disabled critical
  // section: restoring EE must still deliver each pending SDK callback.
  // An explicit token polling boundary can request latest-register semantics.
  if (cpu_.msr & kPpcMsrEe) pe_notification_irq_active_ = false;
  static const bool ordered_irq = RuntimeEnvBool("GEKKOAOT_GX_ORDERED_PE_IRQ", true);
  GX::PeEvent event;
  for (;;) {
    // Publishing several enabled token interrupts before a guest can
    // acknowledge the first folds distinct SDK callbacks into one latch.
    // Keep notifications ordered without stopping GPU command execution.
    const auto mask = pi_.InterruptMaskValue();
    if (ordered_irq && !polling_token &&
        (((mask & HW::PI::NativePI::PeToken) && pe_.TokenInterruptPending()) ||
         ((mask & HW::PI::NativePI::PeFinish) && pe_.FinishInterruptPending()))) break;
    if (!native_gx_.ConsumePeEvent(&event)) break;
    NativeGXPeEvent(this,event.reg,event.value);
  }
}

std::uint32_t HostRuntime::SprRead(CPUState* cpu, std::uint16_t spr, std::uint32_t address) {
  auto* self = Self(cpu);
  if (!self) return 0;
  self->CountHostEffect();
  // User monitor registers are read-only aliases, not independent storage.
  if (spr >= 936u && spr <= 942u) spr += 16u;
  if(self->native_counter_poll_enabled_ &&
     (spr==953u || spr==954u || spr==957u || spr==958u))
    self->ServiceTimedMmioPoll(address,true);

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
  self->CountHostEffect();
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
      std::size_t legacy_fixed_ignored = 0u;
      // v166: fixed-address DOL/ELF files discovered on disc are not necessarily
      // executable overlays. Debug companion ELFs (Terminator 3 ships one) can
      // cover the same virtual addresses as main.dol without ever being entered.
      // Such an unused fixed image must not veto the guest's SDR1 page-table
      // activation merely because it was compiled by an older toolchain.
      //
      // REL modules remain part of the admission fence because OSLink may make
      // them live after paging is enabled. Fixed modules are instead filtered at
      // dispatch time below: a non-restartable one is never entered while real
      // guest paging is active.
      for (const auto& side : self->secondary_modules_) {
        if (!side) continue;
        const auto* descriptor = side->Descriptor();
        if (descriptor && descriptor->num_rel_modules == 0u) {
          if (!side->SupportsMemoryRestart()) ++legacy_fixed_ignored;
          continue;
        }
        restartable = restartable && side->SupportsMemoryRestart();
      }
      const bool enable_pages = value != 0u && restartable;
      if (enable_pages != self->memory_.PagedVmem()) {
        std::fprintf(stderr,
                     "GEKKOAOT_VM_MIGRATION_V97=1 sdr1=%08x paging=%u module_restart=%u side_modules=%zu legacy_fallback=%u\n",
                     value, enable_pages ? 1u : 0u, restartable ? 1u : 0u,
                     self->secondary_modules_.size(), (value && !restartable) ? 1u : 0u);
        if (legacy_fixed_ignored != 0u)
          std::fprintf(stderr,
                       "GEKKOAOT_VM_FIXED_EXEC_CAP_V166=1 sdr1=%08x paging=%u ignored_legacy_fixed=%zu policy=dispatch-fenced\n",
                       value, enable_pages ? 1u : 0u, legacy_fixed_ignored);
        // A previous fixed-image identity may point at a legacy side module.
        // Paging changes its admissibility, so force the next boundary to
        // resolve the executable again under the v166 capability filter.
        self->fixed_exec_route_cache_ = {};
        ++self->fixed_exec_route_epoch_;
        ++self->fixed_exec_structure_epoch_;
        ++self->dispatch_code_probe_generation_;
        self->fixed_exec_active_descriptor_ = nullptr;
        self->fixed_exec_negative_pcs_.fill(0u);
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
    // v335: sample guest DEC programming (bounded) to diagnose decrementer storms.
    static std::uint64_t dec_writes = 0u;
    if ((++dec_writes & 0xffffu) == 1u && dec_writes < (1u << 22)) {
      std::uint32_t frames[4]{}, sp = cpu->gpr[1];
      for (auto& frame : frames) {
        std::uint32_t back = 0u, saved = 0u;
        if (!self->memory_.Read32(sp, &back) || !self->memory_.Read32(back + 4u, &saved)) break;
        frame = saved; sp = back;
      }
      std::fprintf(stderr, "GEKKOAOT_DEC_WRITE_V335=1 n=%llu value=%08x old=%08x pc=%08x lr=%08x tb=%016llx callers=%08x,%08x,%08x,%08x\n",
                   static_cast<unsigned long long>(dec_writes), value, old_value, address, cpu->lr,
                   static_cast<unsigned long long>(cpu->timebase), frames[0], frames[1], frames[2], frames[3]);
    }
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
    if (spr >= 536u && spr <= 542u && (spr & 1u) == 0u)
      self->RefreshVmemBatModes();

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
      if (ok && !memory_to_lc)
        self->native_gx_.NotifyMemoryWrite(memory_address, blocks * 32u);
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
  self->CountHostEffect();

  if (operation == kCacheTlbie) {
    self->InvalidateDataTlb(address);
    static unsigned logs = 0;
    if (logs++ < 32u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_DTLB_TLBIE_V212=1 ea=%08x set=%u pc=%08x invalidations=%llu\n",
                   address, (address >> 12u) & 0x3fu, instruction_address,
                   static_cast<unsigned long long>(self->data_tlb_invalidations_));
    return;
  }

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
  if (operation == kCacheIcbi && !self->IcbiLineTouchesFixedImages(address)) {
    // v274: a line outside every fixed-image chunk cannot change a chunk hash,
    // a native hook proof or an image route, so keep those epoch-local caches.
    // Games that reinstall exception vectors (Ghost Recon 2 issues millions of
    // icbi on 0x80000d00..) otherwise forced a full route, hook-proof and
    // 128 KiB dispatch-probe rebuild after every line. Only the code that
    // reads this line is invalidated: the low-vector interpreter cache, the
    // dispatch probes that decode it and, outside the vector page where no
    // REL can live, the lazy REL mapping.
    self->InvalidateLowInstructionCacheLine(address);
    if (AddressSpace::ToPhysical(address) >= kLowInstructionCacheSpan)
      self->rel_mapping_dirty_ = true;
    self->InvalidateDispatchProbesForRange(address, 32u);
    ++self->icbi_route_invalidations_;
    static unsigned targeted_logs = 0;
    if (self->runtime_profile_enabled_ && targeted_logs++ < 16u)
      std::fprintf(stderr,
                   "GEKKOAOT_ICBI_TARGETED_V274=1 ea=%08x pc=%08x targeted=%llu action=keep-fixed-image-route\n",
                   address, instruction_address,
                   static_cast<unsigned long long>(++self->icbi_targeted_invalidations_));
    return;
  }
  if (operation == kCacheIcbi) {
    self->InvalidateLowInstructionCacheLine(address);
    // OSLink/overlay loaders publish relocated instructions through icbi.
    // Defer the expensive MEM1 REL-header scan until a REL route is actually
    // needed, then perform it once for the whole publication epoch.
    self->rel_mapping_dirty_ = true;
    self->fixed_exec_route_cache_ = {};
    ++self->fixed_exec_route_epoch_;
    self->fixed_exec_active_descriptor_ = nullptr;
    self->fixed_exec_negative_pcs_.fill(0u);
    self->InvalidateDispatchProbesForRange(address, 32u);
    ++self->icbi_route_invalidations_;
    static unsigned logs = 0;
    if (logs++ < 64u)
      std::fprintf(stderr,
                   "GEKKOAOT_ICBI_AOT_IDENTITY_V87=1 ea=%08x pc=%08x invalidations=%llu action=drop-fixed-image-route\n",
                   address, instruction_address,
                   static_cast<unsigned long long>(self->icbi_route_invalidations_));
  }
}


void HostRuntime::InvalidateInstructionRange(std::uint32_t range_addr, std::uint32_t range_size) {
    sdk_idle_poll_pc_=0u;
    const std::uint32_t physical=AddressSpace::ToPhysical(range_addr);
    if(physical<kLowInstructionCacheSpan){
      const std::uint64_t physical_end=std::min<std::uint64_t>(
          std::uint64_t(kLowInstructionCacheSpan),std::uint64_t(physical)+range_size);
      const std::size_t first=physical/kLowInstructionCacheLineBytes;
      const std::size_t last=static_cast<std::size_t>((physical_end+kLowInstructionCacheLineBytes-1u)/kLowInstructionCacheLineBytes);
      for(std::size_t line=first;line<last&&line<low_instruction_cache_valid_.size();++line)
        low_instruction_cache_valid_[line]=false;
    }
    rel_mapping_dirty_=true;
    fixed_exec_route_cache_={};
    ++fixed_exec_route_epoch_;
    fixed_exec_active_descriptor_=nullptr;
    fixed_exec_negative_pcs_.fill(0u);
    InvalidateDispatchProbesForRange(range_addr, range_size);
    icbi_route_invalidations_ += range_size/32u;
}

bool HostRuntime::DispatchCopiedCacheStub(std::uint32_t address, std::uint64_t* cycles) {
  CountHostEffect();
  // The SDK apploader copies this complete cache-publication leaf into RAM
  // and patches its two range immediates. Lower only the proven whole routine;
  // arbitrary RAM code still requires a compiled module.
  constexpr std::array<std::uint32_t, 15> tail = {
      0x54860034u, 0x7ce42a14u, 0x7ce63850u, 0x38e7001fu, 0x54e7d97eu,
      0x7ce903a6u, 0x7c0037acu, 0x38c60020u, 0x4200fff8u, 0x7c0004acu,
      0x4c00012cu, 0x8001000cu, 0x7c0803a6u, 0x38210008u, 0x4e800020u};
  if (!cycles || (address & 3u) || (cpu_.msr & kPpcMsrPr)) return false;
  const auto* code = memory_.Resolve(address, 76u);
  if (!code) return false;
  const auto word = [&](unsigned i) {
    const auto* p = code + i * 4u;
    return (std::uint32_t(p[0]) << 24u) | (std::uint32_t(p[1]) << 16u) |
           (std::uint32_t(p[2]) << 8u) | p[3];
  };
  constexpr std::array<std::uint32_t,4> prefix = {
      0x3c800000u, 0x38840000u, 0x3ca00000u, 0x38a50000u};
  for (unsigned i=0; i<4; ++i)
    if ((word(i) & 0xffff0000u) != prefix[i]) return false;
  for (unsigned i=0; i<tail.size(); ++i)
    if (word(i+4u) != tail[i]) return false;
  const auto immediate_pair = [&](unsigned i) {
    return ((word(i) & 0xffffu) << 16u) +
        static_cast<std::uint32_t>(static_cast<std::int16_t>(word(i+1u)));
  };
  const std::uint32_t base = immediate_pair(0), size = immediate_pair(2);
  const std::uint32_t aligned = base & ~31u;
  const std::uint64_t end = std::uint64_t(base) + size;
  const std::uint64_t rounded = (end + 31u) & ~31ull;
  // Reject wrap and zero-CTR loops; do not silently change their semantics.
  if (end + 31u > 0xffffffffull || rounded <= aligned) return false;
  std::uint32_t return_address=0;
  if (cpu_.gpr[1] > 0xfffffff3u ||
      !memory_.Read32(cpu_.gpr[1]+12u, &return_address)) return false;
  const auto span = static_cast<std::uint32_t>(rounded-aligned);
  InvalidateInstructionRange(aligned,span);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  cpu_.gpr[4]=base; cpu_.gpr[5]=size;
  cpu_.gpr[6]=static_cast<std::uint32_t>(rounded); cpu_.gpr[7]=span/32u;
  cpu_.ctr=0; cpu_.gpr[0]=return_address;
  cpu_.gpr[1]+=8u; cpu_.lr=return_address; cpu_.pc=return_address & ~3u;
  *cycles=16u+std::uint64_t(span/32u)*3u;
  static unsigned logs=0;
  if (logs++<16u)
    std::fprintf(stderr,"GEKKOAOT_COPIED_CACHE_STUB_V252=1 pc=%08x base=%08x size=%u return=%08x policy=whole-signature-native-lowering\n",
                 address,base,size,cpu_.pc);
  return true;
}

void HostRuntime::InvalidateLowInstructionCache() {
  sdk_idle_poll_pc_ = 0u;
  low_instruction_cache_valid_.fill(false);
  native_low_vector_cache_.Reset();
}

void HostRuntime::InvalidateLowInstructionCacheLine(std::uint32_t address) {
  sdk_idle_poll_pc_ = 0u;
  const std::uint32_t physical = AddressSpace::ToPhysical(address);
  if (physical >= kLowInstructionCacheSpan) return;
  const std::size_t line = physical / kLowInstructionCacheLineBytes;
  if (line < low_instruction_cache_valid_.size())
    low_instruction_cache_valid_[line] = false;
  native_low_vector_cache_.InvalidateLine(address);
}

void HostRuntime::CaptureIdleRegs() {
  auto* out = idle_loop_.regs.data();
  const auto* cpu = reinterpret_cast<const std::uint8_t*>(&cpu_);
  constexpr std::size_t kCore = offsetof(CPUState, timebase);
  constexpr std::size_t kSystem =
      offsetof(CPUState, program_exception) + sizeof(std::uint32_t) - offsetof(CPUState, sr);
  std::memcpy(out, cpu, kCore);
  std::memcpy(out + kCore, cpu + offsetof(CPUState, sr), kSystem);
  std::memcpy(out + kCore + kSystem, &cpu_.reserve_addr, sizeof(cpu_.reserve_addr));
  std::memcpy(out + kCore + kSystem + sizeof(cpu_.reserve_addr), &cpu_.reserve_valid,
              sizeof(cpu_.reserve_valid));
  idle_loop_.have_regs = true;
}

bool HostRuntime::IdleRegsMatch() const {
  if (!idle_loop_.have_regs) return false;
  const auto* in = idle_loop_.regs.data();
  const auto* cpu = reinterpret_cast<const std::uint8_t*>(&cpu_);
  constexpr std::size_t kCore = offsetof(CPUState, timebase);
  constexpr std::size_t kSystem =
      offsetof(CPUState, program_exception) + sizeof(std::uint32_t) - offsetof(CPUState, sr);
  return std::memcmp(in, cpu, kCore) == 0 &&
         std::memcmp(in + kCore, cpu + offsetof(CPUState, sr), kSystem) == 0 &&
         std::memcmp(in + kCore + kSystem, &cpu_.reserve_addr, sizeof(cpu_.reserve_addr)) == 0 &&
         std::memcmp(in + kCore + kSystem + sizeof(cpu_.reserve_addr), &cpu_.reserve_valid,
                     sizeof(cpu_.reserve_valid)) == 0;
}

void HostRuntime::SnapshotIdlePages() {
  constexpr std::size_t kPage = GuestWriteTracker::kPageBytes;
  idle_page_data_.resize(idle_pages_.size() * kPage);
  for (std::size_t i = 0; i < idle_pages_.size(); ++i)
    std::memcpy(idle_page_data_.data() + i * kPage,
                reinterpret_cast<const void*>(idle_pages_[i]), kPage);
}

void HostRuntime::RestartIdleLoop() {
  auto& s = idle_loop_;
  s.head_pc = cpu_.pc;
  s.boundaries = 0u;
  s.prev_boundaries = ~0u;
  s.sequence = 0xcbf29ce484222325ull;
  s.prev_sequence = 0u;
  s.stable = 0u;
  s.phase = 0u;
  s.have_regs = false;
}

void HostRuntime::FailIdleLoopProof() {
  auto& s = idle_loop_;
  s.stable = 0u;
  if (s.phase != 0u) {
    // An unproductive proof costs tracker scans; back off exponentially on
    // loops that repeat registers but keep changing memory.
    s.backoff = std::min<std::uint32_t>(4096u, std::max<std::uint32_t>(16u, s.backoff * 2u));
    s.cooldown = s.backoff;
  }
  s.phase = 0u;
}

// v274: called at every HostRuntime boundary (top of Run's loop).
void HostRuntime::ObserveIdleLoop(RunResult& result) {
  auto& s = idle_loop_;
  const std::uint32_t pc = cpu_.pc;
  if (pc != s.head_pc) {
    s.sequence = (s.sequence ^ pc) * 0x100000001b3ull;
    if (++s.boundaries > kIdleLoopMaxBoundaries) {
      RestartIdleLoop();
      s.effects = host_effects_ + hle_calls_;
      CaptureIdleRegs();
    }
    return;
  }

  // One iteration completed. It repeats the previous one exactly when it
  // crossed the same boundary PCs, called no host service, took no
  // exception/interrupt and returned with identical guest registers.
  const bool repeat = s.have_regs && s.boundaries == s.prev_boundaries &&
                      s.sequence == s.prev_sequence &&
                      host_effects_ + hle_calls_ == s.effects &&
                      cpu_.exception == 0u && fault_.kind == FaultKind::None &&
                      IdleRegsMatch();
  if (!repeat) {
    FailIdleLoopProof();
  } else if (s.cooldown != 0u) {
    --s.cooldown;
  } else if (s.phase == 0u) {
    if (++s.stable >= kIdleLoopStableIterations && (cpu_.msr & kPpcMsrEe) != 0u) {
      if (!guest_write_tracker_.Available()) {
        if (guest_write_tracker_.Enable({memory_.Mem1(), memory_.Mem2(),
                                         memory_.FakeVmemBacking()}))
          std::fprintf(stderr, "GEKKOAOT_IDLE_LOOP_PROOF_V274=1 tracker=%s\n",
                       guest_write_tracker_.BackendName());
        else
          idle_loop_proof_enabled_ = false;  // no exact proof on this host
      }
      if (idle_loop_proof_enabled_ && guest_write_tracker_.Reset()) s.phase = 1u;
    }
  } else if (s.phase == 1u) {
    // Learn the pages one iteration writes, then snapshot them: they are
    // the start state of the iteration that must prove the fixed point.
    idle_pages_.clear();
    if (!guest_write_tracker_.Collect(&idle_pages_, kIdleLoopMaxPages)) {
      FailIdleLoopProof();
    } else {
      SnapshotIdlePages();
      s.phase = 2u;
    }
  } else {
    idle_written_.clear();
    bool changed = !guest_write_tracker_.Collect(&idle_written_, kIdleLoopMaxPages);
    bool unknown = false;
    for (const auto page : idle_written_) {
      if (changed) break;
      const auto it = std::find(idle_pages_.begin(), idle_pages_.end(), page);
      if (it == idle_pages_.end()) { unknown = true; continue; }
      const auto index = static_cast<std::size_t>(it - idle_pages_.begin());
      changed = std::memcmp(reinterpret_cast<const void*>(page),
                            idle_page_data_.data() + index * GuestWriteTracker::kPageBytes,
                            GuestWriteTracker::kPageBytes) != 0;
    }
    if (changed) {
      FailIdleLoopProof();
    } else if (unknown) {
      // The write set grew (for example a callee touched a new stack page);
      // extend it and prove the next iteration instead.
      for (const auto page : idle_written_)
        if (std::find(idle_pages_.begin(), idle_pages_.end(), page) == idle_pages_.end() &&
            idle_pages_.size() < kIdleLoopMaxPages)
          idle_pages_.push_back(page);
      SnapshotIdlePages();
    } else {
      // Proven: registers, host state and every written page are unchanged,
      // so the next iteration would replay this one bit for bit. Sleep until
      // the next hardware/timer event (bounded by the maintenance cap).
      s.backoff = 0u;
      ++idle_loop_parks_;
      if (idle_loop_parks_ <= 8u)
        std::fprintf(stderr,
                     "GEKKOAOT_IDLE_LOOP_PROOF_V274=1 action=park head=%08x boundaries=%u pages=%zu sp=%08x parks=%llu\n",
                     s.head_pc, s.boundaries, idle_pages_.size(), cpu_.gpr[1],
                     static_cast<unsigned long long>(idle_loop_parks_));
      WaitForIdleEvent(result);
      // Writes made while parked (DMA, disk threads) belong to the next
      // verified iteration's start state, not to the loop.
      if (!guest_write_tracker_.Reset()) FailIdleLoopProof();
      else SnapshotIdlePages();
    }
  }

  s.prev_boundaries = s.boundaries;
  s.prev_sequence = s.sequence;
  s.boundaries = 0u;
  s.sequence = 0xcbf29ce484222325ull;
  s.effects = host_effects_ + hle_calls_;
  CaptureIdleRegs();
}

// v274: generated code sets MSR[EE] without leaving AOT execution unless this
// returns true. An interrupt can only be taken at that point if it is already
// latched: device state changes inside AOT code reach the host through MMIO and
// HLE callbacks, and wall-clock hardware time is sampled at dispatch
// boundaries, which keep their own budget cadence. NativeOS keeps the v17
// boundary on every enable because its idle/alarm services act at boundaries.
bool HostRuntime::EeEnableNeedsBoundary() {
  if (runtime_profile_enabled_) ++ee_enable_queries_;
  if (!ee_pending_query_enabled_ || compat_diagnostics_enabled_ || !os_hooks_.empty())
    return true;
  return pi_.InterruptPending() || decrementer_pending_;
}

// v274: OSDisableInterrupts/OSEnableInterrupts/OSRestoreInterrupts end with
// "mtmsr; rlwinm r3,...; blr". The MSR[EE] side exit returns to HostRuntime
// at the rlwinm, and re-entering the AOT module just to retire two register
// instructions costs a full chunk entry (state load/store) plus another
// boundary. Retire such tails here instead: at most four Rc=0 integer
// register instructions (rlwinm, ori, oris, addi, addis, or) followed by blr.
// Recognition uses instruction encodings only; no title or address database.
// Every word is decoded before any state changes, so a non-matching tail
// leaves the CPU untouched. Returns the retired instruction count, or 0.
std::uint32_t HostRuntime::TryExecuteLeafTail(std::uint32_t pc, bool execute) {
  constexpr std::uint32_t kMaxOps = 4u;
  constexpr std::uint32_t kBlr = 0x4e800020u;
  std::array<std::uint32_t, kMaxOps> ops{};
  std::uint32_t count = 0u;
  for (;;) {
    std::uint32_t word = 0u;
    if (!memory_.Read32(pc + 4u * count, &word)) return 0u;
    if (word == kBlr) break;
    if (count == kMaxOps) return 0u;
    const std::uint32_t opcode = word >> 26u;
    const bool supported =
        (opcode == 21u && (word & 1u) == 0u) ||               // rlwinm
        opcode == 24u || opcode == 25u ||                      // ori, oris
        opcode == 14u || opcode == 15u ||                      // addi, addis
        (opcode == 31u && ((word >> 1u) & 0x3ffu) == 444u &&  // or
         (word & 1u) == 0u);
    if (!supported) return 0u;
    ops[count++] = word;
  }
  if (count == 0u) return 0u;
  if (!execute) return count + 1u;

  auto& gpr = cpu_.gpr;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t w = ops[i];
    const std::uint32_t opcode = w >> 26u;
    const std::uint32_t rs = (w >> 21u) & 31u;  // rD for addi/addis
    const std::uint32_t ra = (w >> 16u) & 31u;
    const std::uint32_t imm = w & 0xffffu;
    switch (opcode) {
    case 21u: {
      const std::uint32_t sh = (w >> 11u) & 31u;
      const std::uint32_t mb = (w >> 6u) & 31u;
      const std::uint32_t me = (w >> 1u) & 31u;
      const std::uint32_t value = gpr[rs];
      const std::uint32_t rotated = sh == 0u ? value : ((value << sh) | (value >> (32u - sh)));
      const std::uint32_t head = 0xffffffffu >> mb;
      const std::uint32_t tail = 0xffffffffu << (31u - me);
      const std::uint32_t mask = mb <= me ? (head & tail) : (head | tail);
      gpr[ra] = rotated & mask;
      break;
    }
    case 24u: gpr[ra] = gpr[rs] | imm; break;
    case 25u: gpr[ra] = gpr[rs] | (imm << 16u); break;
    case 14u:
      gpr[rs] = (ra == 0u ? 0u : gpr[ra]) +
                static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(imm)));
      break;
    case 15u:
      gpr[rs] = (ra == 0u ? 0u : gpr[ra]) + (imm << 16u);
      break;
    default: {  // or rA,rS,rB
      const std::uint32_t rb = (w >> 11u) & 31u;
      gpr[ra] = gpr[rs] | gpr[rb];
      break;
    }
    }
  }
  cpu_.pc = cpu_.lr;
  return count + 1u;
}

bool HostRuntime::IcbiLineTouchesFixedImages(std::uint32_t address) {
  // Only the BAT-mapped MEM1 windows have a fixed EA identity. Physical-mode
  // or paged addresses (and the ICFI flash-invalidate request at EA 0) keep
  // the conservative full invalidation.
  const std::uint32_t window = address & 0xf0000000u;
  if (window != 0x80000000u && window != 0xc0000000u) return true;
  const std::uint32_t physical = AddressSpace::ToPhysical(address);
  if (physical >= AddressSpace::CompatMem1BackingSize) return true;
  const std::uint64_t start = 0x80000000ull | (physical & ~31u);
  const std::uint64_t end = start + 32u;
  const auto touches = [&](const GekkoAOTModuleDesc* descriptor) {
    if (!descriptor) return false;
    if (!descriptor->chunk_ranges || descriptor->num_chunk_ranges == 0u) return true;
    std::uint32_t lo = 0u;
    std::uint32_t hi = descriptor->num_chunk_ranges;
    while (lo < hi) {
      const std::uint32_t mid = lo + (hi - lo) / 2u;
      if (descriptor->chunk_ranges[mid].end <= start) lo = mid + 1u;
      else hi = mid;
    }
    return lo < descriptor->num_chunk_ranges && descriptor->chunk_ranges[lo].start < end;
  };
  if (touches(module_.Descriptor())) return true;
  for (const auto& side : secondary_modules_) {
    const auto* descriptor = side ? side->Descriptor() : nullptr;
    if (!descriptor) continue;
    // REL images run at OSLink-chosen heap addresses; their publication is
    // tracked through rel_mapping_dirty_, which the targeted path preserves
    // outside the exception-vector page.
    if (descriptor->num_rel_modules != 0u) {
      if (AddressSpace::ToPhysical(address) < kLowInstructionCacheSpan) continue;
      return true;
    }
    if (touches(descriptor)) return true;
  }
  return false;
}

void HostRuntime::InvalidateDispatchProbesForRange(std::uint32_t address,
                                                  std::uint32_t size) {
  ++native_read_poll_generation_;
  // v274: dispatch probes used to be flushed (128 KiB) on every icbi epoch.
  // A probe at pc decodes [pc-4, pc+24), so only probes whose window meets the
  // published lines can change; drop those under each MEM1 alias a dispatch
  // PC can use, plus every recorded SDK idle hit (its OSEnableInterrupts
  // callee lies elsewhere). Paged/physical addresses, ICFI (EA 0) and large
  // ranges keep the full flush.
  const std::uint32_t window = address & 0xf0000000u;
  const std::uint32_t physical = AddressSpace::ToPhysical(address);
  constexpr std::uint32_t kTargetedProbeRangeLimit = 4096u;
  if ((window != 0x80000000u && window != 0xc0000000u) || size == 0u ||
      size > kTargetedProbeRangeLimit ||
      physical >= AddressSpace::CompatMem1BackingSize) {
    ++dispatch_code_probe_generation_;
    sdk_idle_probe_pcs_.clear();
    return;
  }
  const std::uint32_t first = physical & ~31u;
  const std::uint32_t last = (physical + size + 31u) & ~31u;
  const std::uint32_t bases[] = {0x80000000u, 0xc0000000u, 0u};
  const auto drop = [this](std::uint32_t pc) {
    auto& probe = dispatch_code_probe_cache_[((pc >> 2u) ^ (pc >> 13u)) &
                                             (kDispatchCodeProbeCacheSize - 1u)];
    if (probe.pc == pc) probe = {};
  };
  for (const std::uint32_t base : bases)
    for (std::uint32_t pc = (base | first) - 20u; pc != (base | last) + 4u; pc += 4u)
      drop(pc);
  for (const std::uint32_t pc : sdk_idle_probe_pcs_) drop(pc);
  sdk_idle_probe_pcs_.clear();
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

bool HostRuntime::ExecuteNativeLowMemoryVector(std::uint32_t address,
                                              std::uint64_t* charged_cycles) {
  if (!native_low_vector_enabled_ || !charged_cycles ||
      (cpu_.msr & (kPpcMsrEe | kPpcMsrPr)) != 0u ||
      (spr_state_[kSprHid0] & kHid0IceMask) == 0u)
    return false;
  const auto physical = AddressSpace::ToPhysical(address);
  if (!IsLowExceptionVectorAddress(physical) || (physical & 0xffu) != 0u)
    return false;

  const NativeLowVector* proof = native_low_vector_cache_.Find(address);
  std::optional<NativeLowVector> decoded;
  if (!proof) {
    constexpr auto line_span = (NativeLowVector::ByteCount + kLowInstructionCacheLineBytes - 1u) &
                               ~(kLowInstructionCacheLineBytes - 1u);
    if (!memory_.Resolve(address, line_span)) return false;
    std::array<std::uint32_t, NativeLowVector::WordCount> words{};
    // Inspect exactly the currently visible instructions without filling any
    // untouched cache line. A failed proof/custom vector must retain the
    // interpreter's original fetch order and fault/SMC visibility.
    for (std::size_t i = 0u; i < words.size(); ++i) {
      const auto word_physical = physical + std::uint32_t(i) * 4u;
      const auto line = word_physical / kLowInstructionCacheLineBytes;
      if (low_instruction_cache_valid_[line])
        words[i] = low_instruction_cache_[word_physical / 4u];
      else if (!memory_.Read32(word_physical, &words[i]))
        return false;
      // Custom VM/DSI trampolines usually differ at their first opcode. Do
      // not inspect the other cache lines once the exact SDK proof fails.
      if (i == 0u && words[i] != 0x7c9043a6u) return false;
    }
    decoded = NativeLowVector::Decode(words);
    if (!decoded) return false;
    proof = &*decoded;
  }

  if (!proof->Execute(cpu_, memory_, address, spr_state_[272u], *charged_cycles))
    return false;
  if (decoded) {
    // Both accepted RI branches fetch all five cache lines. Context writes
    // cannot overlap them (checked by Execute), so materialize those same
    // lines only after successful admission/execution, then retain the proof.
    // The complete five-line aperture was prevalidated above.
    bool fetched = true;
    for (std::uint32_t offset = 0u; offset < NativeLowVector::ByteCount;
         offset += kLowInstructionCacheLineBytes) {
      std::uint32_t word = 0u;
      fetched = FetchLowMemoryInstruction(address + offset, &word) && fetched;
    }
    if (fetched) native_low_vector_cache_.Store(address, *decoded);
    ++native_low_vector_proofs_;
  }
  // Preserve diagnostics/idle-proof bookkeeping from 34 instructions plus
  // nine SPR transfers without re-entering each individual host service.
  if (track_host_effects_) host_effects_ += 43u;
  if (native_low_vector_executions_++ == 0u)
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_EXCEPTION_VECTOR_V294=1 accepted=%08x instructions=34 cycles=35 identity=exact-sdk-template fallback=original-interpreter\n",
                 address);
  return true;
}

bool HostRuntime::ExecuteLowMemoryInstruction(std::uint32_t address,
                                              std::uint64_t* charged_cycles) {
  CountHostEffect();
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
  if (physical == 0x0300u) NoteDsiTrace('A', cpu_.srr0, cpu_.exception);
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
    pe_notification_irq_active_ = false;
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
  if (!force_sample) {
    if (charged > std::numeric_limits<std::uint64_t>::max() -
                      hardware_clock_guest_since_sample_)
      hardware_clock_guest_since_sample_ = poll_quantum;
    else
      hardware_clock_guest_since_sample_ += charged;

    if (hardware_clock_started_ && hardware_clock_guest_since_sample_ < poll_quantum)
      return 0u;
  }
  // v158: idle/event waits call with charged=0 after a real wall-clock park.
  // "force_sample" must therefore actually sample the clock; the old code
  // merely selected a smaller guest-work quantum and could return zero forever
  // after sleeping, forcing NativeOS/SDK idle paths back into a busy loop.
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
    // v359: advance by whole periods so the cadence holds its exact rate
    // (rescheduling from `now` lost the sampling lateness every frame, e.g.
    // 59.3 Hz for 60). Never queue a burst after a host stall: resync instead.
    if (!host_present_started_ || now - host_present_next_ >= present_period)
      host_present_next_ = now + present_period;
    else
      host_present_next_ += present_period;
    host_present_started_ = true;
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

  {
    // v359: the in-game HUD shows these live, with this thread's CPU load
    // (the guest CPU runs here, so it is the emulation's critical thread).
    const double thread_seconds = GekkoAOT::GX::CurrentThreadCpuSeconds();
    float main_cpu = -1.f;
    if (perf_last_thread_seconds_ >= 0.0 && thread_seconds >= 0.0)
      main_cpu = static_cast<float>(std::clamp(
          100.0 * (thread_seconds - perf_last_thread_seconds_) / seconds, 0.0, 100.0));
    perf_last_thread_seconds_ = thread_seconds;
    const float values[] = {static_cast<float>(vps), static_cast<float>(nominal_vps),
                            static_cast<float>(guest_fps), static_cast<float>(speed), main_cpu,
                            static_cast<float>(host_fps_limit_hz_)};
    native_gx_.PublishPerf(values, static_cast<std::uint32_t>(std::size(values)));
  }
  std::fprintf(stderr,
               "GEKKOAOT_PERF_V49=1 vps=%.3f nominal_vps=%.3f guest_fps=%.3f "
               "present_fps=%.3f interp_fps=%.3f speed=%.2f target=%u drops=%llu\n",
               vps, nominal_vps, guest_fps, present_fps, interp_fps, speed,
               host_fps_limit_hz_, static_cast<unsigned long long>(dropped_delta));
  // v335: opt-in guest OS thread dump (GEKKOAOT_GUEST_THREAD_DUMP=1, every
  // 5 s): walks the SDK active-thread queue in guest RAM, so it also covers
  // titles whose OS runs as guest code (no NativeOS). Saved PC/LR, the stack
  // chain and the wait queue show where each thread is blocked.
  static const bool thread_dump = RuntimeEnvBool("GEKKOAOT_GUEST_THREAD_DUMP", false);
  static unsigned thread_dump_tick = 0u;
  if (thread_dump && ++thread_dump_tick % 5u == 0u) {
    std::uint32_t thread = 0u, current = 0u;
    memory_.Read32(0x800000E4u, &current);
    memory_.Read32(0x800000DCu, &thread);
    for (unsigned n = 0u; thread != 0u && n < 32u; ++n) {
      std::uint16_t state = 0u;
      std::uint32_t prio = 0u, suspend = 0u, queue = 0u, srr0 = 0u, lr = 0u, sp = 0u, next = 0u;
      memory_.Read16(thread + 0x2C8u, &state);
      memory_.Read32(thread + 0x2D0u, &prio);
      memory_.Read32(thread + 0x2CCu, &suspend);
      memory_.Read32(thread + 0x2DCu, &queue);
      memory_.Read32(thread + 0x198u, &srr0);
      memory_.Read32(thread + 0x084u, &lr);
      memory_.Read32(thread + 0x004u, &sp);
      std::uint32_t frames[2]{};
      for (auto& frame : frames) {
        std::uint32_t back = 0u, saved = 0u;
        if (!memory_.Read32(sp, &back) || !memory_.Read32(back + 4u, &saved)) break;
        frame = saved; sp = back;
      }
      std::fprintf(stderr, "GEKKOAOT_GUEST_THREAD_V335=1 thread=%08x%s state=%u prio=%d suspend=%d queue=%08x pc=%08x lr=%08x callers=%08x,%08x\n",
                   thread, thread == current ? "*" : "", state, static_cast<int>(prio),
                   static_cast<int>(suspend), queue, srr0, lr, frames[0], frames[1]);
      if (!memory_.Read32(thread + 0x2FCu, &next)) break;
      thread = next;
    }
  }
  // v335: interrupt delivery rates; a storm (an unacknowledged cause or an
  // OS that re-arms every boundary) shows up here as tens of thousands per second.
  if (irq_external_taken_ != irq_report_external_ || irq_decrementer_taken_ != irq_report_decrementer_) {
    std::fprintf(stderr, "GEKKOAOT_IRQ_RATE_V335=1 external_per_s=%.0f causes=%05x decrementer_per_s=%.0f "
                 "di=%.0f si=%.0f exi=%.0f ai=%.0f dsp=%.0f vi=%.0f pe_token=%.0f pe_finish=%.0f cp=%.0f fpscr=%08x\n",
                 static_cast<double>(irq_external_taken_ - irq_report_external_) / seconds,
                 irq_external_causes_,
                 static_cast<double>(irq_decrementer_taken_ - irq_report_decrementer_) / seconds,
                 irq_cause_counts_[2] / seconds, irq_cause_counts_[3] / seconds, irq_cause_counts_[4] / seconds,
                 irq_cause_counts_[5] / seconds, irq_cause_counts_[6] / seconds, irq_cause_counts_[8] / seconds,
                 irq_cause_counts_[9] / seconds, irq_cause_counts_[10] / seconds, irq_cause_counts_[11] / seconds,
                 cpu_.fpscr);
    irq_cause_counts_.fill(0u);
    irq_report_external_ = irq_external_taken_;
    irq_report_decrementer_ = irq_decrementer_taken_;
    irq_external_causes_ = 0u;
  }
  if (rate_throttles_ != rate_report_throttles_) {
    std::fprintf(stderr, "GEKKOAOT_GUEST_RATE_V333=1 throttles_per_s=%.0f parked=%.1f%% clock_spin_parks=%llu probes=%llu\n",
                 static_cast<double>(rate_throttles_ - rate_report_throttles_) / seconds,
                 100.0 * static_cast<double>(rate_throttle_ns_ - rate_report_ns_) / 1e9 / seconds,
                 static_cast<unsigned long long>(clock_spin_parks_),
                 static_cast<unsigned long long>(clock_spin_probes_));
    rate_report_throttles_ = rate_throttles_;
    rate_report_ns_ = rate_throttle_ns_;
  }
  // v274: host-boundary telemetry; every optimization that keeps guest code
  // native shows up here as fewer boundaries per second.
  const auto per_second = [&](std::uint64_t total, std::uint64_t& last) {
    const double rate = static_cast<double>(total - last) / seconds;
    last = total;
    return rate;
  };
  if (runtime_profile_enabled_) {
    native_gx_.ReportSyncProfile(seconds);
    std::fprintf(stderr,
                 "GEKKOAOT_IDLE_V300=1 idle_gx_busy=%.1f%% idle_gx_idle=%.1f%% gx_wakes_per_s=%.0f\n",
                 100.0 * static_cast<double>(idle_ns_gx_busy_) / 1e9 / seconds,
                 100.0 * static_cast<double>(idle_ns_gx_idle_) / 1e9 / seconds,
                 static_cast<double>(idle_gx_wakes_) / seconds);
    idle_ns_gx_busy_ = idle_ns_gx_idle_ = idle_gx_wakes_ = 0u;
    std::fprintf(stderr,
                 "GEKKOAOT_EXTERNAL_V300=1 reads_per_s vm=%.0f wgpipe=%.0f mmio=%.0f other=%.0f "
                 "writes_per_s vm=%.0f wgpipe=%.0f mmio=%.0f other=%.0f\n",
                 external_reads_[0] / seconds, external_reads_[1] / seconds,
                 external_reads_[2] / seconds, external_reads_[3] / seconds,
                 external_writes_[0] / seconds, external_writes_[1] / seconds,
                 external_writes_[2] / seconds, external_writes_[3] / seconds);
    external_reads_ = {};
    external_writes_ = {};
    if (!external_other_regions_.empty()) {
      std::vector<std::pair<std::uint64_t, std::uint32_t>> top;
      for (const auto& [key, count] : external_other_regions_) top.emplace_back(count, key);
      std::sort(top.begin(), top.end(), std::greater<>());
      std::string line;
      for (std::size_t i = 0; i < top.size() && i < 8u; ++i) {
        char item[48];
        std::snprintf(item, sizeof(item), " %08x%s=%.0f", top[i].second & 0xffff0000u,
                      (top[i].second & 1u) ? "w" : "r", static_cast<double>(top[i].first) / seconds);
        line += item;
      }
      std::fprintf(stderr, "GEKKOAOT_EXTERNAL_OTHER_V301=1 per_s%s\n", line.c_str());
      external_other_regions_.clear();
    }
  }
  if (runtime_profile_enabled_)
  std::fprintf(stderr,
               "GEKKOAOT_BOUNDARY_V274=1 boundaries_per_s=%.0f leaf_tails_per_s=%.0f "
               "hle_per_s=%.0f host_effects_per_s=%.0f idle_parks_per_s=%.1f guest_mhz=%.1f "
               "ee_queries_per_s=%.0f\n",
               per_second(boundaries_total_, perf_last_boundaries_),
               per_second(leaf_tail_executions_, perf_last_leaf_tails_),
               per_second(hle_calls_, perf_last_hle_calls_),
               per_second(host_effects_, perf_last_host_effects_),
               per_second(idle_loop_parks_, perf_last_idle_parks_),
               per_second(guest_cycles_total_, perf_last_guest_cycles_) / 1e6,
               per_second(ee_enable_queries_, perf_last_ee_queries_));
  if (runtime_profile_enabled_ && mmio_poll_parks_) {
    static std::uint64_t last_parks = 0, last_ns = 0;
    std::fprintf(stderr, "GEKKOAOT_MMIO_POLL_STATS_V347=1 parks_per_s=%.0f parked_ms_per_s=%.1f\n",
                 per_second(mmio_poll_parks_, last_parks), per_second(mmio_poll_park_ns_, last_ns) / 1e6);
    last_parks = mmio_poll_parks_; last_ns = mmio_poll_park_ns_;
  }
  if (runtime_profile_enabled_ && fixed_point_probes_) {
    static std::uint64_t last_parks = 0, last_probes = 0;
    static std::uint64_t last_ns = 0;
    std::fprintf(stderr, "GEKKOAOT_FIXED_POINT_STATS_V346=1 parks_per_s=%.0f probes_per_s=%.0f parked_ms_per_s=%.1f\n",
                 per_second(fixed_point_parks_, last_parks), per_second(fixed_point_probes_, last_probes),
                 per_second(fixed_point_park_ns_, last_ns) / 1e6);
    last_parks = fixed_point_parks_; last_probes = fixed_point_probes_; last_ns = fixed_point_park_ns_;
  }
  if (runtime_profile_enabled_ && (yield_spin_parks_ || yield_spin_declines_)) {
    static std::uint64_t last_parks = 0, last_declines = 0, last_ns = 0;
    std::fprintf(stderr, "GEKKOAOT_YIELD_SPIN_STATS_V344=1 parks_per_s=%.0f declines_per_s=%.0f parked_ms_per_s=%.1f\n",
                 per_second(yield_spin_parks_, last_parks), per_second(yield_spin_declines_, last_declines),
                 per_second(yield_spin_park_ns_, last_ns) / 1e6);
    last_parks = yield_spin_parks_; last_declines = yield_spin_declines_; last_ns = yield_spin_park_ns_;
  }
  if (runtime_profile_enabled_) {
    // v301: busiest native services (s<SimpleHleKind>, o<NativeOS::Kind>).
    std::vector<std::pair<std::uint64_t, std::string>> kinds;
    for (std::size_t i = 0; i < simple_hle_kind_calls_.size(); ++i)
      if (simple_hle_kind_calls_[i]) kinds.emplace_back(simple_hle_kind_calls_[i], "s" + std::to_string(i));
    for (std::size_t i = 0; i < os_hle_kind_calls_.size(); ++i)
      if (os_hle_kind_calls_[i]) kinds.emplace_back(os_hle_kind_calls_[i], "o" + std::to_string(i));
    std::sort(kinds.begin(), kinds.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string line;
    for (std::size_t i = 0; i < kinds.size() && i < 10u; ++i) {
      char item[48];
      std::snprintf(item, sizeof(item), " %s=%.0f", kinds[i].second.c_str(),
                    static_cast<double>(kinds[i].first) / seconds);
      line += item;
    }
    std::fprintf(stderr, "GEKKOAOT_HLE_KINDS_V301=1 guest_declines=%llu per_s%s\n",
                 static_cast<unsigned long long>(hle_guest_declines_), line.c_str());
    std::fprintf(stderr,"GEKKOAOT_NATIVE_SDK_PACKETS_V306=1 packets_per_s=%.0f mmio_callbacks=0\n",
                 static_cast<double>(native_sdk_gx_packets_)/seconds);
    native_sdk_gx_packets_=0u;
    simple_hle_kind_calls_ = {};
    os_hle_kind_calls_ = {};
  }
  if (boundary_profile_enabled_ && !boundary_profile_.empty()) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> top(boundary_profile_.begin(),
                                                             boundary_profile_.end());
    const std::size_t count = std::min<std::size_t>(top.size(), 64u);
    std::partial_sort(top.begin(), top.begin() + static_cast<std::ptrdiff_t>(count), top.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
    for (std::size_t i = 0; i < count; ++i)
      std::fprintf(stderr, "GEKKOAOT_BOUNDARY_PROFILE_V274=1 rank=%zu pc=%08x lr=%08x per_s=%.0f\n",
                   i + 1u, static_cast<unsigned>(top[i].first >> 32u),
                   static_cast<unsigned>(top[i].first),
                   static_cast<double>(top[i].second) / seconds);
    boundary_profile_.clear();
  }

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
      GekkoAOT::HW::VI::NativeVI::CpuClockHz / 15u;
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
  bool pause_requested = false;
  std::uint32_t next_fps = host_fps_limit_hz_;
  auto next_graphics = live_graphics_settings_;
  while (std::getline(file, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto equals = line.find('=');
    if (equals != std::string::npos) {
      const auto key = line.substr(0, equals), value = line.substr(equals + 1);
      if (key == "paused") pause_requested = value == "true" || value == "1";
      GX::ParseGraphicsSetting(next_graphics, key, value);
      if (key == "fullscreen") next_graphics.fullscreen = value == "true" || value == "1";
      if (key == "resolution") {
        unsigned width = 0, height = 0; char extra = 0;
        if (std::sscanf(value.c_str(), "%ux%u%c", &width, &height, &extra) == 2 &&
            width >= 320 && width <= 7680 && height >= 240 && height <= 4320) {
          next_graphics.output_width = width; next_graphics.output_height = height;
        }
      }
    }
    constexpr std::string_view fps_prefix = "fps_limit=";
    constexpr std::string_view aspect_prefix = "aspect_ratio=";
    if (line.starts_with(fps_prefix)) {
      char* end = nullptr;
      const auto value = std::strtoul(line.c_str() + fps_prefix.size(), &end, 10);
      if (end && *end == '\0' && value >= 1u && value <= 1000u) {
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
  if (native_gx_.ApplyGraphicsSettings(next_graphics)) {
    live_graphics_settings_ = next_graphics;
    const bool interpolation = next_graphics.interpolation && native_gx_.HasFrameInterpolation();
    if (interpolation != frame_interpolation_enabled_) {
      frame_interpolation_enabled_ = interpolation;
      native_gx_.SetFrameReadyFence(interpolation);
      interpolation_pair_valid_ = false;
      host_present_started_ = false;
    }
  }
  if (pause_requested) {
    // Stop at the existing host-service boundary, never in the middle of an
    // AOT block or a hardware operation. All guest clocks remain frozen.
    native_gx_.SetPaused(true);
    dsp_.FlushAudioSink();
    std::fprintf(stderr, "GEKKOAOT_PAUSE state=paused pc=%08x tb=%llu\n",
                 cpu_.pc, static_cast<unsigned long long>(cpu_.timebase));
    while (pause_requested && !native_gx_.QuitRequested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      native_gx_.PollEvents();
      std::ifstream control(live_video_config_path_);
      if (!control) continue;
      // QSaveFile atomically replaces the file, so no partial state is read.
      while (std::getline(control, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.starts_with("paused=")) {
          const auto value = line.substr(7);
          pause_requested = value == "true" || value == "1";
        }
      }
    }
    native_gx_.SetPaused(false);
    dsp_.FlushAudioSink();
    hardware_clock_started_ = false;
    hardware_clock_guest_since_sample_ = 0;
    host_present_started_ = false;
    interpolation_pair_valid_ = false;
    perf_metrics_started_ = false;
    std::fprintf(stderr, "GEKKOAOT_PAUSE state=running pc=%08x tb=%llu\n",
                 cpu_.pc, static_cast<unsigned long long>(cpu_.timebase));
  }
}

void HostRuntime::ServiceWidescreenFrustumAssist(bool frame_boundary) {
  if (!frame_boundary || !widescreen_frustum_assist_enabled_) return;

  constexpr float kNativeAspect = 4.0f / 3.0f;
  constexpr std::uint32_t kMem1Base = 0x80000000u;
  constexpr std::size_t kAutoScanBegin = 0x00200000u;
  constexpr std::size_t kAutoScanTailGuard = 0x00200000u;
  constexpr std::size_t kAutoScanChunk = 0x00080000u;  // 512 KiB / VI frame.
  constexpr std::uint32_t kAutoConfirmFrames = 45u;
  constexpr std::uint32_t kAutoRetryFrames = 180u;

  ++widescreen_frustum_scan_frames_;

  const auto read_guest_float = [&](std::uint32_t address, float* out) -> bool {
    if (!out) return false;
    const auto* p = memory_.Resolve(address, sizeof(float));
    if (!p) return false;
    *out = GekkoAOT::GX::ReadBigEndianFloat(p);
    return std::isfinite(*out);
  };
  const auto write_guest_float = [&](std::uint32_t address, float value) -> bool {
    auto* p = memory_.Resolve(address, sizeof(float));
    if (!p || !std::isfinite(value)) return false;
    GekkoAOT::GX::WriteBigEndianFloat(p, value);
    return true;
  };
  const auto clear_auto_scan = [&] {
    widescreen_frustum_scan_offset_ = 0u;
    widescreen_frustum_scan_candidate_count_ = 0u;
    widescreen_frustum_scan_candidate_aspect_ = 0u;
    widescreen_frustum_scan_candidate_fov_ = 0u;
  };
  const auto clear_pending = [&] {
    widescreen_frustum_pending_aspect_address_ = 0u;
    widescreen_frustum_pending_fov_address_ = 0u;
    widescreen_frustum_pending_confirm_frames_ = 0u;
  };
  const auto invalidate_auto_selection = [&] {
    widescreen_frustum_aspect_address_ = 0u;
    widescreen_frustum_fov_address_ = 0u;
    widescreen_frustum_selected_logged_ = false;
    widescreen_frustum_next_scan_frame_ =
        widescreen_frustum_scan_frames_ + kAutoRetryFrames;
  };

  const bool wide = native_gx_.WidescreenActive();
  const float target_aspect = native_gx_.WidescreenTargetAspect();

  // Restore exactly the value observed before the assist modified it.  Do not
  // assume every title's native camera scalar is bit-identical to 4/3.
  if (!wide || !std::isfinite(target_aspect) ||
      target_aspect <= kNativeAspect * 1.005f) {
    if (widescreen_frustum_aspect_address_ != 0u) {
      float current = 0.0f;
      if (read_guest_float(widescreen_frustum_aspect_address_, &current) &&
          std::fabs(current - widescreen_frustum_last_target_aspect_) <= 0.002f &&
          write_guest_float(widescreen_frustum_aspect_address_,
                            widescreen_frustum_original_aspect_)) {
        std::fprintf(stderr,
                     "GEKKOAOT_WIDESCREEN_FRUSTUM_V177 action=restore address=0x%08x aspect=%.6f\n",
                     widescreen_frustum_aspect_address_,
                     widescreen_frustum_original_aspect_);
      }
    }
    widescreen_frustum_last_target_aspect_ = kNativeAspect;
    return;
  }
  widescreen_frustum_last_target_aspect_ = target_aspect;

  float observed_fov = 0.0f;
  float observed_aspect = 0.0f;
  const bool have_camera =
      native_gx_.CurrentPerspectiveCamera(&observed_fov, &observed_aspect) &&
      GekkoAOT::GX::LooksLikeCameraFov(observed_fov) &&
      GekkoAOT::GX::LooksLikeNativeAspect(observed_aspect);
  if (have_camera) {
    widescreen_frustum_observed_fov_ = observed_fov;
    widescreen_frustum_observed_aspect_ = observed_aspect;
  }

  // Explicit override remains an escape hatch, but is not part of automatic
  // detection and therefore does not introduce a per-game rule in the runtime.
  if (widescreen_frustum_aspect_address_ == 0u &&
      widescreen_frustum_pending_aspect_address_ == 0u) {
    if (const char* configured =
            std::getenv("GEKKOAOT_WIDESCREEN_FRUSTUM_ASPECT_ADDR");
        configured && *configured) {
      char* endp = nullptr;
      const unsigned long value = std::strtoul(configured, &endp, 0);
      if (endp != configured && endp && *endp == '\0' &&
          value <= std::numeric_limits<std::uint32_t>::max() &&
          memory_.Resolve(static_cast<std::uint32_t>(value), sizeof(float))) {
        widescreen_frustum_aspect_address_ = static_cast<std::uint32_t>(value);
        widescreen_frustum_manual_ = true;
        float original = kNativeAspect;
        if (read_guest_float(widescreen_frustum_aspect_address_, &original))
          widescreen_frustum_original_aspect_ = original;
        clear_auto_scan();
        std::fprintf(stderr,
                     "GEKKOAOT_WIDESCREEN_FRUSTUM_V177 source=manual address=0x%08x original=%.6f target=%.6f\n",
                     widescreen_frustum_aspect_address_,
                     widescreen_frustum_original_aspect_, target_aspect);
      }
    }
  }

  // Automatic path: Aurora supplies the actual perspective projection consumed
  // by GX.  Search incrementally for a *unique* adjacent RAM pair matching that
  // live FOV and aspect.  This removes per-title address rules and rejects
  // unrelated 1.333333/FOV-looking constants elsewhere in MEM1.
  if (!widescreen_frustum_manual_ && have_camera &&
      widescreen_frustum_aspect_address_ == 0u &&
      widescreen_frustum_pending_aspect_address_ == 0u &&
      widescreen_frustum_scan_frames_ >= widescreen_frustum_next_scan_frame_) {
    const std::size_t reported =
        std::min<std::size_t>(AddressSpace::ReportedMem1Size(), memory_.Mem1().size());
    const std::size_t scan_end =
        reported > kAutoScanTailGuard ? reported - kAutoScanTailGuard : reported;

    if (scan_end > kAutoScanBegin + 8u) {
      if (widescreen_frustum_scan_offset_ == 0u)
        widescreen_frustum_scan_offset_ = static_cast<std::uint32_t>(kAutoScanBegin);

      const std::size_t begin = widescreen_frustum_scan_offset_;
      const std::size_t end = std::min(scan_end, begin + kAutoScanChunk + 4u);
      const auto candidates = GekkoAOT::GX::FindProjectionCorrelatedCandidates(
          memory_.Mem1(), kMem1Base, begin, end, observed_fov, observed_aspect, 8u);

      for (const auto& candidate : candidates) {
        if (widescreen_frustum_scan_candidate_count_ == 0u) {
          widescreen_frustum_scan_candidate_aspect_ = candidate.aspect_address;
          widescreen_frustum_scan_candidate_fov_ = candidate.fov_address;
        }
        if (widescreen_frustum_scan_candidate_count_ < 2u)
          ++widescreen_frustum_scan_candidate_count_;
      }

      const std::size_t next = begin + kAutoScanChunk;
      const bool finished = next >= scan_end ||
                            widescreen_frustum_scan_candidate_count_ > 1u;
      if (!finished) {
        widescreen_frustum_scan_offset_ = static_cast<std::uint32_t>(next);
      } else {
        if (widescreen_frustum_scan_candidate_count_ == 1u) {
          widescreen_frustum_pending_aspect_address_ =
              widescreen_frustum_scan_candidate_aspect_;
          widescreen_frustum_pending_fov_address_ =
              widescreen_frustum_scan_candidate_fov_;
          widescreen_frustum_pending_confirm_frames_ = 0u;
          widescreen_frustum_ambiguous_logged_ = false;
          std::fprintf(stderr,
                       "GEKKOAOT_WIDESCREEN_FRUSTUM_V177 source=projection phase=confirm address=0x%08x fov-address=0x%08x gx-fov=%.3f gx-aspect=%.6f frames=%u\n",
                       widescreen_frustum_pending_aspect_address_,
                       widescreen_frustum_pending_fov_address_, observed_fov,
                       observed_aspect, kAutoConfirmFrames);
        } else if (!widescreen_frustum_ambiguous_logged_) {
          std::fprintf(stderr,
                       "GEKKOAOT_WIDESCREEN_FRUSTUM_V177 source=projection phase=scan candidates=%u action=no-patch reason=%s gx-fov=%.3f gx-aspect=%.6f\n",
                       widescreen_frustum_scan_candidate_count_,
                       widescreen_frustum_scan_candidate_count_ == 0u ? "none" : "ambiguous",
                       observed_fov, observed_aspect);
          widescreen_frustum_ambiguous_logged_ = true;
        }
        clear_auto_scan();
        widescreen_frustum_next_scan_frame_ =
            widescreen_frustum_scan_frames_ + kAutoRetryFrames;
      }
    }
  }

  // Candidate must track the live GX camera for 45 frames.  This is much
  // stronger evidence than merely remaining in a plausible numeric range.
  if (!widescreen_frustum_manual_ &&
      widescreen_frustum_aspect_address_ == 0u &&
      widescreen_frustum_pending_aspect_address_ != 0u) {
    float aspect = 0.0f;
    float fov = 0.0f;
    const bool valid = have_camera &&
        read_guest_float(widescreen_frustum_pending_aspect_address_, &aspect) &&
        read_guest_float(widescreen_frustum_pending_fov_address_, &fov) &&
        GekkoAOT::GX::LooksLikeAspect(aspect, observed_aspect) &&
        GekkoAOT::GX::MatchesObservedFov(fov, observed_fov);
    if (!valid) {
      clear_pending();
      widescreen_frustum_next_scan_frame_ =
          widescreen_frustum_scan_frames_ + kAutoRetryFrames;
    } else if (++widescreen_frustum_pending_confirm_frames_ >= kAutoConfirmFrames) {
      widescreen_frustum_aspect_address_ =
          widescreen_frustum_pending_aspect_address_;
      widescreen_frustum_fov_address_ = widescreen_frustum_pending_fov_address_;
      widescreen_frustum_original_aspect_ = aspect;
      clear_pending();
      widescreen_frustum_selected_logged_ = false;
    }
  }

  if (widescreen_frustum_aspect_address_ == 0u) return;

  float current_aspect = 0.0f;
  if (!read_guest_float(widescreen_frustum_aspect_address_, &current_aspect)) return;

  const bool original_aspect = GekkoAOT::GX::LooksLikeAspect(
      current_aspect, widescreen_frustum_original_aspect_, 0.0040f);
  const bool already_target = std::fabs(current_aspect - target_aspect) <= 0.002f;
  if (!original_aspect && !already_target) {
    if (!widescreen_frustum_manual_) invalidate_auto_selection();
    return;
  }

  if (original_aspect &&
      !write_guest_float(widescreen_frustum_aspect_address_, target_aspect))
    return;

  if (!widescreen_frustum_selected_logged_) {
    float fov = 0.0f;
    const bool have_fov = widescreen_frustum_fov_address_ != 0u &&
                          read_guest_float(widescreen_frustum_fov_address_, &fov);
    std::fprintf(stderr,
                 "GEKKOAOT_WIDESCREEN_FRUSTUM_V177 source=%s action=patch address=0x%08x original=%.6f target=%.6f fov-address=0x%08x fov=%s%.3f gx-fov=%.3f gx-aspect=%.6f hardcode=none\n",
                 widescreen_frustum_manual_ ? "manual" : "projection",
                 widescreen_frustum_aspect_address_,
                 widescreen_frustum_original_aspect_, target_aspect,
                 widescreen_frustum_fov_address_, have_fov ? "" : "n/a:",
                 have_fov ? fov : 0.0f, widescreen_frustum_observed_fov_,
                 widescreen_frustum_observed_aspect_);
    widescreen_frustum_selected_logged_ = true;
  }
}

void HostRuntime::ServiceDiscCompletion(std::uint64_t cycles) {
  if (di_async_completion_pending_) {
    // A guest may cancel/break the DI command before the deferred completion.
    // NativeDI clears TSTART in that case; drop the scheduled TCINT instead of
    // resurrecting a cancelled command.
    if ((di_.DMAControl() & 1u) == 0u || di_.CommandSequence() != di_async_sequence_) {
      static unsigned cancel_logs = 0u;
      if (EnvTraceDvd() && cancel_logs++ < 32u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_DI_ASYNC_V58=1 phase=cancel seq=%llu offset=%08llx dma=%08x bytes=%u\n",
                     static_cast<unsigned long long>(di_async_sequence_),
                     static_cast<unsigned long long>(di_async_disc_offset_),
                     di_async_dma_address_, di_async_bytes_);
      di_async_completion_pending_ = false;
      di_async_cycles_remaining_ = 0u;
      if (di_io_inflight_) QuiesceDiscIo();
      di_async_staging_.clear();
    } else if (cycles >= di_async_cycles_remaining_) {
      if (di_io_inflight_) {
        if (!di_io_future_.valid() ||
            di_io_future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
          // Guest drive time has expired, but host I/O is still running. Keep
          // TSTART asserted and poll on the next hardware slice; never block the
          // AOT CPU worker waiting for disk/decompression.
          di_async_cycles_remaining_ = 0u;
          return;
        }
        DiscIoResult io = di_io_future_.get();
        di_io_inflight_ = false;
        if (io.ok) di_async_staging_ = std::move(io.bytes);
        else di_async_staging_.clear();
      }
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
          native_gx_.NotifyMemoryWrite(dma, bytes);
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
      if (EnvTraceDvd() && complete_logs++ < 256u) {
        std::uint64_t hash = 0xcbf29ce484222325ull;
        if (const auto* target = memory_.Resolve(dma, bytes))
          for (std::uint32_t i = 0; i < bytes; ++i) hash = (hash ^ target[i]) * 0x100000001b3ull;
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_DI_PARITY_V143=1 phase=%s seq=%llu offset=%08llx dma=%08x bytes=%u status=%08x fnv=%016llx pc=%08x visibility=completion\n",
                     completed ? "complete" : (published ? "stale-drop" : "dma-fault"),
                     static_cast<unsigned long long>(sequence),
                     static_cast<unsigned long long>(offset), dma, bytes, di_.Status(),
                     static_cast<unsigned long long>(hash), cpu_.pc);
      }
    } else {
      di_async_cycles_remaining_ -= cycles;
    }
  }
}

void HostRuntime::AdvanceRuntimeCycles(std::uint64_t charged, RunResult& result,
                                       bool idle_wait) {
  if (charged > std::numeric_limits<std::uint64_t>::max() - result.cycles)
    result.cycles = std::numeric_limits<std::uint64_t>::max();
  else
    result.cycles += charged;
  if (runtime_profile_enabled_ && !idle_wait) guest_cycles_total_ += charged;
  if (!idle_wait) rate_charged_ += charged;

  const std::uint64_t hardware_cycles =
      TakeRealtimeHardwareCycles(charged, idle_wait);
  perf_hardware_cycles_total_ += hardware_cycles;

  // The async GX parser publishes ordered PE token/finish events through a
  // bounded queue.  Commit them on the CPU thread before a zero-time fast
  // return so interrupt latency is bounded by AOT dispatch, not wall-clock
  // sampling. PeEventPending() is one atomic load on the no-event
  // path.
  if (runtime_parallel_workers_ && native_gx_.PeEventPending())
    SyncNativeGXInterrupts();

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
  if (cp_.RegisterDrainPending()) {
    cp_.ServiceRegisterDrain();
    SyncNativeCPInterrupt();
  }
  ServiceDiscCompletion(hardware_cycles);

  PollLiveVideoConfig(hardware_cycles);
  AdvanceCpuTimers(hardware_cycles);
  si_.AdvanceCycles(hardware_cycles);
  SyncNativeSIInterrupt();
  vi_.AdvanceCycles(hardware_cycles);
  const bool vi_interrupt_update = vi_.ConsumeInterruptUpdate();
  if (vi_interrupt_update)
    SyncNativeVIInterrupt();

  odd_field_boundary = vi_.ConsumeOddFieldBoundary();
  even_field_boundary = vi_.ConsumeEvenFieldBoundary();
  if (runtime_profile_enabled_ && (odd_field_boundary || even_field_boundary)) {
    // v300 diagnostic: GX display copies produced during each VI field.
    static std::string pattern;
    static std::uint64_t last_frames = 0u;
    const auto produced = perf_guest_frames_total_ - last_frames;
    last_frames = perf_guest_frames_total_;
    pattern.push_back(static_cast<char>('0' + std::min<std::uint64_t>(produced, 9u)));
    if (pattern.size() >= 60u) {
      std::fprintf(stderr, "GEKKOAOT_FIELD_COPIES_V300=1 %s\n", pattern.c_str());
      pattern.clear();
    }
  }
  frame_boundary = vi_.ConsumeFrameBoundary();
  if (frame_boundary) {
    ++perf_vi_boundaries_total_;
    ServiceWidescreenFrustumAssist(true);
  }

  // v160: only after a complete VI frame has elapsed can an already-handled
  // CP breakpoint be classified as a scheduler/video-flip deadlock.
  ServiceIdleCpBreakpointRecovery(frame_boundary);

  // NativeHW v4: SI automatic polling is a real hardware-time producer, not
  // merely an optimization fallback. v3 disabled this when NativeHW SI was on,
  // freezing IN_HI/IN_LO for games that consume automatic SIPOLL samples. Keep
  // one 120 Hz host snapshot + SI latch in both modes, while explicit COMCSR
  // transfers retain their <=1 ms low-latency refresh. Missed host samples are
  // collapsed to one fresh snapshot after a stall.
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
  if (GekkoAOT::NativeHW::SI::NeedsPeriodicHostPoll(
          native_hw_.SI(), native_input_ready_)) {
    constexpr std::uint64_t kNativeInputPollCycles =
        GekkoAOT::NativeHW::SI::PollIntervalCycles(
            GekkoAOT::HW::VI::NativeVI::CpuClockHz, 120u);
    native_input_hw_phase_ += hardware_cycles;
    if (native_input_hw_phase_ >= kNativeInputPollCycles) {
      native_input_hw_phase_ %= kNativeInputPollCycles;
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
  const auto queue_vi_frame = [&](std::uint32_t top, std::uint32_t bottom) {
    if (!GekkoAOT::NativeHW::VI::ShouldPresent(
            native_hw_.VI(), perf_guest_frames_total_, native_vi_last_queued_frame_))
      return;
    // Include VI geometry for RAM-owned scanout. Its pixels and selected field
    // can change without a new GX copy; the renderer can dedupe cached GX data.
    QueueHostPresent(top, bottom, vi_.XfbWidthPixels(), vi_.XfbStrideBytes(),
                     vi_.XfbFieldHeight());
    if (native_hw_.VI()) native_vi_last_queued_frame_ = perf_guest_frames_total_;
  };
  if (gx_ready && !gx_present_on_copy_) {
    const std::uint32_t top = vi_.XfbAddressTop();
    const std::uint32_t bottom = vi_.XfbAddressBottom();
    const bool split_fields = top != 0u && bottom != 0u && top != bottom;

    if (split_fields) {
      if (odd_field_boundary) {
        queue_vi_frame(top, top);
        static unsigned odd_logs = 0;
        if (odd_logs++ < 32u)
          std::fprintf(stderr,
                       "GEKKOAOT_NATIVE_VI_FIELD_SCANOUT_V50=1 field=odd xfb=%08x top=%08x bottom=%08x\n",
                       top, top, bottom);
      }
      if (even_field_boundary) {
        queue_vi_frame(bottom, bottom);
        static unsigned even_logs = 0;
        if (even_logs++ < 32u)
          std::fprintf(stderr,
                       "GEKKOAOT_NATIVE_VI_FIELD_SCANOUT_V50=1 field=even xfb=%08x top=%08x bottom=%08x\n",
                       bottom, top, bottom);
      }
    } else if (frame_boundary) {
      const std::uint32_t xfb = top != 0u ? top : bottom;
      if (xfb != 0u) {
        queue_vi_frame(xfb, xfb);
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
  const std::uint32_t effective_aid_hz = native_hw_.AI()
      ? GekkoAOT::NativeHW::AI::EffectiveAidRate(true, guest_aid_hz, native_32k_mixer)
      : (native_32k_mixer ? 32028u : guest_aid_hz);
  if (native_audio_effective_hz_ != effective_aid_hz) {
    dsp_.SetAudioSampleRateHz(effective_aid_hz);
    native_audio_effective_hz_ = effective_aid_hz;
  }
  {
    static std::uint32_t last_guest_hz = 0u;
    static std::uint32_t last_effective_hz = 0u;
    if (guest_aid_hz != last_guest_hz || effective_aid_hz != last_effective_hz) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_AUDIO_RATE_V69 guest_aid_hz=%u effective_aid_hz=%u "
                   "ax_hle=%u jaudio_hle=%u native_hw_ai=%u native_hw_dsp=%u policy=%s\n",
                   guest_aid_hz, effective_aid_hz, dsp_.NativeAxHleActive() ? 1u : 0u,
                   dsp_.NativeJAudioHleActive() ? 1u : 0u, native_hw_.AI() ? 1u : 0u,
                   GekkoAOT::NativeHW::DSP::HostTaskBackendActive(
                       native_hw_.DSP(), dsp_.NativeAxHleActive(), dsp_.NativeJAudioHleActive()) ? 1u : 0u,
                   dsp_.NativeAxHleActive() ? "ax-fixed-32k"
                                            : (dsp_.NativeJAudioHleActive() ? "jaudio-fixed-32k" : "guest-ai"));
      last_guest_hz = guest_aid_hz;
      last_effective_hz = effective_aid_hz;
    }
  }
  dsp_.AdvanceCycles(hardware_cycles);
  ai_.AdvanceCycles(hardware_cycles);

  // NativeHW v3: AI/DSP MMIO writes synchronize PI immediately, so after a
  // time advance the only interesting transition is an interrupt that actually
  // became pending. Avoid the old 256-cycle "is it pending yet?" polling when
  // the corresponding NativeHW service is enabled. Legacy mode keeps the exact
  // previous quantum for A/B compatibility.
  const bool native_dsp_irq_due = GekkoAOT::NativeHW::DSP::TimedIrqNeedsSync(
      native_hw_.DSP(), dsp_.InterruptPending());
  const bool native_ai_irq_due = GekkoAOT::NativeHW::AI::TimedIrqNeedsSync(
      native_hw_.AI(), ai_.InterruptPending());
  if (native_dsp_irq_due) SyncNativeDSPInterrupt();
  if (native_ai_irq_due) SyncNativeAIInterrupt();

  if (!native_hw_.DSP() || !native_hw_.AI()) {
    constexpr std::uint64_t kTimedIrqSyncQuantum = 256u;
    timed_irq_sync_cycles_ += hardware_cycles;
    if (timed_irq_sync_cycles_ >= kTimedIrqSyncQuantum || frame_boundary) {
      timed_irq_sync_cycles_ %= kTimedIrqSyncQuantum;
      if (!native_hw_.DSP()) SyncNativeDSPInterrupt();
      if (!native_hw_.AI()) SyncNativeAIInterrupt();
    }
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
  std::fprintf(stderr,
               "GEKKOAOT_NATIVE_PPCHALT_OVERLAY_GUARD_V214=1 policy=live-signature-revalidate-on-index-hit\n");
}

bool HostRuntime::DetectSdkPpcHalt(std::uint32_t pc, std::uint32_t* entry) const {
  // PPCHalt is static SDK code. Pre-index its five PCs once when the DOL is
  // loaded instead of hashing and reading guest RAM at every dispatch boundary.
  if (ppc_halt_pcs_.empty() || pc < ppc_halt_pcs_.front().pc ||
      pc > ppc_halt_pcs_.back().pc) return false;
  const auto it=std::lower_bound(ppc_halt_pcs_.begin(),ppc_halt_pcs_.end(),pc,
      [](const PpcHaltPc& item,std::uint32_t value){return item.pc<value;});
  if(it==ppc_halt_pcs_.end()||it->pc!=pc) return false;

  // v214: a fixed secondary DOL/ELF may overlay the main executable at the
  // exact same guest addresses.  The PPCHalt index above intentionally belongs
  // to the boot/main DOL, so an address-only hit is not sufficient after an
  // overlay has been published.  Medal of Honor: Frontline is a concrete retail
  // case: main.dol has PPCHalt at 0x80683744 while Moh2StubRelGC.elf has
  // PPCMthid2 (mtspr HID2,r3) at 0x80683744.  Stopping on the stale main-DOL
  // index prevents the mission-return stub from restoring hardware state.
  //
  // Candidate hits are extremely rare, so revalidate the five live words here
  // rather than paying guest-memory reads at every dispatch boundary.  This is
  // also self-modifying/overlay safe: only the executable bytes currently
  // visible in MEM1 can be classified as PPCHalt.
  static constexpr std::array<std::uint32_t,5> kPattern={
      0x7c0004acu,0x60000000u,0x38600000u,0x60000000u,0x4bfffff4u,
  };
  for(std::uint32_t i=0;i<kPattern.size();++i) {
    std::uint32_t word=0u;
    if(!memory_.Read32(it->entry+i*4u,&word)||word!=kPattern[i]) {
      static unsigned overlay_guard_logs=0u;
      if(overlay_guard_logs++<16u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_PPCHALT_OVERLAY_GUARD_V214=1 pc=%08x indexed_entry=%08x mismatch_offset=%u live=%08x expected=%08x action=bypass-stale-index\n",
                     pc,it->entry,i*4u,word,kPattern[i]);
      return false;
    }
  }

  if(entry) *entry=it->entry;
  return true;
}

void HostRuntime::RefreshSecondaryRelMappings() {
  // v156: this used to scan the whole 24 MiB MEM1 every time a secondary
  // address missed active_rel_sections_.  On titles with canonical REL calls
  // (notably SMB) that made ResolveSecondaryAddress consume essentially an
  // entire CPU core while the game appeared stuck.  A linked REL cannot be
  // executed correctly until its relocated instructions are published through
  // icbi/ICInvalidateRange, so CacheControl() marks this index dirty at that
  // architectural boundary.  Between publication epochs the mapping is stable.
  if (!rel_mapping_dirty_) return;
  cached_rel_route_ = {};

  ++rel_mapping_refreshes_;
  std::vector<ActiveRelSection> discovered;
  const auto& ram = memory_.Mem1();
  if (ram.size() < 0x40u) {
    active_rel_sections_.clear();
    rel_mapping_dirty_ = false;
    return;
  }

  struct PendingRel {
    const GekkoAOTModuleDesc* descriptor = nullptr;
    const GekkoAOTRelModule* module = nullptr;
    bool found = false;
    const GekkoAOTRelImportProof* proofs = nullptr;
    std::uint32_t proof_count = 0;
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
      std::uint32_t proof_count = 0;
      const auto* proofs = library->RelImportProofs(module.module_id, &proof_count);
      pending.push_back({descriptor, &module, false, proofs, proof_count});
      by_module_id.emplace(module.module_id, index);
    }
  }

  if (pending.empty()) {
    active_rel_sections_.clear();
    rel_mapping_dirty_ = false;
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
        // OSLink rewrites the section address but not its size.  Requiring an
        // exact size match also prevents duplicate module-id overlays from
        // spuriously claiming a live header merely because their section is
        // smaller than the currently loaded variant.
        if (runtime_start == 0u || runtime_size != section.size) {
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
      if (valid && item.proofs &&
          !MatchRelImportProofs(ram, candidate, item.proofs, item.proof_count))
        valid = false;
      if (valid && !candidate_sections.empty()) {
        discovered.insert(discovered.end(), candidate_sections.begin(), candidate_sections.end());
        item.found = true;
      }
    }
  }
  active_rel_sections_ = std::move(discovered);
  rel_mapping_dirty_ = false;

  static unsigned refresh_logs = 0u;
  if (refresh_logs++ < 32u) {
    std::fprintf(stderr,
                 "GEKKOAOT_REL_MAP_REFRESH_V156=1 sections=%zu refreshes=%llu action=index-committed\n",
                 active_rel_sections_.size(),
                 static_cast<unsigned long long>(rel_mapping_refreshes_));
    // Diagnostic: GEKKOAOT_REL_MAP_TRACE=1 lists each live section.
    static const bool trace = RuntimeEnvU32("GEKKOAOT_REL_MAP_TRACE", 0u, 1u) != 0u;
    if (trace)
      for (const auto& section : active_rel_sections_)
        std::fprintf(stderr, "GEKKOAOT_REL_MAP_SECTION module=%u section=%u canonical=%08x runtime=%08x size=%08x\n",
                     section.module_id, section.section_index, section.linked_start,
                     section.runtime_start, section.size);
  }
}

const GekkoAOTModuleDesc* HostRuntime::SelectFixedExecutableModule(
    std::uint32_t runtime_address, bool* overlap) {
  if (overlap) *overlap = false;
  // A single DOL plus relocatable RELs cannot have a fixed-image ambiguity.
  // Avoid clearing/scanning the candidate table at every AOT boundary unless
  // a fixed side executable has actually been registered.
  if (!has_fixed_secondary_modules_) return nullptr;

  // RFI during SDK BAT initialization enters the physical MEM1 alias while
  // translation is disabled. DolRecomp dispatch canonicalizes this alias, so
  // image selection must use the same address before consulting coverage and
  // verified routes. Otherwise overlapping ELFs fall through to the first
  // secondary module and execute a different image at the same physical PC.
  if (runtime_address < AddressSpace::CompatMem1BackingSize)
    runtime_address |= 0x80000000u;

  // Keep the currently executing verified chunk on the absolute cheapest path.
  if (fixed_exec_route_cache_.valid && fixed_exec_route_cache_.descriptor &&
      runtime_address >= fixed_exec_route_cache_.chunk_start &&
      runtime_address < fixed_exec_route_cache_.chunk_end) {
    if (overlap) *overlap = true;
    return fixed_exec_route_cache_.descriptor;
  }

  // v277: once ICBI/hash verification has established the active fixed image,
  // most GoldenEye/MOH-style execution lands on pages wholly covered by that
  // image. Test that ownership immediately after the current-chunk cache. The
  // old order still walked secondary-page, route, negative and memo caches
  // before reaching the same proof, making image routing a top perf symbol.
  if (const auto* active = fixed_exec_active_descriptor_;
      active && active->num_rel_modules == 0u && active->chunk_hashes) {
    if (fixed_exec_active_pages_owner_ != active ||
        fixed_exec_active_pages_epoch_ != fixed_exec_structure_epoch_)
      RebuildFixedExecActivePages();
    if (runtime_address >= 0x80000000u) {
      const std::uint32_t mem1_page = (runtime_address - 0x80000000u) >> 12u;
      if (mem1_page < 8192u &&
          ((fixed_exec_active_pages_[mem1_page >> 6u] >> (mem1_page & 63u)) & 1u) != 0u) {
        if (overlap) *overlap = true;
        return active;
      }
    }
  }

  // v275: a PC whose MEM1 page no fixed secondary executable covers has at
  // most one candidate (the main image), which the resolver below reports as
  // "no overlap". The one exception is a verified main image (its active-epoch
  // shortcut returns the main descriptor), so keep the full path for it.
  if (fixed_exec_active_descriptor_ != module_.Descriptor()) {
    if (fixed_exec_secondary_pages_epoch_ != fixed_exec_structure_epoch_)
      RebuildFixedExecSecondaryPages();
    const std::uint32_t mem1_page = (runtime_address - 0x80000000u) >> 12u;
    if (runtime_address >= 0x80000000u && mem1_page < 8192u &&
        ((fixed_exec_secondary_pages_[mem1_page >> 6u] >> (mem1_page & 63u)) & 1u) == 0u) {
      if (fixed_exec_route_cache_.valid &&
          (runtime_address < fixed_exec_route_cache_.chunk_start ||
           runtime_address >= fixed_exec_route_cache_.chunk_end))
        fixed_exec_route_cache_ = {};
      return nullptr;
    }
  }

  // v175: keep a direct-mapped page route cache in addition to the single
  // current-range cache. Fixed side executables often bounce between a handful
  // of main/overlay chunks each frame; one range slot cannot retain that working
  // set and paid FindModuleChunk/candidate scans again on every bounce. Entries
  // are valid only in the current architectural I-cache publication epoch.
  const std::uint32_t route_page = runtime_address >> 12u;
  // v178: MEM1 is 32 MiB = exactly 8192 4K pages.  Use the guest page
  // number directly instead of hashing 8192 pages into the old 256-entry
  // cache.  Fixed overlays that bounce between distant chunks no longer
  // evict each other merely because the hash collides.  Epoch validation
  // still makes ICBI/module publication the authority.
  const std::size_t route_slot =
      static_cast<std::size_t>(route_page) &
      (kFixedExecPageRouteCacheSize - 1u);
  auto& page_route = fixed_exec_page_routes_[route_slot];
  if (page_route.epoch == fixed_exec_route_epoch_ &&
      page_route.page_tag == route_page && page_route.descriptor &&
      runtime_address >= page_route.chunk_start &&
      runtime_address < page_route.chunk_end) {
    fixed_exec_route_cache_ = {page_route.descriptor, page_route.chunk_start,
                               page_route.chunk_end, true};
    ++fixed_exec_page_cache_hits_;
    if (fixed_exec_page_cache_hits_ <= 8u)
      std::fprintf(stderr,
                   "GEKKOAOT_FIXED_EXEC_PAGE_CACHE_V178=1 pc=%08x chunk=%08x-%08x hits=%llu fills=%llu epoch=%llu\n",
                   runtime_address, page_route.chunk_start, page_route.chunk_end,
                   static_cast<unsigned long long>(fixed_exec_page_cache_hits_),
                   static_cast<unsigned long long>(fixed_exec_page_cache_fills_),
                   static_cast<unsigned long long>(fixed_exec_route_epoch_));
    if (overlap) *overlap = true;
    return page_route.descriptor;
  }

  // An unresolved overlap currently falls back to the legacy address router.
  // Before v163 that same PC repeated the full FNV neighborhood resolver on
  // every AOT boundary. Loading loops can hit one ambiguous entry thousands of
  // times per frame, turning the router itself into ~100% CPU. Cache only the
  // exact unresolved PC (not a range), so semantics are identical until ICBI.
  const std::size_t negative_slot =
      ((runtime_address >> 2u) ^ (runtime_address >> 11u) ^ (runtime_address >> 17u)) &
      (kFixedExecNegativeCacheSize - 1u);
  if (fixed_exec_negative_pcs_[negative_slot] == runtime_address) {
    if (overlap) *overlap = true;
    ++fixed_exec_negative_cache_hits_;
    static unsigned negative_hit_logs = 0u;
    if (negative_hit_logs++ < 32u)
      std::fprintf(stderr,
                   "GEKKOAOT_FIXED_EXEC_ROUTER_V163=1 action=negative-pc-cache-hit pc=%08x hits=%llu fallback=legacy-address-routing\\n",
                   runtime_address,
                   static_cast<unsigned long long>(fixed_exec_negative_cache_hits_));
    return nullptr;
  }

  // v274: routes that never read guest memory are resolved once per
  // (structural epoch, active image, PC). icbi drops the active image but
  // cannot change chunk coverage, so these entries survive it. The preceding route caches already missed,
  // exactly as on the full path below, so replaying the memo reproduces that
  // path's result and its route/page-cache side effects.
  const auto& memo = fixed_exec_route_memo_[
      ((runtime_address >> 2u) ^ (runtime_address >> 13u)) &
      (kFixedExecRouteMemoSize - 1u)];
  if (memo.epoch == fixed_exec_structure_epoch_ && memo.pc == runtime_address &&
      memo.active == fixed_exec_active_descriptor_) {
    if (memo.route) {
      fixed_exec_route_cache_ = {memo.route, memo.chunk_start, memo.chunk_end, true};
      page_route = {memo.route, route_page, memo.chunk_start, memo.chunk_end,
                    fixed_exec_route_epoch_};
      ++fixed_exec_page_cache_fills_;
      if (overlap) *overlap = true;
      return memo.route;
    }
    if (fixed_exec_route_cache_.valid &&
        (runtime_address < fixed_exec_route_cache_.chunk_start ||
         runtime_address >= fixed_exec_route_cache_.chunk_end))
      fixed_exec_route_cache_ = {};
    return nullptr;
  }

  return SelectFixedExecutableModuleSlow(runtime_address, overlap);
}

void HostRuntime::RebuildFixedExecActivePages() {
  fixed_exec_active_pages_.fill(0u);
  fixed_exec_active_pages_owner_ = fixed_exec_active_descriptor_;
  fixed_exec_active_pages_epoch_ = fixed_exec_structure_epoch_;
  const auto* active = fixed_exec_active_descriptor_;
  if (!active || !active->chunk_ranges || !active->chunk_hashes) return;
  constexpr std::uint64_t kBase = 0x80000000u;
  constexpr std::uint64_t kEnd = kBase + 8192ull * 4096ull;
  // Merge the valid chunks (chunk_ranges are sorted, as FindModuleChunk's
  // binary search requires) and mark only pages lying wholly inside a merged
  // interval: every word of such a page is in a chunk with a build hash.
  std::uint64_t run_start = 0u, run_end = 0u;
  const auto flush = [&]() {
    if (run_end <= run_start) return;
    const std::uint64_t start = std::max(run_start, kBase);
    const std::uint64_t end = std::min(run_end, kEnd);
    if (end <= start) return;
    const std::uint64_t first = (start - kBase + 4095u) >> 12u;
    const std::uint64_t last = (end - kBase) >> 12u;  // exclusive
    for (std::uint64_t page = first; page < last; ++page)
      fixed_exec_active_pages_[page >> 6u] |= 1ull << (page & 63u);
  };
  for (std::uint32_t i = 0; i < active->num_chunk_ranges; ++i) {
    if (active->chunk_hashes[i] == 0u) continue;
    const std::uint64_t start = active->chunk_ranges[i].start;
    const std::uint64_t end = active->chunk_ranges[i].end;
    if (end <= start) continue;
    if (start <= run_end && end > run_start && run_end > run_start) {
      run_end = std::max(run_end, end);
      continue;
    }
    flush();
    run_start = start;
    run_end = end;
  }
  flush();
}

void HostRuntime::RebuildFixedExecSecondaryPages() {
  fixed_exec_secondary_pages_.fill(0u);
  constexpr std::uint32_t kBase = 0x80000000u;
  constexpr std::uint32_t kLast = kBase + 8192u * 4096u - 1u;
  for (const auto& secondary : secondary_modules_) {
    const auto* descriptor = secondary ? secondary->Descriptor() : nullptr;
    // Same admission as the resolver's candidates (REL-bearing images and
    // images without chunk hashes never take part in fixed-image routing).
    if (!descriptor || descriptor->num_rel_modules != 0u || !descriptor->chunk_hashes ||
        !descriptor->chunk_ranges)
      continue;
    for (std::uint32_t i = 0; i < descriptor->num_chunk_ranges; ++i) {
      if (descriptor->chunk_hashes[i] == 0u) continue;
      const auto range = descriptor->chunk_ranges[i];
      if (range.end <= range.start || range.end - 1u < kBase || range.start > kLast) continue;
      const std::uint32_t first = (std::max(range.start, kBase) - kBase) >> 12u;
      const std::uint32_t last = (std::min(range.end - 1u, kLast) - kBase) >> 12u;
      for (std::uint32_t page = first; page <= last; ++page)
        fixed_exec_secondary_pages_[page >> 6u] |= 1ull << (page & 63u);
    }
  }
  fixed_exec_secondary_pages_epoch_ = fixed_exec_structure_epoch_;
}

// v274: the cache probes above run on every AOT boundary of titles with a
// fixed side executable. Keep the resolver's 1.5 KiB candidate/score frame out
// of that path; this body is the unchanged v84-v178 resolver.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
const GekkoAOTModuleDesc* HostRuntime::SelectFixedExecutableModuleSlow(
    std::uint32_t runtime_address, bool* overlap) {
  const std::uint32_t route_page = runtime_address >> 12u;
  auto& page_route = fixed_exec_page_routes_[
      static_cast<std::size_t>(route_page) & (kFixedExecPageRouteCacheSize - 1u)];
  const std::size_t negative_slot =
      ((runtime_address >> 2u) ^ (runtime_address >> 11u) ^ (runtime_address >> 17u)) &
      (kFixedExecNegativeCacheSize - 1u);
  auto& memo = fixed_exec_route_memo_[
      ((runtime_address >> 2u) ^ (runtime_address >> 13u)) &
      (kFixedExecRouteMemoSize - 1u)];
  const auto memoize = [&](const GekkoAOTModuleDesc* route, std::uint32_t start,
                           std::uint32_t end) {
    memo = {fixed_exec_structure_epoch_, fixed_exec_active_descriptor_, route,
            runtime_address, start, end};
  };

  const auto remember_route = [&](const GekkoAOTModuleDesc* route_descriptor,
                                  std::uint32_t start, std::uint32_t end) {
    fixed_exec_route_cache_ = {route_descriptor, start, end, true};
    if (!route_descriptor || runtime_address < start || runtime_address >= end)
      return;
    page_route = {route_descriptor, route_page, start, end,
                  fixed_exec_route_epoch_};
    ++fixed_exec_page_cache_fills_;
  };

  // v161: once v155 has proven which fixed executable owns the current
  // instruction-cache epoch, do not rebuild the candidate list and run
  // FindModuleChunk across every side executable at each new AOT chunk.  ICBI
  // already clears fixed_exec_active_descriptor_, so this remains exactly as
  // conservative as the existing epoch ownership rule while removing a hot
  // routing cost in games such as MOH Frontline with several fixed ELFs.
  if (fixed_exec_active_descriptor_ && fixed_exec_active_descriptor_->num_rel_modules == 0u &&
      fixed_exec_active_descriptor_->chunk_hashes) {
    std::uint32_t active_chunk = 0u;
    if (FindModuleChunk(fixed_exec_active_descriptor_, runtime_address, &active_chunk) &&
        fixed_exec_active_descriptor_->chunk_hashes[active_chunk] != 0u) {
      const auto range = fixed_exec_active_descriptor_->chunk_ranges[active_chunk];
      remember_route(fixed_exec_active_descriptor_, range.start, range.end);
      memoize(fixed_exec_active_descriptor_, range.start, range.end);
      if (overlap) *overlap = true;
      return fixed_exec_active_descriptor_;
    }
  }

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
    if (memory_.PagedVmem() && secondary_index >= 0) {
      const auto index = static_cast<std::size_t>(secondary_index);
      if (index >= secondary_modules_.size() || !secondary_modules_[index] ||
          !secondary_modules_[index]->SupportsMemoryRestart())
        return;
    }
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
    memoize(nullptr, 0u, 0u);
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
      remember_route(candidate.descriptor, range.start, range.end);
      memoize(candidate.descriptor, range.start, range.end);
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
    remember_route(candidate.descriptor, range.start, range.end);
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
  fixed_exec_negative_pcs_[negative_slot] = runtime_address;
  static unsigned miss_logs = 0u;
  if (miss_logs++ < 64u)
    std::fprintf(stderr,
                 "GEKKOAOT_FIXED_EXEC_ROUTER_V163=0 action=identity-unresolved pc=%08x candidates=%zu matching=%zu cache=exact-pc fallback=legacy-address-routing\\n",
                 runtime_address, candidate_count, matching_count);
  return nullptr;
}

bool HostRuntime::DispatchFixedExecutableModule(const GekkoAOTModuleDesc* descriptor,
                                                std::uint32_t runtime_address,
                                                RunResult& result) {
  if (!descriptor || !descriptor->dispatch) return false;
  if (memory_.PagedVmem() && descriptor != module_.Descriptor()) {
    for (const auto& side : secondary_modules_) {
      if (!side || side->Descriptor() != descriptor) continue;
      if (!side->SupportsMemoryRestart()) return false;
      break;
    }
  }
  RecordCompatBreadcrumb(runtime_address, CompatDispatchKind::Secondary);
  cpu_.pc = runtime_address;
  cpu_.downcount = 0;
  cpu_.external_read_count = 0;
  cpu_.external_write_count = 0;
  native_region_last_blocked_start_ = 0u;
  native_region_probe_capture_ = true;
  const bool dispatched = descriptor->dispatch(&cpu_, runtime_address);
  native_region_probe_capture_ = false;
  if (!dispatched) {
    cpu_.pc = runtime_address;
    return false;
  }
  // v350: an overlay image reloaded over the bootstrap executable (MoH
  // Frontline re-runs its stub to leave a mission) keeps the bootstrap SDK
  // hooks' original bytes, so the entry-time region guard bounces to the host
  // before one instruction runs; the router then picks the same image again
  // and the guest livelocks with time frozen. Mirror the v204c REL handling:
  // run the exact host call when the entry itself is hooked, otherwise admit
  // the rejected range once.
  if (cpu_.pc == runtime_address && cpu_.downcount == 0 &&
      cpu_.external_read_count == 0u && cpu_.external_write_count == 0u &&
      native_region_last_blocked_start_ != 0u) {
    const std::uint32_t blocked_start = native_region_last_blocked_start_;
    static unsigned zero_progress_logs = 0u;
    if (DispatchHostCall(runtime_address)) {
      ++hle_calls_;
      ++result.dispatches;
      if (zero_progress_logs++ < 64u)
        std::fprintf(stderr,
                     "GEKKOAOT_FIXED_ZERO_PROGRESS_V350=1 pc=%08x guard=%08x action=dispatch-exact-host-call\n",
                     runtime_address, blocked_start);
      return true;
    }
    native_region_retry_start_ = blocked_start;
    cpu_.pc = runtime_address;
    const bool retried = descriptor->dispatch(&cpu_, runtime_address);
    native_region_retry_start_ = 0u;
    if (!retried) {
      cpu_.pc = runtime_address;
      return false;
    }
    if (zero_progress_logs++ < 64u)
      std::fprintf(stderr,
                   "GEKKOAOT_FIXED_ZERO_PROGRESS_V350=1 pc=%08x guard=%08x next=%08x action=retry-blocked-range-once\n",
                   runtime_address, blocked_start, cpu_.pc);
  }
  ++secondary_dispatches_;
  ++result.dispatches;
  if (cpu_.downcount < 0) {
    const std::uint64_t charged =
        static_cast<std::uint64_t>(-(cpu_.downcount + 1)) + 1u;
    AdvanceRuntimeCyclesFast(charged, result);
  }
  ConsumeAotPollPark(result);
  return true;
}

bool HostRuntime::ResolveSecondaryAddress(std::uint32_t runtime_address,
                                          const GekkoAOTModuleDesc** descriptor,
                                          std::uint32_t* linked_address) {
  if (!descriptor || !linked_address) return false;
  *descriptor = nullptr;
  *linked_address = runtime_address;

  // Never trust a previously discovered live mapping after the guest has
  // published new code.  Refresh once first; all following lookups in the same
  // I-cache epoch stay on the cheap indexed path.
  if (rel_mapping_dirty_) RefreshSecondaryRelMappings();

  if (cached_rel_route_.owner && runtime_address >= cached_rel_route_.runtime_start &&
      runtime_address < cached_rel_route_.runtime_end) {
    *descriptor = cached_rel_route_.owner;
    *linked_address = cached_rel_route_.linked_start +
                      (runtime_address - cached_rel_route_.runtime_start);
    return true;
  }

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
      // Cache only verified executable coverage, bounded to this live section.
      // The publication epoch invalidates it before any overlay replacement.
      for (std::uint32_t i = 0; i < section.owner->num_code_ranges; ++i) {
        const auto& code = section.owner->code_ranges[i];
        if (linked < code.start || linked >= code.end) continue;
        const auto start = std::max(code.start, section.linked_start);
        const auto end = std::min<std::uint64_t>(code.end,
            std::uint64_t(section.linked_start) + section.size);
        const auto live_start = section.runtime_start + (start - section.linked_start);
        const auto live_end = live_start + (end - start);
        bool exclusive = true;
        const auto* main = module_.Descriptor();
        if (main) {
          for (std::uint32_t j = 0; j < main->num_code_ranges; ++j) {
            const auto& range = main->code_ranges[j];
            if (live_start < range.end && live_end > range.start) {
              exclusive = false; break;
            }
          }
        }
        cached_rel_route_ = {section.owner, live_start,
            static_cast<std::uint32_t>(live_end), start, exclusive};
        break;
      }
      return true;
    }
    return false;
  };
  if (resolve_active()) return true;

  // Older modules can publish canonical callbacks. Only a currently linked
  // REL can own such a target; coverage in an unloaded variant is no proof.
  const ActiveRelSection* canonical_owner = nullptr;
  for (const auto& section : active_rel_sections_) {
    if (runtime_address < section.linked_start ||
        std::uint64_t(runtime_address) >= std::uint64_t(section.linked_start) + section.size ||
        !section.owner ||
        !AddressInRanges(section.owner->code_ranges, section.owner->num_code_ranges,
                         runtime_address))
      continue;
    if (canonical_owner && canonical_owner->owner != section.owner)
      return false; // Ambiguous canonical callbacks must be published as live addresses.
    canonical_owner = &section;
  }
  if (canonical_owner) {
    *descriptor = canonical_owner->owner;
    return true;
  }

  // Fixed-address DOL/ELF side modules can be dispatched directly. RELs must
  // have a live mapping: identical synthetic coverage in several variants
  // cannot establish which executable is actually installed.
  const auto resolve_canonical = [&]() -> bool {
    for (const auto& library : secondary_modules_) {
      const auto* module = library ? library->Descriptor() : nullptr;
      if (memory_.PagedVmem() && module && module->num_rel_modules == 0u &&
          library && !library->SupportsMemoryRestart())
        continue;
      if (module && module->num_rel_modules == 0u &&
          AddressInRanges(module->code_ranges, module->num_code_ranges, runtime_address)) {
        *descriptor = module;
        *linked_address = runtime_address;
        return true;
      }
    }
    return false;
  };

  // Prefer live OSLink mappings over canonical fallback.  The expensive map
  // refresh already happened above only when the I-cache epoch was dirty.
  if (resolve_active()) return true;
  return resolve_canonical();
}

std::uint32_t HostRuntime::TranslateSecondaryAddress(const GekkoAOTModuleDesc* descriptor,
                                                     std::uint32_t linked_address) const {
  // A relocation in REL A may branch directly to the canonical linked address
  // of REL B. active_rel_sections_ now includes data too, so require native
  // code coverage for PC translation while the memory bridge below can use all
  // sections.
  // The executing owner disambiguates overlapping synthetic REL windows.
  // Prefer it before considering an explicit cross-module destination.
  for (const auto& section : active_rel_sections_) {
    if (section.owner != descriptor) continue;
    const auto end = std::uint64_t(section.linked_start) + section.size;
    if (linked_address >= section.linked_start && std::uint64_t(linked_address) < end &&
        AddressInRanges(descriptor->code_ranges, descriptor->num_code_ranges, linked_address))
      return section.runtime_start + (linked_address - section.linked_start);
  }
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

void HostRuntime::PublishSecondaryControlState(const GekkoAOTModuleDesc* descriptor) {
  // LR is an architectural return address. A main-DOL callee may save it in an
  // OSThread or stack frame before returning, so it must carry the real OSLink
  // address, never the compiler's synthetic window. Exception SRR0 needs the
  // same repair when an exception was raised inside the REL wrapper.
  cpu_.lr = TranslateSecondaryAddress(descriptor, cpu_.lr);
  if (IsLowExceptionVectorAddress(AddressSpace::ToPhysical(cpu_.pc)))
    cpu_.srr0 = TranslateSecondaryAddress(descriptor, cpu_.srr0);
}

bool HostRuntime::TranslateCanonicalRelAddress(std::uint32_t canonical_address,
                                               std::uint32_t* runtime_address) const {
  if (!runtime_address) return false;

  // Address queries emitted by a REL have a known owner. Its own sections
  // must win over another loaded module with the same synthetic link window.
  if (native_region_secondary_owner_) {
    // v366: a REL's canonical window and its own OSLink placement can overlap
    // (Twilight Princess module 55: canonical 80c6814c, runtime 80c615ac).
    // Translation requests come from canonical provenance, so an address in
    // both of the owner's windows is canonical: GEKKOAOT_REL_OWNER_CANONICAL_FIRST=1
    // translates it instead of keeping it as a live pointer. Opt-in: no title
    // has hit the overlap yet (TP's module-55 crash logged none).
    static const bool canonical_first = RuntimeEnvBool("GEKKOAOT_REL_OWNER_CANONICAL_FIRST", false);
    const ActiveRelSection* own_canonical = nullptr;
    for (const auto& section : active_rel_sections_)
      if (section.owner == native_region_secondary_owner_ && canonical_address >= section.linked_start &&
          std::uint64_t(canonical_address) < std::uint64_t(section.linked_start) + section.size) {
        own_canonical = &section;
        break;
      }
    for (const auto& section : active_rel_sections_) {
      if (section.owner != native_region_secondary_owner_) continue;
      if (canonical_address >= section.runtime_start &&
          std::uint64_t(canonical_address) < std::uint64_t(section.runtime_start) + section.size) {
        if (own_canonical) {
          static unsigned overlap_logs = 0u;
          if (overlap_logs++ < 16u)
            std::fprintf(stderr,
                         "GEKKOAOT_REL_OWN_WINDOW_OVERLAP_V366 pc=%08x address=%08x module=%u canonical_section=%u "
                         "runtime_section=%u action=%s\n",
                         cpu_.pc, canonical_address, section.module_id, own_canonical->section_index,
                         section.section_index, canonical_first ? "translate" : "keep");
          if (canonical_first) {
            *runtime_address = own_canonical->runtime_start + (canonical_address - own_canonical->linked_start);
            return true;
          }
        }
        *runtime_address = canonical_address;
        return true;
      }
    }
    for (const auto& section : active_rel_sections_) {
      if (section.owner != native_region_secondary_owner_) continue;
      if (canonical_address >= section.linked_start &&
          std::uint64_t(canonical_address) < std::uint64_t(section.linked_start) + section.size) {
        *runtime_address = section.runtime_start + (canonical_address - section.linked_start);
        // Diagnostic: the owner's canonical window also covers another live
        // module's OSLink placement, so this address is ambiguous.
        static unsigned ambiguous_logs = 0u;
        if (ambiguous_logs < 64u)
          for (const auto& other : active_rel_sections_)
            if (other.owner != native_region_secondary_owner_ &&
                canonical_address >= other.runtime_start &&
                std::uint64_t(canonical_address) < std::uint64_t(other.runtime_start) + other.size) {
              ++ambiguous_logs;
              std::fprintf(stderr,
                           "GEKKOAOT_REL_AMBIGUOUS_ADDRESS_V366 pc=%08x lr=%08x address=%08x owner_module=%u "
                           "owner_runtime=%08x live_module=%u live_section=%u\n",
                           cpu_.pc, cpu_.lr, canonical_address, section.module_id, *runtime_address,
                           other.module_id, other.section_index);
              break;
            }
        return true;
      }
    }
  }

  // v172: the synthetic REL link window and OSLink's live MEM1 placements can
  // overlap numerically. An already-linked pointer from the currently running
  // REL can therefore look like a canonical pointer belonging to another REL.
  // Translating it a second time corrupts data/heap state. Keep that pointer
  // unchanged when the live mapping says it belongs to the current REL while
  // the canonical mapping would reinterpret it as a different module.
  std::uint32_t current_module_id = 0u;
  for (const auto& section : active_rel_sections_) {
    const std::uint64_t linked_end =
        std::uint64_t(section.linked_start) + section.size;
    if (cpu_.pc >= section.linked_start && std::uint64_t(cpu_.pc) < linked_end) {
      current_module_id = section.module_id;
      break;
    }
  }

  const ActiveRelSection* runtime_hit = nullptr;
  const ActiveRelSection* canonical_hit = nullptr;
  for (const auto& section : active_rel_sections_) {
    const std::uint64_t runtime_end =
        std::uint64_t(section.runtime_start) + section.size;
    if (!runtime_hit && canonical_address >= section.runtime_start &&
        std::uint64_t(canonical_address) < runtime_end)
      runtime_hit = &section;

    const std::uint64_t linked_end =
        std::uint64_t(section.linked_start) + section.size;
    if (!canonical_hit && canonical_address >= section.linked_start &&
        std::uint64_t(canonical_address) < linked_end)
      canonical_hit = &section;
  }

  if (current_module_id != 0u && runtime_hit && canonical_hit &&
      runtime_hit->module_id == current_module_id &&
      canonical_hit->module_id != current_module_id) {
    *runtime_address = canonical_address;
    static unsigned identity_logs = 0u;
    if (identity_logs++ < 32u)
      std::fprintf(stderr,
                   "GEKKOAOT_REL_RUNTIME_IDENTITY_V172=1 pc=%08x address=%08x "
                   "runtime_module=%u canonical_module=%u action=keep-runtime\n",
                   cpu_.pc, canonical_address, runtime_hit->module_id,
                   canonical_hit->module_id);
    return true;
  }

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

std::uint32_t HostRuntime::SecondaryModuleCapabilities(const GekkoAOTModuleDesc* descriptor) {
  if (cached_secondary_caps_owner_ == descriptor) return cached_secondary_caps_;
  const auto found = secondary_module_caps_.find(descriptor);
  cached_secondary_caps_owner_ = descriptor;
  cached_secondary_caps_ = found == secondary_module_caps_.end() ? 0u : found->second;
  return cached_secondary_caps_;
}

bool HostRuntime::DispatchSecondaryModule(std::uint32_t runtime_address, RunResult& result) {
  const GekkoAOTModuleDesc* resolved = nullptr;
  std::uint32_t linked_address = runtime_address;

  const auto commit_dispatch = [&](const GekkoAOTModuleDesc* descriptor,
                                   std::uint32_t address) -> bool {
    if (!descriptor || !descriptor->dispatch) return false;
    TraceRelGprs("secondary-enter",
                 runtime_address, address, cpu_);
    if (memory_.PagedVmem() && descriptor != module_.Descriptor()) {
      for (const auto& side : secondary_modules_) {
        if (!side || side->Descriptor() != descriptor) continue;
        if (!side->SupportsMemoryRestart()) return false;
        break;
      }
    }
    RecordCompatBreadcrumb(address, CompatDispatchKind::Secondary);
    cpu_.pc = address;
    cpu_.downcount = 0;
    cpu_.external_read_count = 0;
    cpu_.external_write_count = 0;

    native_region_last_blocked_start_ = 0u;
    native_region_probe_capture_ = true;
    const auto* previous_secondary_owner = native_region_secondary_owner_;
    native_region_secondary_owner_ = descriptor;
    const bool first_dispatch_ok = descriptor->dispatch(&cpu_, address);
    native_region_secondary_owner_ = previous_secondary_owner;
    native_region_probe_capture_ = false;
    if (!first_dispatch_ok) return false;

    // v204c: an entry-time region guard can return to the same REL PC before
    // executing any guest instruction. Distinguish that from a genuine guest
    // loop by requiring zero charged cycles and zero external accesses.
    if (cpu_.pc == address && cpu_.downcount == 0 &&
        cpu_.external_read_count == 0u &&
        cpu_.external_write_count == 0u &&
        native_region_last_blocked_start_ != 0u) {
      const std::uint32_t blocked_start = native_region_last_blocked_start_;

      // Preserve a real HLE entry if the requested PC itself is intercepted.
      if (DispatchHostCall(address)) {
        ++hle_calls_;
        static unsigned exact_hle_logs = 0u;
        if (exact_hle_logs++ < 64u)
          std::fprintf(stderr,
                       "GEKKOAOT_REL_ZERO_PROGRESS_V204C=1 pc=%08x guard=%08x action=dispatch-exact-host-call\n",
                       address, blocked_start);
      } else {
        // The rejected guard may be the containing function start while the
        // dispatch address is an interior continuation (e.g. 805E2D3C inside
        // 805E2CAC). Admit exactly that guard once.
        native_region_retry_start_ = blocked_start;
        cpu_.pc = address;
        cpu_.downcount = 0;
        cpu_.external_read_count = 0;
        cpu_.external_write_count = 0;
        const auto* previous_retry_owner = native_region_secondary_owner_;
        native_region_secondary_owner_ = descriptor;
        const bool retry_ok = descriptor->dispatch(&cpu_, address);
        native_region_secondary_owner_ = previous_retry_owner;
        native_region_retry_start_ = 0u;
        if (!retry_ok) return false;

        static unsigned retry_logs = 0u;
        if (retry_logs++ < 64u)
          std::fprintf(stderr,
                       "GEKKOAOT_REL_ZERO_PROGRESS_V204C=1 pc=%08x guard=%08x next=%08x downcount=%lld action=retry-blocked-range-once\n",
                       address, blocked_start, cpu_.pc,
                       static_cast<long long>(cpu_.downcount));

        if (cpu_.pc == address && cpu_.downcount == 0 &&
            cpu_.external_read_count == 0u &&
            cpu_.external_write_count == 0u)
          return false;
      }
    }

    // v174: a REL is compiled at a synthetic canonical link address, while
    // OSLink installs it at a live MEM1 address. DolRecomp may intentionally
    // keep static REL data bases canonical across *internal* AOT side exits so
    // its provenance-aware addressing can continue on the next slice. v173
    // normalized r14-r31 after every dispatch slice, which could therefore turn
    // an internal compiler/runtime boundary into a false guest ABI boundary.
    // SMB1/2 are particularly sensitive because their main game code spends
    // long stretches inside RELs.
    //
    // Normalize registers only when control really leaves this REL (main.dol
    // or another module). Argument registers must be included: a REL may pass
    // a pointer to its static data through r3-r10 to a main.dol function.
    // If the next PC is either a
    // canonical or live executable address owned by this same descriptor, keep
    // the canonical register state intact for the next AOT slice.
    // Relocation-proven codegen already writes fully materialized pointers to
    // architectural GPRs. Its module boundary needs no numeric register repair.
    // Keep the legacy policy only for modules lacking this explicit capability.
    if ((SecondaryModuleCapabilities(descriptor) & GEKKOAOT_MODULE_CAP_REL_LIVE_GPRS) == 0u) {
      bool stays_in_rel =
          AddressInRanges(descriptor->code_ranges, descriptor->num_code_ranges, cpu_.pc);
      if (!stays_in_rel) {
        for (const auto& section : active_rel_sections_) {
          if (section.owner != descriptor ||
              !AddressInRanges(descriptor->code_ranges, descriptor->num_code_ranges,
                               section.linked_start))
            continue;
          const std::uint64_t runtime_end =
              std::uint64_t(section.runtime_start) + section.size;
          if (cpu_.pc >= section.runtime_start && std::uint64_t(cpu_.pc) < runtime_end) {
            stays_in_rel = true;
            break;
          }
        }
      }

      // An architectural exception is a suspended instruction, not a guest ABI
      // call. In particular, a lazy-FPU trap can occur between a relocated LIS
      // and ADDI: translating its unfinished high half corrupts the eventual
      // address when initialized data and BSS have different live placements.
      // Preserve all GPR bits until RFI resumes; only publish SRR0/LR below.
      const bool enters_exception = cpu_.pc < 0x10000u;
      if (!stays_in_rel && !enters_exception) {
        // v198: only registers that can legitimately carry pointer state across a
        // PowerPC module ABI boundary are eligible for canonical->live repair.
        //
        // r3-r10 are argument/return registers and r14-r31 are callee-saved. r0,
        // r11 and r12 are volatile scratch registers, while r2/r13 are TOC/SDA
        // anchors. Treating an arbitrary scratch value as a REL pointer is unsafe:
        // Mickey's area008.rel exits with r0 == 0x80581f18 (numerically inside the
        // synthetic REL data window), but that value is not an architectural
        // pointer that survives the call. v174 remapped it to 0x812fc838 and the
        // main-DOL script parser subsequently lost its stream state.
        for (std::uint32_t reg = 3u; reg < 32u; ++reg) {
          if ((reg >= 11u && reg <= 13u)) continue;
          const std::uint32_t value = cpu_.gpr[reg];
          if (AddressInRanges(descriptor->code_ranges, descriptor->num_code_ranges, value))
            continue;

          bool already_live = false;
          for (const auto& section : active_rel_sections_) {
            if (section.owner != descriptor) continue;
            const std::uint64_t runtime_end =
                std::uint64_t(section.runtime_start) + section.size;
            if (value >= section.runtime_start && std::uint64_t(value) < runtime_end) {
              already_live = true;
              break;
            }
          }
          if (already_live) continue;

          for (const auto& section : active_rel_sections_) {
            if (section.owner != descriptor) continue;
            const std::uint64_t linked_end =
                std::uint64_t(section.linked_start) + section.size;
            if (value < section.linked_start || std::uint64_t(value) >= linked_end)
              continue;
            // Code pointers remain canonical for the dispatcher/router. The state
            // repair is specifically for static data bases crossing a real guest
            // module ABI boundary.
            if (AddressInRanges(descriptor->code_ranges, descriptor->num_code_ranges,
                                section.linked_start))
              break;

            const std::uint32_t translated =
                section.runtime_start + (value - section.linked_start);
            cpu_.gpr[reg] = translated;
            static unsigned rel_state_logs = 0u;
            if (rel_state_logs++ < 64u)
              std::fprintf(stderr,
                           "GEKKOAOT_REL_ABI_STATE_V198=1 module=%u reg=r%u canonical=%08x runtime=%08x action=normalize-pointer-abi-only\n",
                           section.module_id, reg, value, translated);
            break;
          }
        }
      }

    }
    PublishSecondaryControlState(descriptor);
    cpu_.pc = TranslateSecondaryAddress(descriptor, cpu_.pc);
    TraceRelGprs("secondary-translated",
                 runtime_address, address, cpu_);
    ++secondary_dispatches_;
    ++result.dispatches;
    if (cpu_.downcount < 0) {
      const std::uint64_t charged =
          static_cast<std::uint64_t>(-(cpu_.downcount + 1)) + 1u;
      AdvanceRuntimeCycles(charged, result);
    }
    ConsumeAotPollPark(result);
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

std::uint64_t HostRuntime::IdleWakeDeadlineCycles(bool include_decrementer) const {
  using GekkoAOT::NativeHW::Events::Deadlines;
  using GekkoAOT::NativeHW::Events::Earliest;
  using GekkoAOT::NativeHW::Events::Never;

  // An already-asserted source should be delivered immediately, not parked.
  if (pi_.InterruptPending() || (include_decrementer && decrementer_pending_)) return 0u;

  Deadlines deadlines{};
  deadlines.vi = vi_.CyclesUntilNextInterrupt();
  // Presentation is host-native work too. Even with all VI compare masks off,
  // an idle PPC must not sleep across a completed scanout frame indefinitely.
  if (native_hw_.VI() && native_gx_.Ready())
    deadlines.vi_frame = vi_.CyclesUntilNextFrameBoundary();
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
  if (native_input_ready_) {
    constexpr std::uint64_t kNativeInputPollCycles =
        GekkoAOT::NativeHW::SI::PollIntervalCycles(
            GekkoAOT::HW::VI::NativeVI::CpuClockHz, 120u);
    deadlines.si = GekkoAOT::NativeHW::SI::CyclesUntilPoll(
        true, native_input_hw_phase_, kNativeInputPollCycles);
  }
#endif
  deadlines.si = std::min(deadlines.si, si_.CyclesUntilTransfer());
  deadlines.ai = ai_.CyclesUntilInterrupt();
  deadlines.dsp = dsp_.CyclesUntilInterrupt();
  if (di_async_completion_pending_)
    deadlines.di = std::max<std::uint64_t>(1u, di_async_cycles_remaining_);

  // DEC counts Time Base ticks at CPU/12. Account for the partial tick already
  // accumulated so an idle park cannot sleep across the 0 -> 0xffffffff edge.
  const std::uint64_t dec_ticks = static_cast<std::uint64_t>(spr_state_[kSprDec]) + 1ull;
  const std::uint64_t dec_scaled = dec_ticks * kTimerRatio;
  deadlines.decrementer =
      !include_decrementer ? Never :
      dec_scaled > decrementer_cycle_remainder_
          ? dec_scaled - decrementer_cycle_remainder_
          : 1u;

  const auto alarm_ticks =
      GekkoAOT::NativeOS::Service::Get().TimebaseTicksUntilNextAlarm(&cpu_);
  if (alarm_ticks != Never) {
    if (alarm_ticks == 0u) {
      deadlines.alarm = 0u;
    } else {
      const std::uint64_t alarm_scaled = alarm_ticks * kTimerRatio;
      deadlines.alarm = alarm_scaled > timebase_cycle_remainder_
                            ? alarm_scaled - timebase_cycle_remainder_
                            : 1u;
    }
  }
  return Earliest(deadlines);
}

void HostRuntime::WaitForIdleEvent(RunResult& result,std::uint64_t maximum_cycles,bool masked_timer,
                                   bool hold_decrementer) {
  // First publish any wall time that elapsed while the guest was entering the
  // idle path. This can itself latch the event we were about to wait for.
  AdvanceRuntimeCycles(0u, result, true);
  if (result.status == RunStatus::HostRequestedExit) return;

  std::uint64_t deadline = std::min(IdleWakeDeadlineCycles(!hold_decrementer),maximum_cycles);
  // A proved finite counter delay can occur with interrupts masked. Pending
  // causes remain latched; they cannot end this timer wait until EE is restored.
  if(deadline==0u && masked_timer && (cpu_.msr&kPpcMsrEe)==0u) deadline=maximum_cycles;
  if (deadline == 0u) return;

  constexpr std::uint64_t kClockHz = GekkoAOT::HW::VI::NativeVI::CpuClockHz;
  const std::uint64_t maintenance_cap = std::max<std::uint64_t>(
      1u, (kClockHz * idle_event_max_wait_us_ + 999999ull) / 1000000ull);
  deadline = GekkoAOT::NativeHW::Events::ClampMaintenance(deadline,
                                                               maintenance_cap);

  // Convert the hardware deadline to one host sleep. Unlike the old 4096-cycle
  // spin loop, wall time is the authority: the hardware clock is sampled again
  // after waking and advances by the actual elapsed duration, including any OS
  // scheduler overshoot. No guest/device time is fabricated from host speed.
  const auto wait_ns = std::max<std::uint64_t>(
      1u, (deadline * 1000000000ull + kClockHz - 1u) / kClockHz);
  ++idle_event_waits_;
  idle_event_requested_cycles_ += deadline;
  const auto until = std::chrono::steady_clock::now() + std::chrono::nanoseconds(wait_ns);
  // v300: PE token/finish events come from the GX worker, asynchronously to
  // every modelled deadline above. While it still owns queued commands (and
  // after publishing our partial WGPIPE batch), sleep on its event channel so a
  // DrawDone/token wait resumes when the GP work completes, not up to the next
  // VI/maintenance deadline (2 ms) later.
  const auto idle_start = runtime_profile_enabled_ ? std::chrono::steady_clock::now()
                                                  : std::chrono::steady_clock::time_point{};
  const bool gx_wait = runtime_parallel_workers_ && native_gx_.WaitForWorkerEvent(until);
  if (!gx_wait) std::this_thread::sleep_until(until);
  if (runtime_profile_enabled_) {
    const auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - idle_start).count());
    (gx_wait ? idle_ns_gx_busy_ : idle_ns_gx_idle_) += ns;
    if (gx_wait) ++idle_gx_wakes_;
  }
  AdvanceRuntimeCycles(0u, result, true);
}

void HostRuntime::ConsumeAotPollPark(RunResult& result) {
  if (cpu_.external_rid != kAotPollParkMarker) return;
  cpu_.external_rid = 0u;

  if (!aot_poll_park_enabled_ || result.status == RunStatus::HostRequestedExit)
    return;

  // A pure AOT poll has no guest-visible work to perform until an asynchronous
  // device/timer/callback can change the loaded RAM flag. Keep architectural
  // interrupt delivery exact: pending IRQ/DEC enters the real guest vector
  // immediately; otherwise park only while EE is enabled and wake on the same
  // event horizon used by the SDK/NativeOS idle paths. The maintenance cap in
  // WaitForIdleEvent prevents an unmodelled producer from sleeping forever.
  if ((cpu_.msr & kPpcMsrEe) == 0u) return;

  ++aot_poll_parks_;
  CountHostEffect();  // already parked by the structural AOT poll detector
  if (aot_poll_parks_ <= 16u) {
    std::fprintf(stderr,
                 "GEKKOAOT_AOT_POLL_PARK_V42=1 pc=%08x parks=%llu ee=1 action=event-deadline-wait\n",
                 cpu_.pc, static_cast<unsigned long long>(aot_poll_parks_));
  }
  if (TryTakeExternalInterrupt() || TryTakeDecrementerInterrupt()) return;

  if (idle_event_wait_enabled_)
    WaitForIdleEvent(result);

  if (result.status != RunStatus::HostRequestedExit) {
    TryTakeExternalInterrupt();
    TryTakeDecrementerInterrupt();
  }
}

HostRuntime::DispatchCodeProbeCacheEntry& HostRuntime::ProbeDispatchCode(std::uint32_t pc) {
  // Executable publication invalidates all classifications at once. Storing
  // that epoch once leaves each hot slot in eight bytes rather than 24.
  if (dispatch_code_probe_epoch_ != dispatch_code_probe_generation_) {
    dispatch_code_probe_cache_.fill({});
    dispatch_code_probe_epoch_ = dispatch_code_probe_generation_;
  }
  const std::size_t slot = ((pc >> 2u) ^ (pc >> 13u)) &
                          (kDispatchCodeProbeCacheSize - 1u);
  auto& probe = dispatch_code_probe_cache_[slot];
  if (probe.pc != pc) { probe = {}; probe.pc = pc; }
  return probe;
}

void HostRuntime::ParkNativeCounterPoll(RunResult& result) {
  if(!native_counter_poll_enabled_ || !idle_event_wait_enabled_ || cpu_.exception ||
     ((cpu_.msr&kPpcMsrEe)!=0u && (pi_.InterruptPending() || decrementer_pending_))) return;
  const auto epoch=native_read_poll_generation_+dispatch_code_probe_generation_;
  if(native_counter_poll_epoch_!=epoch) {
    native_counter_polls_.fill({});native_counter_poll_epoch_=epoch;
  }
  const auto pc=cpu_.pc,lr=cpu_.lr;
  auto& entry=native_counter_polls_[((pc>>2u)^(lr>>2u)^(pc>>13u))&255u];
  const auto read_ram=[this](std::uint32_t ea,std::uint32_t* word) {
    const auto physical=AddressSpace::ToPhysical(ea);
    if((ea&3u) || memory_.Mem1().size()<4u || physical>memory_.Mem1().size()-4u) return false;
    return memory_.Read32(ea,word);
  };
  if(entry.pc!=pc || entry.lr!=lr) {
    entry={};entry.pc=pc;entry.lr=lr;
    const auto plan=NativeCounterPoll::DecodeBoundary(pc,lr,read_ram);
    if(plan) {entry.plan=*plan;entry.valid=true;}
  }
  if(!entry.valid) return;
  std::uint32_t limit;
  if(!read_ram(cpu_.gpr[13]+static_cast<std::uint32_t>(entry.plan.offset),&limit)) return;
  const auto mmcr0=spr_state_[952u],mmcr1=spr_state_[956u];
  const bool user=(cpu_.msr&0x4000u)!=0u,marked=(cpu_.msr&4u)!=0u;
  bool frozen=(mmcr0&0x80000000u) || (mmcr0&(user?0x20000000u:0x40000000u)) ||
              (mmcr0&(marked?0x10000000u:0x08000000u));
  if(entry.plan.spr!=953u && (mmcr0&0x2000u) && !(spr_state_[953u]&0x80000000u)) frozen=true;
  const auto selector=entry.plan.spr==953u ? (mmcr0>>6u)&127u :
      entry.plan.spr==954u ? mmcr0&63u : entry.plan.spr==957u ? (mmcr1>>27u)&31u : (mmcr1>>22u)&31u;
  const auto cycles=NativeCounterPoll::Cycles(spr_state_[entry.plan.spr],cpu_.gpr[entry.plan.baseline],limit,selector,frozen);
  static unsigned counter_proofs=0;
  if(runtime_profile_enabled_ && counter_proofs++<16u)
    std::fprintf(stderr,"GEKKOAOT_NATIVE_COUNTER_PROOF_V318=1 pc=%08x lr=%08x msr=%x limit=%x elapsed=%x selector=%u frozen=%u admitted=%u\n",
      pc,lr,cpu_.msr,limit,spr_state_[entry.plan.spr]-cpu_.gpr[entry.plan.baseline],selector,frozen?1u:0u,cycles?1u:0u);
  if(!cycles) return;
  ++native_counter_poll_parks_;
  if(native_counter_poll_parks_<=16u)
    std::fprintf(stderr,"GEKKOAOT_NATIVE_COUNTER_POLL_V318=1 pc=%08x lr=%08x spr=%u deadline_cycles=%llu parks=%llu\n",
      pc,lr,entry.plan.spr,static_cast<unsigned long long>(*cycles),static_cast<unsigned long long>(native_counter_poll_parks_));
  WaitForIdleEvent(result,*cycles,true);
  if(result.status!=RunStatus::HostRequestedExit) {
    TryTakeExternalInterrupt();TryTakeDecrementerInterrupt();
  }
}

void HostRuntime::ParkClockSpin(RunResult& result) {
  const auto lr=time_poll_return_,entry_pc=time_poll_entry_;
  const bool wide=time_poll_wide_;
  time_poll_return_=0u;
  const auto blocked=[this] {
    return !clock_spin_enabled_ || !idle_event_wait_enabled_ || cpu_.exception ||
           ((cpu_.msr&kPpcMsrEe)!=0u && (pi_.InterruptPending() || decrementer_pending_));
  };
  // Only a clock read repeated from the same site within microseconds is a
  // candidate; ordinary per-frame timing never pays for a probe.
  if(clock_spin_streak_<8u || cpu_.pc!=lr || blocked()) return;
  const auto epoch=native_read_poll_generation_+dispatch_code_probe_generation_;
  if(clock_spin_epoch_!=epoch) {clock_spins_.fill({});clock_spin_epoch_=epoch;}
  auto& entry=clock_spins_[((lr>>2u)^(lr>>8u))&(clock_spins_.size()-1u)];
  if(entry.lr!=lr || entry.entry!=entry_pc || entry.wide!=wide) {
    entry={};entry.lr=lr;entry.entry=entry_pc;entry.wide=wide;
  }
  if(entry.cooldown) {--entry.cooldown;return;}
  struct Memory {
    HostRuntime* self;
    bool Read(std::uint32_t ea,unsigned size,std::uint32_t* value) const {
      // Cached/uncached MEM1 only: MMIO, VM and locked cache end the probe.
      if((ea>>28u)!=0x8u && (ea>>28u)!=0xcu) return false;
      const auto& mem1=self->memory_.Mem1();
      const auto physical=AddressSpace::ToPhysical(ea);
      if(physical>=mem1.size() || mem1.size()-physical<size) return false;
      const auto* p=mem1.data()+physical;
      std::uint32_t v=0;
      for(unsigned i=0;i<size;++i) v=(v<<8u)|p[i];
      *value=v;
      return true;
    }
    bool Fetch(std::uint32_t ea,std::uint32_t* word) const {return !(ea&3u) && Read(ea,4u,word);}
    bool Foreign(std::uint32_t target) const {
      return self->os_hooks_.count(target)!=0u || self->simple_hle_hooks_.count(target)!=0u;
    }
  } memory{this};
  SpinProbeState state;
  for(unsigned i=0;i<32u;++i) state.gpr[i]=cpu_.gpr[i];
  state.cr=cpu_.cr;state.xer=cpu_.xer;state.lr=cpu_.lr;state.ctr=cpu_.ctr;
  const auto call_site=lr-4u;
  const auto returned=wide ? (std::uint64_t(cpu_.gpr[3])<<32u)|cpu_.gpr[4] : std::uint64_t(cpu_.gpr[3]);
  ++clock_spin_probes_;
  // The iteration the guest is about to run must come back to this clock call.
  if(spin_probe_.Run(state,lr,call_site,entry_pc,returned,wide,true,memory)!=SpinProbe::Outcome::Continue) {
    entry.cooldown=256u;
    return;
  }
  AdvanceRuntimeCycles(0u,result,true);
  if(result.status==RunStatus::HostRequestedExit || blocked()) return;
  const auto now=cpu_.timebase;
  const auto waits=[&](std::uint64_t t) {
    return spin_probe_.Run(state,lr,call_site,entry_pc,t,wide,false,memory)==SpinProbe::Outcome::Continue;
  };
  if(!waits(now)) return;
  std::uint64_t exit=0u;
  if(entry.exit>now && !waits(entry.exit) && waits(entry.exit-1u)) {
    exit=entry.exit;  // unchanged wait after an early (event) wake
  } else {
    const auto first=SpinProbe::FirstExit(now,GekkoAOT::Time::GameCubeTimebaseHz,waits);
    if(!first) {entry.cooldown=128u;return;}
    exit=entry.exit=*first;
  }
  ++clock_spin_parks_;
  if(clock_spin_parks_<=16u)
    std::fprintf(stderr,"GEKKOAOT_CLOCK_SPIN_V334=1 entry=%08x lr=%08x wide=%u wait_ticks=%llu parks=%llu\n",
      entry_pc,lr,wide?1u:0u,static_cast<unsigned long long>(exit-now),
      static_cast<unsigned long long>(clock_spin_parks_));
  WaitForIdleEvent(result,(exit-now)*GekkoAOT::Time::CpuCyclesPerTick,true);
  // The clock call returns when the park ends; the guest recomputes and
  // publishes its own state, and pending interrupts enter at the next boundary.
  const auto t=cpu_.timebase;
  if(wide) {cpu_.gpr[3]=static_cast<std::uint32_t>(t>>32u);cpu_.gpr[4]=static_cast<std::uint32_t>(t);}
  else cpu_.gpr[3]=static_cast<std::uint32_t>(t);
}

void HostRuntime::ParkYieldSpin() {
  // v344: OSYieldThread with no ready peer of equal or higher priority returns
  // at once, so a loop around it only ends when an interrupt, an alarm or a
  // thread woken by one changes the awaited state. Cars spins ~30M such
  // yields per second (7 guest cycles apart) on the main thread, inside one
  // AOT chain that only ends at the next interrupt: a full host core. After
  // 64 no-op yields from one site within 64 us, wait inside the yield for the
  // next hardware/timer event and deliver it here, exactly like the
  // OSDisableInterrupts entry fence: SRR0 is the yield's return address. The
  // cap bounds the overshoot of a wait on an inline mftb clock to 250 us.
  if (!active_result_ || !idle_event_wait_enabled_ || cpu_.exception ||
      (cpu_.msr & kPpcMsrEe) == 0u || pi_.InterruptPending() || decrementer_pending_) {
    ++yield_spin_declines_;
    return;
  }
  auto& result = *active_result_;
  const auto start = std::chrono::steady_clock::now();
  ++yield_spin_parks_;
  if (yield_spin_parks_ <= 8u)
    std::fprintf(stderr, "GEKKOAOT_YIELD_SPIN_V344=1 lr=%08x parks=%llu wait=next-event cap_us=250\n",
                 yield_noop_lr_, static_cast<unsigned long long>(yield_spin_parks_));
  constexpr std::uint64_t kMaxParkCycles = GekkoAOT::HW::VI::NativeVI::CpuClockHz / 4000u;
  WaitForIdleEvent(result, kMaxParkCycles, true);
  yield_spin_park_ns_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - start).count());
  if (result.status != RunStatus::HostRequestedExit && !TryTakeExternalInterrupt())
    TryTakeDecrementerInterrupt();
}

void HostRuntime::ThrottleGuestRate(RunResult& result) {
  // Opt-in (GEKKOAOT_GUEST_RATE_CEILING_PCT): cap charged guest cycles at a
  // percentage of 486 MHz. A token bucket with a 20 ms burst keeps per-frame
  // work unthrottled; once it is empty the guest parks on the same event-aware
  // wait as the idle paths, so interrupts and device events are never delayed.
  constexpr double kClockHz=static_cast<double>(GekkoAOT::HW::VI::NativeVI::CpuClockHz);
  const double rate=kClockHz*guest_rate_ceiling_pct_/100.0;
  const double burst=rate*0.020;
  const auto now=std::chrono::steady_clock::now();
  if(!rate_started_) {
    rate_started_=true;rate_tokens_=burst;
  } else {
    rate_tokens_=std::min(burst,rate_tokens_+rate*std::chrono::duration<double>(now-rate_mark_time_).count());
  }
  rate_tokens_-=static_cast<double>(rate_charged_-rate_mark_charged_);
  rate_mark_charged_=rate_charged_;rate_mark_time_=now;
  if(rate_tokens_>=0.0 || !idle_event_wait_enabled_ || cpu_.exception ||
     ((cpu_.msr&kPpcMsrEe)!=0u && (pi_.InterruptPending() || decrementer_pending_))) return;
  const auto wait=static_cast<std::uint64_t>(-rate_tokens_*100.0/guest_rate_ceiling_pct_)+1u;
  if(++rate_throttles_==1u)
    std::fprintf(stderr,"GEKKOAOT_GUEST_RATE_V333=1 ceiling_mhz=%.0f burst_ms=20 first_pc=%08x action=event-aware-wait\n",
                 rate/1e6,cpu_.pc);
  WaitForIdleEvent(result,wait,true);
  rate_throttle_ns_+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-now).count());
  if(result.status!=RunStatus::HostRequestedExit) {
    TryTakeExternalInterrupt();TryTakeDecrementerInterrupt();
  }
}

void HostRuntime::ParkNativeReadPoll(RunResult& result) {
  if(!native_read_poll_enabled_ || !idle_event_wait_enabled_ ||
     (cpu_.msr&kPpcMsrEe)==0u || cpu_.exception || pi_.InterruptPending() || decrementer_pending_) return;
  if(native_read_poll_epoch_!=native_read_poll_generation_+dispatch_code_probe_generation_) {
    native_read_poll_negatives_.fill(0u);
    native_read_polls_.fill({});
    native_read_poll_epoch_=native_read_poll_generation_+dispatch_code_probe_generation_;
  }
  const auto pc=cpu_.pc;
  const auto hash=(pc>>2u)^(pc>>13u);
  auto& negative=native_read_poll_negatives_[hash&(native_read_poll_negatives_.size()-1u)];
  if(negative==pc+1u) return;
  auto& entry=native_read_polls_[hash&(native_read_polls_.size()-1u)];
  const auto read_ram=[this](std::uint32_t ea,std::uint32_t* word) {
    // No MMIO, fake VM, misalignment or faulting access can be classified as
    // a pure RAM wait. All original AOT paths remain available for those cases.
    const auto physical=AddressSpace::ToPhysical(ea);
    if((ea&3u) || memory_.Mem1().size()<4u || physical>memory_.Mem1().size()-4u) return false;
    return memory_.Read32(ea,word);
  };
  if(entry.pc!=pc) {
    const auto plan=NativeReadPoll::Decode(pc,read_ram);
    if(!plan) {negative=pc+1u;return;}
    entry.pc=pc;entry.plan=*plan;
  }
  std::array<std::uint32_t,32> regs;
  std::copy_n(cpu_.gpr,32u,regs.begin());
  if(!entry.plan.Stable(pc,regs,cpu_.cr,cpu_.xer,read_ram)) return;
  ++native_read_poll_parks_;
  if(native_read_poll_parks_<=16u)
    std::fprintf(stderr,"GEKKOAOT_NATIVE_READ_POLL_V318=1 pc=%08x loop=%08x..%08x wait=host-event preserved=gpr+cr+ram parks=%llu\n",
      pc,entry.plan.begin,entry.plan.end,static_cast<unsigned long long>(native_read_poll_parks_));
  WaitForIdleEvent(result);
  if(result.status!=RunStatus::HostRequestedExit) {
    TryTakeExternalInterrupt();
    TryTakeDecrementerInterrupt();
  }
}

void HostRuntime::NoteMmioPoll(std::uint32_t address, std::uint64_t value) {
  // v347: a device status register read over and over with an unchanged value
  // is a hardware wait (HP Sorcerer's Stone: `while (ARGetDMAStatus());`, ~11M
  // DSPCR reads per second for every ARAM DMA). The reads happen inside one
  // AOT chain, so sleep right here until the next modelled device/timer event
  // (ARAM DMA completion, mailbox, DI/SI/EXI/AI deadlines), capped at 250 us.
  // Nothing is delivered or skipped: the guest re-reads the register after the
  // wait and observes the completion itself. Pending interrupts end the wait
  // immediately (they are delivered at the next boundary as before).
  static const bool trace = RuntimeEnvBool("GEKKOAOT_MMIO_POLL_TRACE", false);
  if (trace) {
    static std::uint64_t reads = 0; static unsigned window = 0, windows = 0;
    if ((++reads & 0xfffffu) == 0u && windows < 16u) { window = 24u; ++windows; }
    if (window) { --window; std::fprintf(stderr, "GEKKOAOT_MMIO_POLL_TRACE_V347 ea=%08x value=%llx msr=%x\n",
                                         address, static_cast<unsigned long long>(value), cpu_.msr); }
  }
  if (!mmio_poll_park_enabled_ || !active_result_ || !idle_event_wait_enabled_) return;
  if (address != mmio_poll_address_ || value != mmio_poll_value_) {
    mmio_poll_address_ = address; mmio_poll_value_ = value; mmio_poll_count_ = 0u;
    return;
  }
  if (++mmio_poll_count_ == 1u) mmio_poll_start_ = std::chrono::steady_clock::now();
  if (mmio_poll_count_ < 64u) return;
  const auto now = std::chrono::steady_clock::now();
  mmio_poll_count_ = 0u;
  if (now - mmio_poll_start_ > std::chrono::microseconds(64)) return;  // not a tight wait
  ++mmio_poll_parks_;
  if (mmio_poll_parks_ <= 8u)
    std::fprintf(stderr, "GEKKOAOT_MMIO_POLL_PARK_V347=1 ea=%08x value=%llx parks=%llu wait=next-event cap_us=250\n",
                 address, static_cast<unsigned long long>(value),
                 static_cast<unsigned long long>(mmio_poll_parks_));
  constexpr std::uint64_t kMaxParkCycles = GekkoAOT::HW::VI::NativeVI::CpuClockHz / 4000u;
  WaitForIdleEvent(*active_result_, kMaxParkCycles, true);
  mmio_poll_park_ns_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - now).count());
}

void HostRuntime::ParkFixedPointPoll(RunResult& result) {
  // v346: an AOT wait loop side-exits at its header whenever its cycle budget
  // runs out. When the same header keeps coming back, interpret one iteration
  // in the spin-probe sandbox; if it is a fixed point (same registers, only
  // re-stores of bytes RAM already holds, no HLE/MMIO/SPR/FP), nothing but an
  // interrupt, DMA or another thread can end it: park until the next event.
  // Nemo's `while (VIGetRetraceCount() - start < 2)` frame pacing burned ~40%
  // of its main thread this way.
  if(!fixed_point_park_enabled_ || !idle_event_wait_enabled_ || cpu_.exception ||
     (cpu_.msr&kPpcMsrEe)==0u || pi_.InterruptPending() || decrementer_pending_) return;
  const auto pc=cpu_.pc;
  // A wait loop that side-exits at its header comes straight back to it; any
  // other boundary in between means real work, not a candidate wait.
  const bool repeated=pc==fixed_point_last_pc_;
  fixed_point_last_pc_=pc;
  if(!repeated) return;
  auto& entry=fixed_points_[((pc>>2u)^(pc>>9u))&(fixed_points_.size()-1u)];
  if(entry.pc!=pc) entry={pc,0u,0u,0u,false};
  if(entry.cooldown) {--entry.cooldown;return;}
  // A proven wait site is re-probed at once: its failures are the iterations
  // that leave the wait, and the next wait must not spin through a cooldown.
  if(++entry.hits<(entry.proven ? 1u : 4u)) return;
  struct Memory {
    HostRuntime* self;
    bool Read(std::uint32_t ea,unsigned size,std::uint32_t* value) const {
      if((ea>>28u)!=0x8u && (ea>>28u)!=0xcu) return false;  // cached/uncached MEM1 only
      const auto& mem1=self->memory_.Mem1();
      const auto physical=AddressSpace::ToPhysical(ea);
      if(physical>=mem1.size() || mem1.size()-physical<size) return false;
      const auto* p=mem1.data()+physical;
      std::uint32_t v=0;
      for(unsigned i=0;i<size;++i) v=(v<<8u)|p[i];
      *value=v;
      return true;
    }
    bool Fetch(std::uint32_t ea,std::uint32_t* word) const {return !(ea&3u) && Read(ea,4u,word);}
    bool Foreign(std::uint32_t target) const {
      return self->os_hooks_.count(target)!=0u || self->simple_hle_hooks_.count(target)!=0u;
    }
  } memory{this};
  SpinProbeState state;
  for(unsigned i=0;i<32u;++i) state.gpr[i]=cpu_.gpr[i];
  state.cr=cpu_.cr;state.xer=cpu_.xer;state.lr=cpu_.lr;state.ctr=cpu_.ctr;
  ++fixed_point_probes_;
  std::array<std::uint32_t,3> why{};
  if(!spin_probe_.FixedPoint(state,pc,memory,&why)) {
    static const bool trace=RuntimeEnvBool("GEKKOAOT_FIXED_POINT_TRACE",false);
    static std::vector<std::uint32_t> traced;
    static const std::uint32_t trace_pc=[] {
      const char* v=std::getenv("GEKKOAOT_FIXED_POINT_TRACE_PC");
      return v ? static_cast<std::uint32_t>(std::strtoul(v,nullptr,16)) : 0u;
    }();
    static unsigned pc_logs=0u;
    if(trace && ((pc==trace_pc && pc_logs++<64u) ||
                 (traced.size()<4096u && std::find(traced.begin(),traced.end(),pc)==traced.end()))) {
      traced.push_back(pc);
      std::fprintf(stderr,"GEKKOAOT_FIXED_POINT_TRACE_V346 pc=%08x lr=%08x reason=%u at=%08x word=%08x\n",
                   pc,cpu_.lr,why[2],why[0],why[1]);
    }
    // Unproven sites back off exponentially (64 .. 1M re-entries).
    entry.cooldown=entry.proven ? 0u : (64u<<std::min<std::uint32_t>(entry.failures,14u));
    if(!entry.proven) ++entry.failures;
    entry.hits=0u;return;
  }
  entry.proven=true;entry.hits=0u;
  ++fixed_point_parks_;
  if(fixed_point_parks_<=8u || (fixed_point_parks_&1023u)==0u)
    std::fprintf(stderr,"GEKKOAOT_FIXED_POINT_POLL_V346=1 pc=%08x lr=%08x parks=%llu wait=next-event\n",
                 pc,cpu_.lr,static_cast<unsigned long long>(fixed_point_parks_));
  const auto park_start=std::chrono::steady_clock::now();
  WaitForIdleEvent(result);
  fixed_point_park_ns_+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now()-park_start).count());
  static const bool deliver=RuntimeEnvBool("GEKKOAOT_FIXED_POINT_DELIVER",true);
  if(deliver && result.status!=RunStatus::HostRequestedExit && !TryTakeExternalInterrupt())
    TryTakeDecrementerInterrupt();
}

void HostRuntime::ParkSdkIdlePoll(RunResult& result) {
  if (!sdk_idle_park_enabled_) return;

  // v181: this helper sits on every safe AOT boundary, but the SDK idle-poll
  // signature exists at only one/few PCs.  The old order first read the current
  // thread pointer and then decoded up to nine guest instructions for every
  // unrelated PC.  Cache exact-PC hits and misses in the same publication epoch
  // used by executable routing.  This preserves the boundary itself; only the
  // redundant recognition work disappears.
  const std::uint32_t pc = cpu_.pc;
  auto& probe = ProbeDispatchCode(pc);

  if (probe.idle_state == 0u) {
    const auto offset = GekkoAOT::NativeOS::SdkIdlePollOffset(pc,
        [this](std::uint32_t read_pc, std::uint32_t* word) {
          return memory_.Read32(read_pc, word);
        });
    if (!offset) {
      probe.idle_state = 1u;
      return;
    }
    probe.idle_offset = *offset;
    probe.idle_state = 2u;
    if (std::find(sdk_idle_probe_pcs_.begin(), sdk_idle_probe_pcs_.end(), pc) ==
        sdk_idle_probe_pcs_.end())
      sdk_idle_probe_pcs_.push_back(pc);
  } else if (probe.idle_state == 1u) {
    return;
  }

  // v335: the guest OS is idle, i.e. waiting for an interrupt (GXDrawDone waits
  // on the PE finish of commands it just queued). Publish the partial WGPIPE
  // batch now even when a pending exception keeps this boundary from parking:
  // GoldenEye's 10-tick job-worker alarms keep DEC pending almost permanently,
  // so its ~90 DrawDone/frame each waited ~4 ms for an unrelated sync point.
  native_gx_.PublishPendingWrites();
  if ((cpu_.msr & kPpcMsrEe) == 0u || pi_.InterruptPending() ||
      (decrementer_pending_ && !idle_dec_quantum_ticks_)) return;

  const std::int16_t offset = probe.idle_offset;
  if (sdk_idle_poll_pc_ != pc || sdk_idle_poll_offset_ != offset) {
    sdk_idle_poll_pc_ = pc;
    sdk_idle_poll_offset_ = offset;
    std::fprintf(stderr,
                 "GEKKOAOT_IDLE_PARK=1 pc=%08x runqueue_offset=%d deadline=vi-halfline guest-loop=retained\n",
                 pc, int(offset));
  }

  std::uint32_t current = 0;
  if (!memory_.Read32(0x800000e4u, &current) || current != 0u) return;
  std::uint32_t runnable = 0;
  const auto queue = cpu_.gpr[13] + static_cast<std::uint32_t>(offset);
  if (!memory_.Read32(queue, &runnable) || runnable != 0u) return;

  CountHostEffect();  // already parked by the SDK idle-poll recognizer
  // v355: with no runnable thread, a decrementer due again within the
  // coalescing quantum only re-enters this idle loop through a tiny alarm
  // (EA's THREAD_yieldticks(10) worker loop: ~300k DEC/s in MoH Rising Sun
  // and GoldenEye). Hold the DEC until the quantum ends or any other device
  // raises an event, as if the CPU had been busy for that long.
  if (idle_dec_quantum_ticks_ && idle_event_wait_enabled_) {
    const std::uint64_t since = cpu_.timebase - idle_dec_last_tb_;
    const std::uint64_t dec_ticks = static_cast<std::uint64_t>(spr_state_[kSprDec]) + 1ull;
    if (since < idle_dec_quantum_ticks_ && (decrementer_pending_ || dec_ticks < idle_dec_quantum_ticks_)) {
      ++idle_dec_coalesced_;
      WaitForIdleEvent(result, (idle_dec_quantum_ticks_ - since) * kTimerRatio, false, true);
      if (TryTakeExternalInterrupt()) return;
      if (TryTakeDecrementerInterrupt()) idle_dec_last_tb_ = cpu_.timebase;
      return;
    }
    if (decrementer_pending_) {
      TryTakeExternalInterrupt();
      if (TryTakeDecrementerInterrupt()) idle_dec_last_tb_ = cpu_.timebase;
      return;
    }
  }
  // v158: the guest SDK idle loop gets the same event-driven park as NativeOS.
  // The previous half-line wait woke tens of thousands of times per second and
  // left SelectThread/RunQueue polling as a visible perf hotspot. Sleep until
  // the earliest real IRQ/timer/device deadline (bounded by a host-maintenance
  // cap), then let the original guest poll/exception path continue unchanged.
  if (idle_event_wait_enabled_) {
    WaitForIdleEvent(result);
    if (decrementer_pending_ && idle_dec_quantum_ticks_) {
      TryTakeExternalInterrupt();
      if (TryTakeDecrementerInterrupt()) idle_dec_last_tb_ = cpu_.timebase;
      return;
    }
  } else {
    const auto cycles = vi_.CyclesUntilHalfLine();
    if (cycles == 0u) return;
    const auto duration = std::chrono::nanoseconds(
        (std::uint64_t(cycles) * 1000000000ull + HW::VI::NativeVI::CpuClockHz - 1u) /
        HW::VI::NativeVI::CpuClockHz);
    std::this_thread::sleep_until(std::chrono::steady_clock::now() + duration);
    AdvanceRuntimeCycles(0u, result, true);
  }
  TryTakeExternalInterrupt();
  TryTakeDecrementerInterrupt();
}

RunResult HostRuntime::Run(std::uint64_t dispatch_limit) {
  RunResult result{};
  // Host calls inside AOT chains (v344 yield parking) wait on this run.
  struct ActiveRun { RunResult*& slot; ~ActiveRun() { slot = nullptr; } } active_run{active_result_};
  active_result_ = &result;
  if (!module_.Descriptor()) {
    result.status = RunStatus::ModuleNotLoaded;
    return result;
  }

  const auto* descriptor = module_.Descriptor();
  fault_ = {};

  // Diagnostic: GEKKOAOT_STALL_WATCHDOG_S=<n> samples the (racy) guest
  // registers every n seconds from a host thread, so a guest loop that never
  // advances hardware time (no stats, no host exit) still shows where it is.
  // Diagnostic: GEKKOAOT_WATCH_ADDRS=a,b,... samples guest words every 200 us
  // and logs each change with the current OSThread and the (racy) PC.
  std::jthread memory_watch;
  if (const char* watch = std::getenv("GEKKOAOT_WATCH_ADDRS"); watch && *watch) {
    std::vector<std::uint32_t> addresses;
    for (const char* p = watch; *p;) {
      char* end = nullptr;
      const auto value = std::strtoul(p, &end, 16);
      if (end == p) break;
      addresses.push_back(static_cast<std::uint32_t>(value));
      p = *end == ',' ? end + 1 : end;
    }
    memory_watch = std::jthread([this, addresses](std::stop_token stop) {
      std::vector<std::uint32_t> last(addresses.size(), 0xdeadbeefu);
      unsigned logs = 0;
      while (!stop.stop_requested() && logs < 20000u) {
        for (std::size_t i = 0; i < addresses.size(); ++i) {
          std::uint32_t value = 0, thread = 0;
          if (!memory_.Read32(addresses[i], &value) || value == last[i]) continue;
          memory_.Read32(0x800000e4u, &thread);
          last[i] = value;
          ++logs;
          std::fprintf(stderr, "GEKKOAOT_WATCH addr=%08x value=%08x thread=%08x pc=%08x lr=%08x tb=%llx\n",
                       addresses[i], value, thread, cpu_.pc, cpu_.lr,
                       static_cast<unsigned long long>(cpu_.timebase));
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
    });
  }

  std::jthread stall_watchdog;
  if (const std::uint32_t period = RuntimeEnvU32("GEKKOAOT_STALL_WATCHDOG_S", 0u, 3600u)) {
    stall_watchdog = std::jthread([this, period](std::stop_token stop) {
      unsigned samples = 0;
      while (!stop.stop_requested() && samples < 600u) {
        for (std::uint32_t i = 0; i < period * 10u && !stop.stop_requested(); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (stop.stop_requested()) break;
        const CPUState snapshot = cpu_;
        std::fprintf(stderr,
                     "GEKKOAOT_STALL_WATCHDOG n=%u pc=%08x lr=%08x ctr=%08x msr=%08x r1=%08x r3=%08x r4=%08x r5=%08x r6=%08x r31=%08x tb=%llx "
                     "pi=%05x/%04x dsp=%04x aid_ctl=%04x aid_debt=%llu aid_irqs=%llu\n",
                     ++samples, snapshot.pc, snapshot.lr, snapshot.ctr, snapshot.msr,
                     snapshot.gpr[1], snapshot.gpr[3], snapshot.gpr[4], snapshot.gpr[5],
                     snapshot.gpr[6], snapshot.gpr[31],
                     static_cast<unsigned long long>(snapshot.timebase),
                     pi_.InterruptCauseValue(), pi_.InterruptMaskValue(), dsp_.Control(),
                     dsp_.AudioDmaControl(),
                     static_cast<unsigned long long>(dsp_.AudioDmaPendingBlocks()),
                     static_cast<unsigned long long>(dsp_.AidInterruptCount()));
      }
    });
  }

  // Device MMIO writes and AdvanceRuntimeCycles keep PI causes synchronized.
  // Seed the initial state once, rather than polling every device again before
  // every dispatch (the old loop duplicated the end-of-dispatch synchronization).
  if (!run_banner_reported_)
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_CP_FIFO_V162=1 drain=batched-until-breakpoint breakpoint=exact-stop+irq fifo-top=base+size-4 wrap=exact\n");
  SyncNativeCPInterrupt();
  SyncNativeDSPInterrupt();
  SyncNativeAIInterrupt();
  SyncNativeDIInterrupt();
  SyncNativeEXIInterrupt();
  SyncNativeSIInterrupt();

  const std::uint32_t native_idle_sleep_us =
      RuntimeEnvU32("GEKKOAOT_NATIVE_IDLE_SLEEP_US", 0u, 1000u);
  sdk_idle_park_enabled_ = RuntimeEnvBool("GEKKOAOT_IDLE_PARK", true);
  native_read_poll_enabled_=RuntimeEnvBool("GEKKOAOT_NATIVE_READ_POLL",false);
  native_counter_poll_enabled_=RuntimeEnvBool("GEKKOAOT_NATIVE_COUNTER_POLL",false);
  clock_spin_enabled_=RuntimeEnvBool("GEKKOAOT_CLOCK_SPIN_PARK",true);
  yield_spin_enabled_=RuntimeEnvBool("GEKKOAOT_YIELD_SPIN_PARK",true);
  fixed_point_park_enabled_=RuntimeEnvBool("GEKKOAOT_FIXED_POINT_PARK",true);
  mmio_poll_park_enabled_=RuntimeEnvBool("GEKKOAOT_MMIO_POLL_PARK",true);
  idle_dec_quantum_ticks_=static_cast<std::uint64_t>(RuntimeEnvU32("GEKKOAOT_IDLE_DEC_QUANTUM_US",100u,10000u))*
                          (GekkoAOT::HW::VI::NativeVI::CpuClockHz/kTimerRatio)/1000000u;
  // Opt-in power cap: generated code's cycle charge is not a Gekko cycle count
  // (DNDD01 legitimately charges ~1.9 GHz at 60 fps and drops to 38 fps under
  // a 200% cap), so no default ceiling is safe.
  guest_rate_ceiling_pct_=RuntimeEnvU32("GEKKOAOT_GUEST_RATE_CEILING_PCT",0u,100000u);
  aot_poll_park_enabled_ = RuntimeEnvBool("GEKKOAOT_AOT_POLL_PARK", true);
  leaf_tail_enabled_ = RuntimeEnvBool("GEKKOAOT_LEAF_TAIL", true);
  // Opt-in until measured: the SFA A/B showed no boundary change (the
  // matrix HLE may reach the host through another callback).
  gx_matrix_chain_enabled_ = RuntimeEnvBool("GEKKOAOT_GX_MATRIX_CHAIN", false);
  ee_pending_query_enabled_ = RuntimeEnvBool("GEKKOAOT_EE_PENDING_QUERY", true);
  boundary_profile_enabled_ = RuntimeEnvBool("GEKKOAOT_BOUNDARY_PROFILE", false);
  // Diagnostic counters (boundaries, host effects, EE queries, leaf tails,
  // charged guest cycles) cost nothing unless explicitly requested.
  runtime_profile_enabled_ =
      boundary_profile_enabled_ || RuntimeEnvBool("GEKKOAOT_RUNTIME_PROFILE", false);
  // Opt-in: parking proven no-op loops shifts guest timing enough to expose a
  // timing-sensitive loader race in Luigi's Mansion (3 crashes in 13 runs of
  // the foyer scene versus 0 in 12 with the prover off).
  idle_loop_proof_enabled_ =
      idle_loop_proof_enabled_ && RuntimeEnvBool("GEKKOAOT_IDLE_LOOP_PROOF", false);
  track_host_effects_ = idle_loop_proof_enabled_ || runtime_profile_enabled_;
  idle_event_wait_enabled_ = RuntimeEnvBool("GEKKOAOT_IDLE_EVENT_WAIT", true);
  idle_event_max_wait_us_ = RuntimeEnvU32("GEKKOAOT_IDLE_EVENT_MAX_US", 2000u, 10000u);
  if (!run_banner_reported_) {
    std::fprintf(stderr,
                 "GEKKOAOT_RUNTIME_PERF_V159=1 clock=tsc-or-steady zero-cycle=fast-return external-pointer=ram-first mmio=page-dispatch wgpipe=be-fixed irq-probe=single-load nativeos=o1-best+priority-coherent safe-chain=%lld spin-yield=off context-drop=off idle-sleep-us=%u event-wait=%u event-max-us=%u aot-poll-park=%u force-sample=real\n",
                 static_cast<long long>(cpu_.cycle_budget), native_idle_sleep_us,
                 idle_event_wait_enabled_ ? 1u : 0u, idle_event_max_wait_us_,
                 aot_poll_park_enabled_ ? 1u : 0u);
    run_banner_reported_ = true;
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_EXCEPTION_VECTOR_POLICY_V294=%u default=off cache=icbi+icfi ice-off=original-interpreter\n",
                 native_low_vector_enabled_ ? 1u : 0u);
  }

  // v276: the NativeOS service is a process singleton; resolve it once instead
  // of twice per host boundary.
  auto& native_os = GekkoAOT::NativeOS::Service::Get();
  while (dispatch_limit == 0 || result.dispatches < dispatch_limit) {
    if (cpu_.pc == 0u) {
      result.status = RunStatus::Completed;
      break;
    }

    // v321: a DSI latch must be consumed by entering the 0x300 vector. A host
    // service that lowered a fault and then redirected pc leaves generated code
    // unwinding at every entry forever; report that state once.
    if ((cpu_.exception & kPpcExcDsi) != 0u && !stale_dsi_reported_) {
      NoteDsiTrace('B', cpu_.pc, cpu_.msr);
      if (!IsLowExceptionVectorAddress(AddressSpace::ToPhysical(cpu_.pc))) {
        stale_dsi_reported_ = true;
        std::fprintf(stderr,
                     "GEKKOAOT_VM_DSI_STALE_LATCH_V321=1 pc=%08x lr=%08x msr=%08x raise_pc=%08x raise_lr=%08x raise_msr=%08x dar=%08x srr0=%08x\n",
                     cpu_.pc, cpu_.lr, cpu_.msr, last_dsi_pc_, last_dsi_lr_, last_dsi_msr_,
                     cpu_.dar, cpu_.srr0);
        for (std::uint32_t k = 0; k < 16u; ++k) {
          const auto i = (dsi_trace_next_ + k) & 15u;
          if (dsi_trace_tag_[i])
            std::fprintf(stderr, "GEKKOAOT_VM_DSI_TRACE_V321=1 tag=%c pc=%08x exc=%x\n",
                         static_cast<char>(dsi_trace_tag_[i]), dsi_trace_pc_[i], dsi_trace_exc_[i]);
        }
      }
    }
    if (runtime_profile_enabled_) {
      ++boundaries_total_;
      // v274: GEKKOAOT_BOUNDARY_PROFILE=1 histograms host boundaries by guest
      // PC/LR and prints the top entries with the perf metrics line.
      if (boundary_profile_enabled_)
        ++boundary_profile_[(std::uint64_t(cpu_.pc) << 32u) | cpu_.lr];
    }
    if (idle_loop_proof_enabled_) {
      ObserveIdleLoop(result);
      if (result.status == RunStatus::HostRequestedExit) break;
    }

    std::uint32_t halt_entry = 0;
    if (!ppc_halt_pcs_.empty() && cpu_.pc >= ppc_halt_pcs_.front().pc &&
        cpu_.pc <= ppc_halt_pcs_.back().pc && DetectSdkPpcHalt(cpu_.pc, &halt_entry)) {
      if (compat_diagnostics_enabled_)
        RecordCompatBreadcrumb(cpu_.pc, CompatDispatchKind::Main);
      ReportCompatFailure("guest-halt", halt_entry, 0u, false);
      result.status = RunStatus::GuestHalted;
      result.fault_address = halt_entry;
      break;
    }

    if (native_os.IsIdle() && cpu_.pc == native_os.IdlePc()) {
      CountHostEffect();  // NativeOS owns this idle loop and parks it itself
      if (compat_diagnostics_enabled_)
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

      // v335: idle means waiting for an interrupt; make queued GPU work visible
      // before delivering a pending one (see ParkSdkIdlePoll).
      native_gx_.PublishPendingWrites();
      // v183: pending IRQ/DEC latches are almost always clear while the guest
      // is idle. Avoid entering the full exception helpers on that common path.
      if ((pi_.InterruptPending() && TryTakeExternalInterrupt()) ||
          (decrementer_pending_ && TryTakeDecrementerInterrupt()))
        continue;

      if (idle_event_wait_enabled_) {
        // v158: no runnable OSThread means there is no useful PPC work until a
        // hardware/timer/alarm event occurs. Park the host once instead of
        // burning a core on Run()->AdvanceRuntimeCycles() 4096-cycle probes.
        WaitForIdleEvent(result);
      } else {
        constexpr std::uint64_t kNativeIdleMaxQuantum = 4096u;
        const std::uint64_t idle_quantum = std::max<std::uint64_t>(
            1u, std::min<std::uint64_t>(kNativeIdleMaxQuantum, vi_.TicksPerHalfLine()));
        if (native_idle_sleep_us != 0u)
          std::this_thread::sleep_for(std::chrono::microseconds(native_idle_sleep_us));
        AdvanceRuntimeCycles(idle_quantum, result, true);
      }
      ++result.dispatches;
      if (result.status == RunStatus::HostRequestedExit) break;
      continue;
    }

    // At a dispatch boundary cpu_.pc is the next guest instruction, matching
    // the architectural SRR0 value for an asynchronous external interrupt.
    // Do this before NativeOS/HLE dispatch so the real low-memory 0x500 vector
    // owns interrupt delivery and can wake the guest scheduler normally.
    // v183: keep exact per-boundary interrupt visibility, but make the normal
    // no-event boundary two cheap latch tests instead of two helper calls.
    if (pi_.InterruptPending()) TryTakeExternalInterrupt();
    if (decrementer_pending_) TryTakeDecrementerInterrupt();
    if (time_poll_return_ != 0u) ParkClockSpin(result);
    if (guest_rate_ceiling_pct_ != 0u && rate_charged_ - rate_mark_charged_ >= 65536u)
      ThrottleGuestRate(result);
    ParkSdkIdlePoll(result);
    ParkNativeCounterPoll(result);
    ParkNativeReadPoll(result);
    ParkFixedPointPoll(result);
    if (result.status == RunStatus::HostRequestedExit) break;

    const std::uint32_t native_os_pc = cpu_.pc;
    if (native_os.Poll(&cpu_, native_os_pc)) {
      if (compat_diagnostics_enabled_)
        RecordCompatBreadcrumb(native_os_pc, CompatDispatchKind::NativeHle);
      ++hle_calls_;
      continue;
    }

    cpu_.downcount = 0;
    cpu_.external_read_count = 0;
    cpu_.external_write_count = 0;
    const std::uint32_t dispatch_pc = cpu_.pc;
    if (compat_diagnostics_enabled_)
      RecordCompatBreadcrumb(dispatch_pc, CompatDispatchKind::Main);

    std::uint64_t low_memory_cycles = 0;
    // v182: low-memory vectors are exceptional. Avoid entering the sizeable
    // interpreter helper at every ordinary MEM1 function boundary; the cheap
    // page-mask prefilter is identical to the helper's first semantic test.
    const std::uint32_t dispatch_physical = AddressSpace::ToPhysical(dispatch_pc);
    bool native_low_vector = false;
    if (IsLowExceptionVectorAddress(dispatch_physical) &&
        ((native_low_vector = ExecuteNativeLowMemoryVector(dispatch_pc, &low_memory_cycles)) ||
         ExecuteLowMemoryInstruction(dispatch_pc, &low_memory_cycles))) {
      // v276: first-level exception vectors run with MSR[EE]=0, so no
      // interrupt, NativeOS alarm or idle park can be taken between their
      // instructions; every per-boundary check below is a no-op for them.
      // Execute the rest of the vector in one host boundary instead of one
      // full Run iteration per instruction. Stop exactly where the old loop
      // could observe something: leaving the vector pages (rfi/branch to the
      // OS handler), MSR[EE] becoming 1, a fault, or an instruction the
      // low-memory executor does not handle (left to the next iteration).
      for (std::uint32_t step = native_low_vector ? 34u : 1u; step < 256u; ++step) {
        if (fault_.kind != FaultKind::None || (cpu_.msr & kPpcMsrEe) != 0u) break;
        const std::uint32_t next_pc = cpu_.pc;
        if (!IsLowExceptionVectorAddress(AddressSpace::ToPhysical(next_pc))) break;
        std::uint64_t step_cycles = 0u;
        if (!ExecuteLowMemoryInstruction(next_pc, &step_cycles)) break;
        low_memory_cycles += step_cycles;
      }
      if (AddressSpace::ToPhysical(dispatch_pc) == 0x0300u)
        NoteDsiTrace('V', cpu_.pc, cpu_.exception);
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
      AdvanceRuntimeCyclesFast(low_memory_cycles, result);
      if (result.status == RunStatus::HostRequestedExit) break;
      continue;
    }

    // A verified live REL range outside main.dol cannot be handled by its
    // dispatcher. Keep the normal event/HLE boundary above, then dispatch the
    // known owner directly instead of paying a main-dispatch miss first.
    if (!has_fixed_secondary_modules_ && !rel_mapping_dirty_ &&
        cached_rel_route_.exclusive_from_main && cached_rel_route_.owner &&
        dispatch_pc >= cached_rel_route_.runtime_start &&
        dispatch_pc < cached_rel_route_.runtime_end &&
        DispatchSecondaryModule(dispatch_pc, result)) {
      if (fault_.kind != FaultKind::None) {
        result.status = FaultStatus();
        result.fault_address = fault_.address;
        result.fault_value = fault_.value;
        break;
      }
      if (result.status == RunStatus::HostRequestedExit) break;
      continue;
    }

    // v170: some retail SDK builds leave OSGetTime/OSGetTick as tiny leaf
    // helpers instead of exposing a resolver-friendly symbol. Executing these
    // through overlapping fixed executable images can turn an otherwise trivial
    // timebase read into a permanent busy loop.
    //
    // v181: safe host boundaries can revisit the same PCs millions of times.
    // The previous miss path Resolve()'d and decoded two guest instructions on
    // every boundary even though almost every PC is not a time helper. Cache the
    // exact-PC classification in the executable publication epoch. This keeps
    // the compatibility HLE semantics unchanged while turning repeated misses
    // into a direct-mapped cache lookup.
    {
      auto& probe = ProbeDispatchCode(dispatch_pc);

      if (probe.time_kind == 0u) {
        std::uint32_t op0=0u, op1=0u, op2=0u, op3=0u, op4=0u, op5=0u;
        const auto* time_head = fast_time_probe_enabled_
            ? memory_.Resolve(dispatch_pc, 8u)
            : nullptr;
        bool have_time = false;
        if (time_head) {
          op0 = ReadGuestBE32(time_head);
          op1 = ReadGuestBE32(time_head + 4u);
          have_time = true;
        } else {
          have_time = memory_.Read32(dispatch_pc + 0u, &op0) &&
                      memory_.Read32(dispatch_pc + 4u, &op1);
        }

        if (have_time && op0 == 0x7c6c42e6u && op1 == 0x4e800020u) {
          probe.time_kind = 2u;
        } else if (have_time && op0 == 0x7c6d42e6u && op1 == 0x7c8c42e6u) {
          bool have_tail = false;
          const auto* time_full = fast_time_probe_enabled_
              ? memory_.Resolve(dispatch_pc, 24u)
              : nullptr;
          if (time_full) {
            op2 = ReadGuestBE32(time_full + 8u);
            op3 = ReadGuestBE32(time_full + 12u);
            op4 = ReadGuestBE32(time_full + 16u);
            op5 = ReadGuestBE32(time_full + 20u);
            have_tail = true;
          } else {
            have_tail = memory_.Read32(dispatch_pc + 8u, &op2) &&
                        memory_.Read32(dispatch_pc + 12u, &op3) &&
                        memory_.Read32(dispatch_pc + 16u, &op4) &&
                        memory_.Read32(dispatch_pc + 20u, &op5);
          }
          if (have_tail && op2 == 0x7cad42e6u && op3 == 0x7c032800u &&
              op4 == 0x4082fff0u && op5 == 0x4e800020u)
            probe.time_kind = 3u;
          else if (have_tail)
            probe.time_kind = 1u;
        } else if (have_time) {
          probe.time_kind = 1u;
        }
        // v274: classify exact integer leaf tails (see TryExecuteLeafTail).
        if (probe.time_kind == 1u && leaf_tail_enabled_ &&
            TryExecuteLeafTail(dispatch_pc, false) != 0u)
          probe.time_kind = 4u;
      }

      if (probe.time_kind == 4u) {
        // Re-decoded on every use, so a stale classification can only fall
        // back to the module; it can never execute different guest bytes.
        if (const std::uint32_t retired = TryExecuteLeafTail(dispatch_pc, true)) {
          ++result.dispatches;
          if (runtime_profile_enabled_) ++leaf_tail_executions_;
          AdvanceRuntimeCyclesFast(retired, result);
          if (result.status == RunStatus::HostRequestedExit) break;
          continue;
        }
        probe.time_kind = 1u;
      }

      if (probe.time_kind == 2u) {
        cpu_.gpr[3] = static_cast<std::uint32_t>(cpu_.timebase);
        cpu_.pc = cpu_.lr;
        ++result.dispatches;
        ++hle_calls_;
        AdvanceRuntimeCyclesFast(2u, result);
        continue;
      }
      if (probe.time_kind == 3u) {
        const std::uint64_t tb = cpu_.timebase;
        cpu_.gpr[3] = static_cast<std::uint32_t>(tb >> 32u);
        cpu_.gpr[4] = static_cast<std::uint32_t>(tb);
        cpu_.pc = cpu_.lr;
        ++result.dispatches;
        ++hle_calls_;
        AdvanceRuntimeCyclesFast(6u, result);
        continue;
      }
    }

    // v84: fixed DOL/ELF executables may replace each other at identical
    // GameCube addresses. Address coverage alone cannot identify the live image;
    // verify the current MEM1 chunk against build-time hashes before allowing a
    // stale main/side module to win an overlap. Unique-address code stays on the
    // existing zero-hash hot path.
    bool fixed_overlap = false;
    const GekkoAOTModuleDesc* fixed_descriptor = nullptr;
    // The selector is intentionally heavyweight because it verifies overlapping
    // fixed images. Most titles have none. Keep their boundary path to one
    // predictable boolean branch instead of calling the router just to discover
    // that fact again.
    if (has_fixed_secondary_modules_)
      fixed_descriptor = SelectFixedExecutableModule(dispatch_pc, &fixed_overlap);
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
      std::uint64_t copied_cache_cycles=0;
      if (fault_.kind == FaultKind::None &&
          DispatchCopiedCacheStub(dispatch_pc, &copied_cache_cycles)) {
        ++result.dispatches;
        AdvanceRuntimeCycles(copied_cache_cycles, result);
        if (result.status == RunStatus::HostRequestedExit) break;
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
      AdvanceRuntimeCyclesFast(charged, result);
      if (result.status == RunStatus::HostRequestedExit) break;
    }
    ConsumeAotPollPark(result);
    if (result.status == RunStatus::HostRequestedExit) break;

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
  result.idle_event_waits = idle_event_waits_;
  result.idle_event_requested_cycles = idle_event_requested_cycles_;
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

  if (result.status != RunStatus::Completed && result.status != RunStatus::HostRequestedExit) {
    // PowerPC EABI frames are chained through 0(r1). The callee normally
    // saves its incoming LR in the caller linkage area at caller_sp + 4.
    // Walking that linkage after a terminal SDK PPCHalt reveals the routine
    // that entered OSPanic, and after a fault the guest callers of the
    // faulting routine, without changing guest control flow.
    std::uint32_t frame_sp = result.stack_pointer;
    for (std::size_t depth = 0; depth < result.halt_frame_sp.size(); ++depth) {
      std::uint32_t caller_sp = 0;
      if (!memory_.Read32(frame_sp, &caller_sp)) break;
      if (caller_sp <= frame_sp || (caller_sp & 3u) != 0u ||
          caller_sp - frame_sp > 0x00100000u)
        break;
      std::uint32_t caller_lr = 0;
      if (!memory_.Read32(caller_sp + 4u, &caller_lr)) break;
      result.halt_frame_sp[depth] = frame_sp;
      result.halt_frame_caller_lr[depth] = caller_lr;
      result.halt_frame_count = static_cast<std::uint8_t>(depth + 1u);
      frame_sp = caller_sp;
    }
  }

  if (result.fault_address == 0 && fault_.kind != FaultKind::None)
    result.fault_address = fault_.address;
  if (result.fault_value == 0 && fault_.kind != FaultKind::None)
    result.fault_value = fault_.value;
  const double guest_fps = produced_frame_period_.count() > 0
      ? 1000000000.0 / static_cast<double>(produced_frame_period_.count())
      : 0.0;
  if (result.status != RunStatus::DispatchLimitReached) std::fprintf(stderr,
               "GEKKOAOT_HOST_FRAME_STATS_V48 target_hz=%u guest_fps=%.3f presents=%llu interpolated=%llu direct=%llu collapsed=%llu\n",
               host_fps_limit_hz_, guest_fps,
               static_cast<unsigned long long>(host_present_count_),
               static_cast<unsigned long long>(interpolated_present_count_),
               static_cast<unsigned long long>(direct_present_count_),
               static_cast<unsigned long long>(host_present_dropped_));
  return result;
}

} // namespace GekkoAOT::Native
