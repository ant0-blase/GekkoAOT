// SPDX-License-Identifier: GPL-3.0-or-later
// Private to the HostRuntime implementation (runtime*.cpp): shared includes and
// stateless helpers. The anonymous namespaces give every translation unit its
// own copy, so nothing here may hold state.
#pragma once
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "sdk/gx_pixel_service.h"
#include "sdk/angular_camera_proof.h"
#include "sdk/j3d_frustum_proof.h"
#include "gx/j3d_frustum.h"
#include "sdk/tangent_camera_proof.h"
#include "sdk/reciprocal_camera_proof.h"
#include "gx/projection_fov.h"
#include "gx/angular_camera.h"
#include "sdk/dvd_fst_service.h"
#include "sdk/gx_array_service.h"
#include "native/runtime.h"
#include "native/ea_mpeg_hle.h"
#include "native/hle/moh_particles.h"
#include "native/atomic_file.h"
#include "perf/capture.h"
#include "native/cpu_timer_policy.h"
#include "perf/latency.h"
#include "sdk/device_proofs.h"
#include <bitset>
#include "native/sdk_autoresolver.h"
#include "native/rel_identity.h"
#if defined(GEKKOAOT_NATIVE_SDL_INPUT)
#include "platform/sdl_input.h"
#endif

#include "core/time.h"
#include "gx/frustum_assist.h"
#include "os/sdk_idle_poll.h"
#include "hw/si/host_si.h"
#include "hw/di/host_di.h"
#include "hw/vi/host_vi.h"
#include "hw/ai/host_ai.h"
#include "hw/pi/host_pi.h"
#include "hw/mi/host_mi.h"
#include "hw/cp/host_cp.h"
#include "hw/dsp/host_dsp.h"
#include "hw/events.h"
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
#if defined(_WIN32)
// <windows.h> (via the SDL input header) defines these as macros; they
// collide with SDK service names, GekkoAOT::Native::ReplaceFile and locals.
#undef SendMessage
#undef ReplaceFile
#undef near
#undef far
#endif

namespace GekkoAOT::Native {

namespace {
constexpr std::uint32_t kNativeRegionProbe = 0xfffffffcu;
constexpr std::uint32_t kRelAddressProbe = 0xfffffffau;
constexpr std::uint32_t kGlobalIndirectProbe = 0xfffffff9u;
// Asked by generated mtmsr code when MSR[EE] is set (DolRecomp patch
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


// REL/native-region dispatch scratch belongs to one HostRuntime/CPU, not
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

// Per-call environment scans on hot service/DI paths (each getenv walks
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
  // Keep safe-host-boundary mode bounded, but do not bounce through the
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

// GEKKOAOT_NATIVE_SERVICE_DISABLE=memset,strlen,... (or "all") makes the
// named SDK leaf services decline so the original AOT guest body runs instead.
// A compatibility/bisect switch that needs no recompile of the module.
const auto& DisabledSimpleServices() {
  static const auto mask = [] {
    std::bitset<static_cast<unsigned>(SimpleHleKind::Count)> bits;
    const char* list = std::getenv("GEKKOAOT_NATIVE_SERVICE_DISABLE");
    if (!list || !*list) return bits;
    std::string_view rest(list);
    while (!rest.empty()) {
      const auto comma = rest.find(',');
      const auto name = rest.substr(0, comma);
      for (unsigned kind = 0; kind < static_cast<unsigned>(SimpleHleKind::Count); ++kind)
        if (name == "all" || name == SimpleServiceName(kind)) bits.set(kind);
      if (comma == std::string_view::npos) break;
      rest.remove_prefix(comma + 1u);
    }
    std::fprintf(stderr, "GEKKOAOT_NATIVE_SERVICE_DISABLE_V301=1 count=%zu\n", bits.count());
    return bits;
  }();
  return mask;
}

// Static-direct libc tokens whose only effects are guest GPRs and plain
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

// SDK matrix uploads (GXLoad*MtxImm) replace guest code whose only
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
  // This is queried before almost every normal AOT dispatch, while an
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
  // Synchronous guest exceptions are rare and usually end in a game's
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

// Hot admission check shared by the SDK service table (runtime_sdk.cpp) and
// the host-call dispatcher (runtime_cpu.cpp); always inlined into both.
#if defined(_MSC_VER)
__forceinline
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline)) inline
#endif
bool HostRuntime::AdmitNativeSdkKind(SimpleHleKind kind) {
  const auto slot=((cpu_.pc>>2u)^(cpu_.pc>>13u))&(kHleFastCacheSize-1u);
  const auto cached=hle_fast_cache_[slot];
  if(cached.address==cpu_.pc && cached.type==HleFastType::Simple && cached.kind==unsigned(kind) &&
     (!has_fixed_secondary_modules_ ||
      // These leaves prove every current code byte before accessing GXData.
      (kind>=SimpleHleKind::GxSetAlphaCompare && kind<=SimpleHleKind::GxSetAlphaUpdate) ||
      NativeHookCodeMatches(cpu_.pc)))return true;
  return AdmitNativeSdkKindSlow(kind);
}

} // namespace GekkoAOT::Native

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
