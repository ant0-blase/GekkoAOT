#include "sdk/gx_pixel_service.h"
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdk/dvd_fst_service.h"
#include "sdk/gx_normal_service.h"
#include "sdk/gx_array_service.h"
#include "sdk/angular_camera_proof.h"
#include "sdk/tangent_camera_proof.h"
#include "sdk/j3d_frustum_proof.h"
#include "native/sdk_autoresolver.h"
#include "sdk/device_proofs.h"
#include "sdk/card_sync_wrapper.h"

#include "native/runtime.h"
#include "sdk/os_primitives.h"
#include "sdk/debug_corpus_signatures_v33.h"
#include "sdk/sdk_service_corpus_v1.h"
#include "sdk/native_console_corpus.h"
#include "sdk/native_pad2001_corpus.h"
#include "sdk/native_card2001_corpus.h"
#include "sdk/sdk_groundtruth_v1.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GekkoAOT::Native {
namespace {

using Resolver = GekkoAOTSdk::StructuralResolver;
using OsKind = GekkoAOT::NativeOS::Kind;

bool EnvEnabled(const char* name, bool fallback = true) {
  const char* value = std::getenv(name);
  if (!value || !*value) return fallback;
  const std::string_view text(value);
  return text != "0" && text != "false" && text != "off" && text != "no";
}

std::string_view EnvText(const char* name, std::string_view fallback) {
  const char* value = std::getenv(name);
  return value && *value ? std::string_view(value) : fallback;
}

constexpr std::array<OsKind, 51> kAllOsKinds = {
    OsKind::InitThreadQueue, OsKind::GetCurrentThread, OsKind::IsThreadTerminated,
    OsKind::DisableScheduler, OsKind::EnableScheduler, OsKind::SelectThread,
    OsKind::Reschedule, OsKind::YieldThread,
    OsKind::CreateThread, OsKind::ExitThread, OsKind::CancelThread,
    OsKind::JoinThread, OsKind::DetachThread, OsKind::ResumeThread,
    OsKind::SuspendThread, OsKind::SleepThread, OsKind::WakeupThread,
    OsKind::SetThreadPriority, OsKind::GetThreadPriority, OsKind::SaveContext, OsKind::LoadContext,
    OsKind::ClearContext, OsKind::InitContext, OsKind::GetCurrentContext,
    OsKind::SetCurrentContext, OsKind::LoadFpuContext, OsKind::SaveFpuContext,
    OsKind::FillFpuContext, OsKind::GetStackPointer, OsKind::SwitchStack, OsKind::InitMutex, OsKind::LockMutex,
    OsKind::UnlockMutex, OsKind::TryLockMutex, OsKind::InitCond,
    OsKind::WaitCond, OsKind::SignalCond, OsKind::InitMessageQueue,
    OsKind::SendMessage, OsKind::ReceiveMessage, OsKind::JamMessage,
    OsKind::InitAlarm, OsKind::CreateAlarm, OsKind::SetAlarm,
    OsKind::SetAbsAlarm, OsKind::SetPeriodicAlarm, OsKind::CancelAlarm,
    OsKind::CheckAlarmQueue, OsKind::DisableInterrupts, OsKind::EnableInterrupts,
    OsKind::RestoreInterrupts,
};

std::optional<OsKind> OsKindForName(std::string_view name) {
  if (name == "__OSInitThreadQueue") return OsKind::InitThreadQueue;
  if (name == "__OSGetCurrentThread") return OsKind::GetCurrentThread;
  for (const auto kind : kAllOsKinds)
    if (name == GekkoAOT::NativeOS::Name(kind)) return kind;
  return std::nullopt;
}

std::optional<SimpleHleKind> SimpleKindForName(std::string_view name) {
  if (name == "OSGetTime") return SimpleHleKind::OsGetTime;
  if (name == "OSGetTick") return SimpleHleKind::OsGetTick;
  if (name == "memcpy" || name == "__memcpy") return SimpleHleKind::Memcpy;
  if (name == "memmove") return SimpleHleKind::Memmove;
  if (name == "memset" || name == "__memset") return SimpleHleKind::Memset;
  if (name == "memcmp") return SimpleHleKind::Memcmp;
  if (name == "bzero") return SimpleHleKind::Bzero;
  if (name == "strlen") return SimpleHleKind::Strlen;
  if (name == "strcmp") return SimpleHleKind::Strcmp;
  if (name == "strncmp") return SimpleHleKind::Strncmp;
  if (name == "strcpy") return SimpleHleKind::Strcpy;
  if (name == "strncpy") return SimpleHleKind::Strncpy;
  if (name == "strcat") return SimpleHleKind::Strcat;
  if (name == "strchr") return SimpleHleKind::Strchr;
  if (name == "strrchr") return SimpleHleKind::Strrchr;
  if (name == "memchr") return SimpleHleKind::Memchr;
  if (name == "strstr") return SimpleHleKind::Strstr;
  if (name == "strncat") return SimpleHleKind::Strncat;
  if (name == "strnlen") return SimpleHleKind::Strnlen;
  if (name == "strcspn") return SimpleHleKind::Strcspn;
  if (name == "strpbrk") return SimpleHleKind::Strpbrk;
  if (name == "DCFlushRange") return SimpleHleKind::DcFlushRange;
  if (name == "DCFlushRangeNoSync") return SimpleHleKind::DcFlushRangeNoSync;
  if (name == "DCStoreRange") return SimpleHleKind::DcStoreRange;
  if (name == "DCStoreRangeNoSync") return SimpleHleKind::DcStoreRangeNoSync;
  if (name == "DCInvalidateRange") return SimpleHleKind::DcInvalidateRange;
  if (name == "DCZeroRange") return SimpleHleKind::DcZeroRange;
  if (name == "DCTouchRange") return SimpleHleKind::DcTouchRange;
  if (name == "ICInvalidateRange") return SimpleHleKind::IcInvalidateRange;
  if (name == "GXLoadPosMtxImm") return SimpleHleKind::GxLoadPosMtxImm;
  if (name == "GXLoadTexMtxImm") return SimpleHleKind::GxLoadTexMtxImm;
  if (name == "GXLoadNrmMtxImm") return SimpleHleKind::GxLoadNrmMtxImm;
  if (name == "GXLoadNrmMtxImm3x3") return SimpleHleKind::GxLoadNrmMtxImm3x3;
  if (name == "DVDFastOpen") return SimpleHleKind::DvdFastOpen;
  if (name == "AngularCameraSetFov") return SimpleHleKind::AngularCameraSetFov;
  if (name == "TangentCameraFrustum") return SimpleHleKind::TangentCameraFrustum;
  if (name == "GXSetArray") return SimpleHleKind::GxSetArray;
  if (name == "GXSetAlphaUpdate") return SimpleHleKind::GxSetAlphaUpdate;
  if (name == "GXSetColorUpdate") return SimpleHleKind::GxSetColorUpdate;
  if (name == "GXSetZMode") return SimpleHleKind::GxSetZMode;
  if (name == "GXSetAlphaCompare") return SimpleHleKind::GxSetAlphaCompare;
  // Stable name decoding also loads manifests when a runtime switch is off.
  if (name == "PADRead") return SimpleHleKind::PadRead;
#define DEVICE_SDK(kind, text) if(name==text) return SimpleHleKind::kind;
#include "native/device_sdk.def"
#undef DEVICE_SDK
  return std::nullopt;
}

std::string_view CardWrapperCallee(SimpleHleKind kind) {
  switch(kind) {
  case SimpleHleKind::CardMount:return "CARDMountAsync";
  case SimpleHleKind::CardRead:return "CARDReadAsync";
  case SimpleHleKind::CardWrite:return "CARDWriteAsync";
  case SimpleHleKind::CardCreate:return "CARDCreateAsync";
  case SimpleHleKind::CardDelete:return "CARDDeleteAsync";
  case SimpleHleKind::CardFastDelete:return "CARDFastDeleteAsync";
  case SimpleHleKind::CardSetStatus:return "CARDSetStatusAsync";
  case SimpleHleKind::CardFormat:return "CARDFormatAsync";
  case SimpleHleKind::CardCheck:return "CARDCheckAsync";
  default:return {};
  }
}

bool IsGxMatrixKind(SimpleHleKind kind) {
  return kind == SimpleHleKind::GxLoadPosMtxImm ||
         kind == SimpleHleKind::GxLoadTexMtxImm ||
         kind == SimpleHleKind::GxLoadNrmMtxImm ||
         kind == SimpleHleKind::GxLoadNrmMtxImm3x3;
}

const char* SimpleKindName(SimpleHleKind kind) {
  switch (kind) {
  case SimpleHleKind::OsGetTime: return "OSGetTime";
  case SimpleHleKind::OsGetTick: return "OSGetTick";
  case SimpleHleKind::Memcpy: return "memcpy";
  case SimpleHleKind::Memmove: return "memmove";
  case SimpleHleKind::Memset: return "memset";
  case SimpleHleKind::Memcmp: return "memcmp";
  case SimpleHleKind::Bzero: return "bzero";
  case SimpleHleKind::Strlen: return "strlen";
  case SimpleHleKind::Strcmp: return "strcmp";
  case SimpleHleKind::Strncmp: return "strncmp";
  case SimpleHleKind::Strcpy: return "strcpy";
  case SimpleHleKind::Strncpy: return "strncpy";
  case SimpleHleKind::Strcat: return "strcat";
  case SimpleHleKind::Strchr: return "strchr";
  case SimpleHleKind::Strrchr: return "strrchr";
  case SimpleHleKind::Memchr: return "memchr";
  case SimpleHleKind::Strstr: return "strstr";
  case SimpleHleKind::Strncat: return "strncat";
  case SimpleHleKind::Strnlen: return "strnlen";
  case SimpleHleKind::Strcspn: return "strcspn";
  case SimpleHleKind::Strpbrk: return "strpbrk";
  case SimpleHleKind::DcFlushRange: return "DCFlushRange";
  case SimpleHleKind::DcFlushRangeNoSync: return "DCFlushRangeNoSync";
  case SimpleHleKind::DcStoreRange: return "DCStoreRange";
  case SimpleHleKind::DcStoreRangeNoSync: return "DCStoreRangeNoSync";
  case SimpleHleKind::DcInvalidateRange: return "DCInvalidateRange";
  case SimpleHleKind::DcZeroRange: return "DCZeroRange";
  case SimpleHleKind::DcTouchRange: return "DCTouchRange";
  case SimpleHleKind::IcInvalidateRange: return "ICInvalidateRange";
  case SimpleHleKind::GxLoadPosMtxImm: return "GXLoadPosMtxImm";
  case SimpleHleKind::GxLoadTexMtxImm: return "GXLoadTexMtxImm";
  case SimpleHleKind::PadRead: return "PADRead";
  case SimpleHleKind::DvdFastOpen: return "DVDFastOpen";
  case SimpleHleKind::GxSetArray: return "GXSetArray";
  case SimpleHleKind::GxSetAlphaUpdate: return "GXSetAlphaUpdate";
  case SimpleHleKind::GxSetColorUpdate: return "GXSetColorUpdate";
  case SimpleHleKind::GxSetZMode: return "GXSetZMode";
  case SimpleHleKind::GxSetAlphaCompare: return "GXSetAlphaCompare";
  case SimpleHleKind::GxLoadNrmMtxImm: return "GXLoadNrmMtxImm";
  case SimpleHleKind::GxLoadNrmMtxImm3x3: return "GXLoadNrmMtxImm3x3";
#define DEVICE_SDK(kind, text) case SimpleHleKind::kind: return text;
#include "native/device_sdk.def"
#undef DEVICE_SDK
  case SimpleHleKind::AngularCameraSetFov: return "AngularCameraSetFov";
  case SimpleHleKind::TangentCameraFrustum: return "TangentCameraFrustum";
  case SimpleHleKind::Count: break;
  }
  return "?";
}


struct DebugCorpusHashV33 {
  std::uint64_t h1 = 0xcbf29ce484222325ull;
  std::uint64_t h2 = 0x9e3779b97f4a7c15ull;
};

DebugCorpusHashV33 HashDebugCorpusShapeV33(const GekkoAOTSdk::StructuralShape& shape) {
  DebugCorpusHashV33 out;
  for (const std::uint32_t value : shape.skeleton) {
    out.h1 ^= value;
    out.h1 *= 0x100000001b3ull;
    out.h2 ^= value + 0x9e3779b9ull + (out.h2 << 6u) + (out.h2 >> 2u);
    out.h2 *= 0xbf58476d1ce4e5b9ull;
  }
  return out;
}

bool MatchesDebugCorpusV33(const std::uint8_t* code,
                           const GekkoAOTSdk::DebugCorpusSignatureV33& sig) {
  if (!code || sig.size < 4u || (sig.size & 3u)) return false;
  const auto shape = GekkoAOTSdk::BuildStructuralShape(code, sig.size);
  if (shape.size != sig.size || shape.skeleton.size() < 3u ||
      shape.direct_calls != sig.direct_calls || shape.branches != sig.branches ||
      shape.returns != sig.returns || shape.stack_frame != sig.stack_frame)
    return false;
  if (GekkoAOTSdk::StructuralPrefixHash(shape.skeleton[0], shape.skeleton[1],
                                        shape.skeleton[2]) != sig.prefix_hash)
    return false;
  const auto hash = HashDebugCorpusShapeV33(shape);
  return hash.h1 == sig.h1 && hash.h2 == sig.h2;
}

constexpr std::uint32_t kStaticDirectOsBase = 0xfffd0000u;
constexpr std::uint32_t kStaticDirectSimpleBase = 0xfffc0000u;

std::uint32_t StaticDirectToken(std::string_view name) {
  if (const auto kind = OsKindForName(name))
    return kStaticDirectOsBase | static_cast<std::uint8_t>(*kind);
  if (const auto kind = SimpleKindForName(name)) {
    if (IsGxMatrixKind(*kind) && !EnvEnabled("GEKKOAOT_GX_MATRIX_HLE", true))
      return 0u;
    return kStaticDirectSimpleBase | static_cast<std::uint8_t>(*kind);
  }
  return 0u;
}

// Some NativeOS entrypoints are not ordinary leaf replacements. They can
// snapshot the currently running thread, switch to another OSContext, wake a
// higher-priority thread, or re-enable an interrupt that immediately captures
// the interrupted architectural state. Their direct-token ABI therefore needs
// the caller's preserved PPC context, not merely the registers used by the SDK
// function body itself.
bool IsContextCaptureBoundary(OsKind kind) {
  switch (kind) {
  case OsKind::EnableScheduler:
  case OsKind::SelectThread:
  case OsKind::Reschedule:
  case OsKind::YieldThread:
  case OsKind::ExitThread:
  case OsKind::CancelThread:
  case OsKind::JoinThread:
  case OsKind::DetachThread:
  case OsKind::ResumeThread:
  case OsKind::SuspendThread:
  case OsKind::SleepThread:
  case OsKind::WakeupThread:
  case OsKind::SetThreadPriority:
  case OsKind::SaveContext:
  case OsKind::LoadContext:
  case OsKind::SetCurrentContext:
  case OsKind::LoadFpuContext:
  case OsKind::SaveFpuContext:
  case OsKind::FillFpuContext:
  case OsKind::LockMutex:
  case OsKind::UnlockMutex:
  case OsKind::TryLockMutex:
  case OsKind::WaitCond:
  case OsKind::SignalCond:
  case OsKind::SendMessage:
  case OsKind::ReceiveMessage:
  case OsKind::JamMessage:
  // Interrupt helpers are a single architectural state boundary.  All three
  // consume/update MSR[EE] through CPUState in the host implementation.  Keep
  // DisableInterrupts in the same ABI class as Enable/Restore so direct-HLE
  // cannot observe a stale MSR that is still resident in the AOT caller.
  case OsKind::DisableInterrupts:
  case OsKind::EnableInterrupts:
  case OsKind::RestoreInterrupts:
    return true;
  default:
    return false;
  }
}

bool IsFullFpContextBoundary(OsKind kind) {
  // NativeOS may capture or switch an OSContext from every context-capture
  // boundary below, not only from the explicit OSSetCurrentContext/FPU APIs.
  // SaveContext() maintains a complete host shadow of FPR0-FPR31, FPSCR and
  // PS0-PS31 so that an asynchronous scheduler/IRQ transition can later resume
  // the exact architectural register file.  Therefore volatile FPR0-FPR13 are
  // inputs at *all* such boundaries.  Leaving them SSA-resident in the AOT
  // caller makes NativeOS snapshot stale values and can corrupt render math
  // (matrices, colors, vertex transforms) while guest-AOT remains correct.
  return IsContextCaptureBoundary(kind);
}

bool IsOptionalForSchedulerClosure(OsKind kind) {
  // The native scheduler only needs the primitives that own run/wait queue
  // transitions to be a closed set.  Several SDK entrypoints are safe to leave
  // as guest AOT wrappers: they either only mutate ABI-visible objects or end
  // up in the core Sleep/Wakeup/Resume primitives below.  Requiring every
  // optional SDK API made stripped retail DOLs impossible to admit when, for
  // example, OSJoinThread or OSJamMessage was never linked at all.
  //
  // v71 makes closure admission LINKAGE-aware. High-level SDK wrappers are not
  // ownership roots: mutex/cond/message/yield ultimately transition threads via
  // Sleep/Wakeup/Resume/Suspend and can therefore be absent from a particular
  // link without disabling NativeOS. When they are linked and resolved they are
  // still installed natively. Guest-created threads are reconciled through the
  // ABI-visible OSThread/run queues. Alarm callbacks remain a separate closure.
  switch (kind) {
  case OsKind::GetCurrentThread:
  case OsKind::SelectThread:
  case OsKind::Reschedule:
  case OsKind::YieldThread:
  case OsKind::SetThreadPriority:
  case OsKind::GetThreadPriority:
  case OsKind::IsThreadTerminated:
  case OsKind::GetCurrentContext:
  case OsKind::LoadFpuContext:
  case OsKind::SaveFpuContext:
  case OsKind::FillFpuContext:
  case OsKind::GetStackPointer:
  case OsKind::SwitchStack:
  case OsKind::CreateThread:
  case OsKind::ExitThread:
  case OsKind::CancelThread:
  case OsKind::JoinThread:
  case OsKind::DetachThread:
  case OsKind::InitContext:
  case OsKind::InitMutex:
  case OsKind::LockMutex:
  case OsKind::UnlockMutex:
  case OsKind::TryLockMutex:
  case OsKind::InitCond:
  case OsKind::WaitCond:
  case OsKind::SignalCond:
  case OsKind::InitMessageQueue:
  case OsKind::SendMessage:
  case OsKind::ReceiveMessage:
  case OsKind::JamMessage:
  case OsKind::InitAlarm:
  case OsKind::SetAbsAlarm:
  case OsKind::SetPeriodicAlarm:
  case OsKind::CheckAlarmQueue:
    return true;
  default:
    return false;
  }
}

bool IsAlarmGuestByDefault(OsKind kind) {
  // The OSContext family is native again.  Its implementation is now tied
  // to the same NativeOS scheduler ownership and follows the SDK PPC field/queue
  // semantics used by retail titles.  Alarm callbacks remain guest-AOT by
  // default because their callback/reentry model is an independent closure.
  switch (kind) {
  case OsKind::InitAlarm:
  case OsKind::CreateAlarm:
  case OsKind::SetAlarm:
  case OsKind::SetAbsAlarm:
  case OsKind::SetPeriodicAlarm:
  case OsKind::CancelAlarm:
  case OsKind::CheckAlarmQueue:
    return true;
  default:
    return false;
  }
}

bool IsInterruptHelper(OsKind kind) {
  return kind == OsKind::DisableInterrupts || kind == OsKind::EnableInterrupts ||
         kind == OsKind::RestoreInterrupts;
}

enum class NativeOsBisectMode : std::uint8_t {
  InterruptDisable,
  InterruptEnable,
  InterruptRestore,
  InterruptEnableRestore,
  Interrupts,
  Scheduler,
  Sync,
  All,
};

NativeOsBisectMode ParseNativeOsBisectMode(std::string_view value) {
  if (value == "disable" || value == "interrupt-disable" ||
      value == "disable-only")
    return NativeOsBisectMode::InterruptDisable;
  if (value == "enable" || value == "interrupt-enable" ||
      value == "enable-only")
    return NativeOsBisectMode::InterruptEnable;
  if (value == "restore" || value == "interrupt-restore" ||
      value == "restore-only")
    return NativeOsBisectMode::InterruptRestore;
  if (value == "enable+restore" || value == "interrupt-enable-restore")
    return NativeOsBisectMode::InterruptEnableRestore;
  if (value == "interrupts" || value == "interrupts-only")
    return NativeOsBisectMode::Interrupts;
  if (value == "scheduler" || value == "scheduler-core")
    return NativeOsBisectMode::Scheduler;
  if (value == "sync" || value == "scheduler+sync")
    return NativeOsBisectMode::Sync;
  return NativeOsBisectMode::All;
}

const char* NativeOsBisectModeName(NativeOsBisectMode mode) {
  switch (mode) {
  case NativeOsBisectMode::InterruptDisable: return "disable";
  case NativeOsBisectMode::InterruptEnable: return "enable";
  case NativeOsBisectMode::InterruptRestore: return "restore";
  case NativeOsBisectMode::InterruptEnableRestore: return "enable+restore";
  case NativeOsBisectMode::Interrupts: return "interrupts";
  case NativeOsBisectMode::Scheduler: return "scheduler";
  case NativeOsBisectMode::Sync: return "sync";
  case NativeOsBisectMode::All: return "all";
  }
  return "all";
}

bool IsInterruptOnlyBisect(NativeOsBisectMode mode) {
  switch (mode) {
  case NativeOsBisectMode::InterruptDisable:
  case NativeOsBisectMode::InterruptEnable:
  case NativeOsBisectMode::InterruptRestore:
  case NativeOsBisectMode::InterruptEnableRestore:
  case NativeOsBisectMode::Interrupts:
    return true;
  default:
    return false;
  }
}

bool InterruptBisectAdmits(OsKind kind, NativeOsBisectMode mode) {
  if (!IsInterruptHelper(kind)) return false;
  switch (mode) {
  case NativeOsBisectMode::InterruptDisable:
    return kind == OsKind::DisableInterrupts;
  case NativeOsBisectMode::InterruptEnable:
    return kind == OsKind::EnableInterrupts;
  case NativeOsBisectMode::InterruptRestore:
    return kind == OsKind::RestoreInterrupts;
  case NativeOsBisectMode::InterruptEnableRestore:
    return kind == OsKind::EnableInterrupts || kind == OsKind::RestoreInterrupts;
  default:
    return true;
  }
}

bool IsSyncKind(OsKind kind) {
  switch (kind) {
  case OsKind::InitMutex:
  case OsKind::LockMutex:
  case OsKind::UnlockMutex:
  case OsKind::TryLockMutex:
  case OsKind::InitCond:
  case OsKind::WaitCond:
  case OsKind::SignalCond:
    return true;
  default:
    return false;
  }
}

bool IsMessageKind(OsKind kind) {
  return kind == OsKind::InitMessageQueue || kind == OsKind::SendMessage ||
         kind == OsKind::ReceiveMessage || kind == OsKind::JamMessage;
}

bool NativeOsBisectAdmits(OsKind kind, NativeOsBisectMode mode) {
  if (IsInterruptHelper(kind)) return InterruptBisectAdmits(kind, mode);
  if (IsAlarmGuestByDefault(kind)) return mode == NativeOsBisectMode::All;
  if (IsMessageKind(kind)) return mode == NativeOsBisectMode::All;
  if (IsSyncKind(kind))
    return mode == NativeOsBisectMode::Sync || mode == NativeOsBisectMode::All;
  // Everything else in NativeOS is scheduler/thread/context state.
  return mode == NativeOsBisectMode::Scheduler ||
         mode == NativeOsBisectMode::Sync || mode == NativeOsBisectMode::All;
}

struct Range {
  std::uint32_t start = 0;
  std::uint32_t end = 0;
};

std::vector<Range> CodeRanges(const GekkoAOTModuleDesc* module) {
  std::vector<Range> ranges;
  if (!module) return ranges;
  ranges.reserve(module->num_code_ranges);
  for (std::uint32_t i = 0; i < module->num_code_ranges; ++i) {
    const auto& in = module->code_ranges[i];
    if (!in.start || in.end <= in.start || (in.start & 3u) || (in.end & 3u)) continue;
    ranges.push_back({in.start, in.end});
  }
  std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
    return std::tie(a.start, a.end) < std::tie(b.start, b.end);
  });
  std::vector<Range> merged;
  for (const auto& range : ranges) {
    if (!merged.empty() && range.start <= merged.back().end) {
      merged.back().end = std::max(merged.back().end, range.end);
    } else {
      merged.push_back(range);
    }
  }
  return merged;
}

std::vector<Range> CodeRanges(const std::vector<GekkoAOTRange>& input) {
  std::vector<Range> ranges;
  ranges.reserve(input.size());
  for (const auto& in : input) {
    if (!in.start || in.end <= in.start || (in.start & 3u) || (in.end & 3u)) continue;
    ranges.push_back({in.start, in.end});
  }
  std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
    return std::tie(a.start, a.end) < std::tie(b.start, b.end);
  });
  std::vector<Range> merged;
  for (const auto& range : ranges) {
    if (!merged.empty() && range.start <= merged.back().end)
      merged.back().end = std::max(merged.back().end, range.end);
    else
      merged.push_back(range);
  }
  return merged;
}

const Range* ContainingRange(const std::vector<Range>& ranges, std::uint32_t address) {
  for (const auto& range : ranges)
    if (address >= range.start && address < range.end) return &range;
  return nullptr;
}

// Candidate prefixes are looked up by the same 64-bit hash the SDK reference
// table stores; every hit is then confirmed by GroundTruthExact.
std::uint64_t CandidatePrefix3(const std::uint8_t* code) {
  return GekkoAOTSdk::StructuralPrefixHash(
      GekkoAOTSdk::StructuralInstructionSkeleton(GekkoAOTSdk::StructuralReadBE32(code)),
      GekkoAOTSdk::StructuralInstructionSkeleton(GekkoAOTSdk::StructuralReadBE32(code + 4)),
      GekkoAOTSdk::StructuralInstructionSkeleton(GekkoAOTSdk::StructuralReadBE32(code + 8)));
}

std::uint64_t Prefix2(std::uint32_t a, std::uint32_t b) {
  return GekkoAOTSdk::StructuralPrefixHash(a, b);
}

void RepairSemanticOsIdentities(Resolver& resolver) {
  const auto& shapes = GekkoAOTSdk::GroundTruthShapes();

  // Collision-prone scheduler wrappers: require the +/-1 counter operation,
  // proven interrupt helpers and an exact reference body.
  for (const auto& [address, function] : resolver.Functions()) {
    const int delta = GekkoAOTSdk::DecodeOsSchedulerDelta(function.code, function.size);
    if (!delta || !resolver.Calls(address, "OSDisableInterrupts", 0.98) ||
        !resolver.Calls(address, "OSRestoreInterrupts", 0.98))
      continue;
    const char* identity = delta > 0 ? "OSDisableScheduler" : "OSEnableScheduler";
    bool exact = false;
    for (std::size_t i = 0; i < GekkoAOTSdk::kGroundTruthExemplars.size(); ++i) {
      const auto& exemplar = GekkoAOTSdk::kGroundTruthExemplars[i];
      if (exemplar.name == identity &&
          GekkoAOTSdk::GroundTruthExact(function.shape, shapes[i])) {
        exact = true;
        break;
      }
    }
    if (exact) resolver.Override(address, identity, 0.999, "scheduler-delta+interrupt-proof");
  }

  // Tiny leaf primitives have stable store semantics even when their raw DSY
  // hashes collide between SDK revisions.
  for (const auto& [address, function] : resolver.Functions()) {
    using Primitive = GekkoAOTSdk::OsPrimitive;
    const auto primitive = GekkoAOTSdk::DecodeOsPrimitive(function.code, function.size);
    const char* identity = nullptr;
    switch (primitive) {
    case Primitive::Mutex: identity = "OSInitMutex"; break;
    case Primitive::MessageQueue: identity = "OSInitMessageQueue"; break;
    case Primitive::Priority: identity = "OSGetThreadPriority"; break;
    default: break;
    }
    if (identity) resolver.Override(address, identity, 0.985, "instruction-semantics");
  }

  // The queue initializer is byte-collision-prone. Its use from OSInitMutex is
  // the semantic discriminator.
  for (const auto owner : resolver.Find("OSInitMutex", 0.98)) {
    const auto* function = resolver.Function(owner);
    if (!function) continue;
    for (const auto target : function->calls) {
      const auto* callee = resolver.Function(target);
      if (!callee) continue;
      if (GekkoAOTSdk::DecodeOsPrimitive(callee->code, callee->size) ==
          GekkoAOTSdk::OsPrimitive::Queue)
        resolver.Override(target, "OSInitThreadQueue", 0.99, "callee-of-OSInitMutex");
    }
  }

  for (const auto& [address, function] : resolver.Functions()) {
    if (GekkoAOTSdk::DecodeOsLoadContextShape(function.code, function.size))
      resolver.Override(address, "OSLoadContext", 0.99, "context-restore-semantics");
  }
  for (const auto clear : resolver.Find("OSClearContext", 0.98)) {
    for (const auto& [address, function] : resolver.Functions()) {
      if (GekkoAOTSdk::DecodeOsInitContext(function.code, function.size,
                                            function.address, clear))
        resolver.Override(address, "OSInitContext", 0.99,
                          "instruction-semantics+call");
    }
  }

  // OSInitCond and OSSignalCond can have the same normalized body. Their direct
  // callee separates them without relying on link order or absolute addresses.
  for (const auto& [address, function] : resolver.Functions()) {
    std::uint32_t target = 0;
    if (!GekkoAOTSdk::DecodeOsThinCallWrapper(function.code, function.size,
                                               function.address, &target))
      continue;
    const auto* callee = resolver.Resolution(target);
    if (!callee || resolver.IsAmbiguous(target) || callee->confidence < 0.98) continue;
    if (callee->name == "OSInitThreadQueue")
      resolver.Override(address, "OSInitCond", 0.999, "thin-wrapper-callee-proof");
    else if (callee->name == "OSWakeupThread")
      resolver.Override(address, "OSSignalCond", 0.999, "thin-wrapper-callee-proof");
  }

  static const std::vector<GekkoAOTSdk::CallGraphRule> kRules = {
      {"OSInitCond", 8, 96, {"OSInitThreadQueue"}, {}, 1, 2, 0.965},
      {"OSWaitCond", 64, 512, {"OSSleepThread", "OSLockMutex", "OSWakeupThread"},
       {"OSDisableInterrupts", "OSRestoreInterrupts"}, 3, 10, 0.975},
      {"OSSignalCond", 8, 160, {"OSWakeupThread"}, {}, 1, 3, 0.955},
      {"OSJoinThread", 80, 640, {"OSDisableInterrupts", "OSSleepThread", "OSRestoreInterrupts"},
       {}, 3, 10, 0.97},
      {"OSDetachThread", 48, 384, {"OSDisableInterrupts", "OSWakeupThread", "OSRestoreInterrupts"},
       {}, 3, 8, 0.97},
      {"OSCreateThread", 128, 1200, {"OSInitContext", "OSDisableInterrupts", "OSRestoreInterrupts"},
       {}, 3, 10, 0.98},
      {"OSExitThread", 96, 900, {"OSClearContext", "OSWakeupThread", "OSDisableInterrupts"},
       {"OSRestoreInterrupts"}, 3, 12, 0.975},
      {"OSTryLockMutex", 48, 480, {"OSDisableInterrupts", "OSGetCurrentThread", "OSRestoreInterrupts"},
       {}, 3, 8, 0.97},
      {"OSInitMessageQueue", 32, 192, {"OSInitThreadQueue"}, {}, 2, 4, 0.97},
      {"OSSetAbsAlarm", 48, 320, {"OSDisableInterrupts", "OSRestoreInterrupts"}, {}, 2, 6, 0.955},
  };
  for (unsigned pass = 0; pass < 8; ++pass) {
    std::size_t added = 0;
    for (const auto& rule : kRules) {
      std::vector<std::uint32_t> matches;
      for (const auto& [address, function] : resolver.Functions()) {
        if (resolver.Resolution(address) && !resolver.IsAmbiguous(address)) continue;
        if (GekkoAOTSdk::MatchCallGraphRule(resolver, function, rule))
          matches.push_back(address);
      }
      if (matches.empty()) continue;
      const auto* first = resolver.Function(matches.front());
      bool one_body = first != nullptr;
      for (const auto address : matches) {
        const auto* candidate = resolver.Function(address);
        one_body = one_body && candidate && first &&
                   candidate->shape.skeleton == first->shape.skeleton;
      }
      if (!one_body) continue;
      for (const auto address : matches)
        if (resolver.Override(address, rule.name, rule.confidence, "closure-callgraph"))
          ++added;
    }
    if (!added) break;
  }
}

} // namespace

const char* SimpleServiceName(unsigned kind) {
  return kind < static_cast<unsigned>(SimpleHleKind::Count)
             ? SimpleKindName(static_cast<SimpleHleKind>(kind))
             : "?";
}

NativeSdkAutoStats AutoRegisterNativeSdk(HostRuntime& runtime,
                                           const std::filesystem::path& manifest_output,
                                           bool cameras_only, bool renderer_only) {
  std::vector<std::pair<std::uint32_t, std::string>> manifest_hooks;
  NativeSdkAutoStats stats;
  if (!EnvEnabled("GEKKOAOT_NATIVE_SDK_AUTO", true)) {
    std::cerr << "GEKKOAOT_NATIVE_SDK_AUTO_V1=0 reason=disabled\n";
    return stats;
  }

  const auto* module = runtime.Module();
  const auto ranges = module && module->code_ranges && module->num_code_ranges != 0
                          ? CodeRanges(module)
                          : CodeRanges(runtime.LoadedDolCodeRanges());
  const std::uint32_t entry_point = module ? module->entry_point : runtime.LoadedDolEntryPoint();
  if (ranges.empty()) {
    std::cerr << "GEKKOAOT_NATIVE_SDK_AUTO_V1=0 reason=no-code-ranges\n";
    return stats;
  }

  std::map<std::uint64_t, std::vector<std::size_t>> prefix3;
  std::map<std::uint64_t, std::vector<std::size_t>> prefix2;
  for (std::size_t i = 0; i < GekkoAOTSdk::kGroundTruthExemplars.size(); ++i) {
    const auto& exemplar = GekkoAOTSdk::kGroundTruthExemplars[i];
    if (exemplar.instruction_count >= 3)
      prefix3[exemplar.prefix3_hash].push_back(i);
    else if (exemplar.instruction_count == 2)
      prefix2[exemplar.prefix2_hash].push_back(i);
  }

  // Scan direct BL targets first. Used SDK functions are overwhelmingly direct
  // call targets, and keeping this set also makes two-instruction leaf matching
  // conservative enough to avoid matching instruction pairs inside game code.
  std::set<std::uint32_t> direct_targets;
  for (const auto& range : ranges) {
    for (std::uint32_t pc = range.start; pc + 4u <= range.end; pc += 4u) {
      const auto* code = runtime.Memory().Resolve(pc, 4u);
      if (!code) continue;
      ++stats.scanned_words;
      std::uint32_t target = 0;
      if (GekkoAOTSdk::StructuralDirectCall(
              pc, GekkoAOTSdk::StructuralReadBE32(code), &target) &&
          ContainingRange(ranges, target))
        direct_targets.insert(target);
    }
  }
  stats.direct_call_targets = direct_targets.size();
  if (entry_point) direct_targets.insert(entry_point);
  for (const auto& range : ranges) direct_targets.insert(range.start);

  // Virtual camera updates are not necessarily direct BL targets. Admit only
  // an entire proven method (prologue through return), never short SDK-shaped
  // instruction sequences found inside arbitrary functions.
  for(const auto& range:ranges)for(auto pc=range.start;pc+508u<=range.end;pc+=4u)
    if(GekkoAOTSdk::ProveTangentCameraUpdate(pc,[&](auto address,auto bytes){return runtime.Memory().Resolve(address,bytes);})){
      runtime.RegisterSimpleHle(SimpleHleKind::TangentCameraFrustum,pc);
      manifest_hooks.emplace_back(pc,"TangentCameraFrustum");++stats.simple_hooks;
      std::cerr<<"GEKKOAOT_CAMERA_PROVIDER kind=tangent-update pc="<<std::hex<<pc<<std::dec<<" proof=entire-virtual-camera-update\n";
    }

  for(const auto pc:direct_targets) {
    if(GekkoAOTSdk::ProveJ3dFrustum(pc,[&](auto address,auto bytes){return runtime.Memory().Resolve(address,bytes);})) {
      // Reuse the existing camera service ABI token. Cached modules retain
      // their service indices; the dispatcher proves the specific builder.
      runtime.RegisterSimpleHle(SimpleHleKind::TangentCameraFrustum,pc);
      manifest_hooks.emplace_back(pc,"TangentCameraFrustum");++stats.simple_hooks;
      std::cerr<<"GEKKOAOT_CAMERA_PROVIDER kind=j3d-frustum pc="<<std::hex<<pc<<std::dec<<" proof=entire-builder+math-callees\n";
    }
    if(GekkoAOTSdk::ProveTangentFrustum(pc,[&](auto address,auto bytes){return runtime.Memory().Resolve(address,bytes);})) {
      runtime.RegisterSimpleHle(SimpleHleKind::TangentCameraFrustum,pc);
      manifest_hooks.emplace_back(pc,"TangentCameraFrustum");++stats.simple_hooks;
      std::cerr<<"GEKKOAOT_CAMERA_PROVIDER kind=tangent-frustum pc="<<std::hex<<pc<<std::dec<<" proof=entire-call-free-consumer\n";
    }
    if(!GekkoAOTSdk::ProveAngularCameraSetter(pc,[&](auto address,auto bytes){return runtime.Memory().Resolve(address,bytes);}))continue;
    runtime.RegisterSimpleHle(SimpleHleKind::AngularCameraSetFov,pc);
    manifest_hooks.emplace_back(pc,"AngularCameraSetFov");++stats.simple_hooks;
    std::cerr<<"GEKKOAOT_CAMERA_PROVIDER kind=angular-setter pc="<<std::hex<<pc<<std::dec<<" proof=body+callees+constants\n";
  }

  if(!cameras_only){
  // Recognize complete call-free GX pixel leaves at proven direct-call
  // entries. No symbol/name or relocated PC is sufficient for admission.
  for(const auto pc:direct_targets)for(unsigned index=0;index<4;++index){
    const auto family=GekkoAOTSdk::GxPixelFamily(index);
    const auto bytes=GekkoAOTSdk::GxPixelProof(family).masks.size()*4;
    const auto* code=runtime.Memory().Resolve(pc,bytes);
    if(!code || !GekkoAOTSdk::ProveGxPixelLeaf({code,bytes},family))continue;
    const auto kind=SimpleHleKind(unsigned(SimpleHleKind::GxSetAlphaCompare)+index);
    runtime.RegisterSimpleHle(kind,pc);manifest_hooks.emplace_back(pc,SimpleKindName(kind));++stats.simple_hooks;
    std::cerr<<"GEKKOAOT_GX_PIXEL_PROVIDER name="<<SimpleKindName(kind)<<" pc="<<std::hex<<pc<<std::dec<<" proof=entire-body+sda-cell+fifo+bookkeeping\n";
  }
  }

  if(cameras_only || renderer_only) {
    // Also used to refresh structurally proven camera providers when loading
    // a valid older AOT/SDK cache. Registration precedes native execution;
    // late registration after VI can miss cached direct function routes.
    if(manifest_output.empty())return stats;
    if(!manifest_output.parent_path().empty())std::filesystem::create_directories(manifest_output.parent_path());
    std::ofstream out(manifest_output,std::ios::trunc);
    out<<(renderer_only?"# GEKKOAOT_NATIVE_RENDERER_MANIFEST_V1\n":"# GEKKOAOT_NATIVE_CAMERA_MANIFEST_V1\n")<<"# address name direct-token\n";
    for(const auto& [pc,name]:manifest_hooks)
      out<<"0x"<<std::hex<<pc<<' '<<name<<" 0x"<<StaticDirectToken(name)<<'\n';
    out.close();if(!out)throw std::runtime_error("camera manifest write failed");
    return stats;
  }
  struct ExactMatch { std::string_view name; std::uint32_t size; };
  Resolver resolver;
  std::set<std::uint32_t> discovered;

  // Exact corpus bodies are only admitted at proven function entries.
  // v70 scanned every aligned instruction and could therefore assign an SDK
  // identity to a byte sequence in the middle of another function (DNDD01
  // famously produced a bogus OSCreateAlarm inside UnsetRun). Direct BL targets,
  // the DOL entry point and executable range starts are conservative function
  // entry evidence and are sufficient for SDK entrypoint lowering.
  for (const auto pc : direct_targets) {
    const auto* range = ContainingRange(ranges, pc);
    if (!range || pc + 12u > range->end) continue;
    const auto* prefix_code = runtime.Memory().Resolve(pc, 12u);
    if (!prefix_code) continue;
    const auto it = prefix3.find(CandidatePrefix3(prefix_code));
    if (it == prefix3.end()) continue;

    std::vector<ExactMatch> matches;
    for (const auto index : it->second) {
      const auto& exemplar = GekkoAOTSdk::kGroundTruthExemplars[index];
      const auto size64 = std::uint64_t{exemplar.instruction_count} * 4u;
      if (size64 < 12u || size64 > 0x10000u || pc + size64 > range->end) continue;
      const auto size = static_cast<std::uint32_t>(size64);
      const auto* code = runtime.Memory().Resolve(pc, size);
      if (!code) continue;
      const auto shape = GekkoAOTSdk::BuildStructuralShape(code, size);
      if (GekkoAOTSdk::GroundTruthExact(shape, GekkoAOTSdk::GroundTruthShapes()[index]))
        matches.push_back({exemplar.name, size});
    }
    if (matches.empty()) continue;

    std::set<std::string_view> names;
    std::uint32_t selected_size = 0xffffffffu;
    for (const auto& match : matches) {
      names.insert(match.name);
      selected_size = std::min(selected_size, match.size);
    }
    if (selected_size == 0xffffffffu || discovered.contains(pc)) continue;
    const auto* code = runtime.Memory().Resolve(pc, selected_size);
    if (!code) continue;
    resolver.AddFunction(pc, selected_size, 0, 0, code, {});
    discovered.insert(pc);
    ++stats.exact_functions;
    if (names.size() == 1)
      resolver.Seed(pc, std::string(*names.begin()), 0.999,
                    "groundtruth-normalized-exact-entry");
    else {
      resolver.MarkAmbiguous(pc);
      ++stats.ambiguous_functions;
    }
  }


  // User-supplied debug ELFs exposed several SDK/MSL leaves that are not
  // present in the old ground-truth corpus (notably ICInvalidateRange,
  // DC*RangeNoSync and common string helpers).  Admit only exact structural
  // fingerprints at already-proven direct call targets.  Fingerprints contain
  // normalized instruction metadata/hashes only; no proprietary code bytes.
  for (const auto pc : direct_targets) {
    if (discovered.contains(pc)) continue;
    const auto* range = ContainingRange(ranges, pc);
    if (!range || pc + 12u > range->end) continue;
    std::set<std::string_view> names;
    std::uint32_t selected_size = 0xffffffffu;
    for (const auto& sig : GekkoAOTSdk::kDebugCorpusSignaturesV33) {
      if (pc + sig.size > range->end) continue;
      const auto* prefix_code = runtime.Memory().Resolve(pc, 12u);
      if (!prefix_code) break;
      if (CandidatePrefix3(prefix_code) != sig.prefix_hash)
        continue;
      const auto* code = runtime.Memory().Resolve(pc, sig.size);
      if (!code || !MatchesDebugCorpusV33(code, sig)) continue;
      names.insert(sig.name);
      selected_size = std::min(selected_size, sig.size);
    }
    if (names.size() != 1u || selected_size == 0xffffffffu) {
      if (names.size() > 1u) ++stats.ambiguous_functions;
      continue;
    }
    const auto* code = runtime.Memory().Resolve(pc, selected_size);
    if (!code) continue;
    resolver.AddFunction(pc, selected_size, 0, 0, code, {});
    resolver.Seed(pc, std::string(*names.begin()), 0.9995,
                  "debug-elf-corpus-exact-v34");
    discovered.insert(pc);
    ++stats.exact_functions;
  }

  // v300 SDK service corpus: exact structural fingerprints of every SDK
  // subsystem (GX, GD, DVD, CARD, PAD/SI, VI, AI, AR/ARQ, DSP, AX/MIX, THP,
  // MTX, MSL...) generated by tools/sdk_service_corpus.py; a fingerprint
  // shared by two names anywhere in the source set (game code included) was
  // dropped at generation. Same admission as
  // Proven `bl` targets, then contiguous library neighbours. NativeOS
  // identities stay governed by the semantic proofs above, so OS names are
  // recorded in the service map only and never seeded from here.
  std::vector<std::pair<std::uint32_t, const GekkoAOTSdk::DebugCorpusSignatureV33*>> service_map;
  if (EnvEnabled("GEKKOAOT_SDK_SERVICE_CORPUS", true)) {
    std::map<std::uint64_t, std::vector<const GekkoAOTSdk::DebugCorpusSignatureV33*>> by_prefix;
    for (const auto& sig : GekkoAOTSdk::kSdkServiceCorpusV1) by_prefix[sig.prefix_hash].push_back(&sig);
    for (const auto& sig : GekkoAOTSdk::kNativeConsoleCorpusV1) by_prefix[sig.prefix_hash].push_back(&sig);
    const auto identify = [&](std::uint32_t pc) -> const GekkoAOTSdk::DebugCorpusSignatureV33* {
      const auto* range = ContainingRange(ranges, pc);
      if (!range || pc + 12u > range->end) return nullptr;
      const auto* head = runtime.Memory().Resolve(pc, 12u);
      if (!head) return nullptr;
      const auto it = by_prefix.find(CandidatePrefix3(head));
      if (it == by_prefix.end()) return nullptr;
      std::set<std::string_view> names;
      const GekkoAOTSdk::DebugCorpusSignatureV33* best = nullptr;
      for (const auto* sig : it->second) {
        if (pc + sig->size > range->end) continue;
        const auto* code = runtime.Memory().Resolve(pc, sig->size);
        if (!code || !MatchesDebugCorpusV33(code, *sig)) continue;
        names.insert(sig->name);
        if (!best || sig->size < best->size) best = sig;
      }
      return names.size() == 1u ? best : nullptr;
    };
    for (const auto& sig : GekkoAOTSdk::kNativePad2001Corpus) by_prefix[sig.prefix_hash].push_back(&sig);
    for (const auto& sig : GekkoAOTSdk::kNativeCard2001Corpus) by_prefix[sig.prefix_hash].push_back(&sig);
    std::map<std::uint32_t, const GekkoAOTSdk::DebugCorpusSignatureV33*> found;
    for (const auto pc : direct_targets)
      if (const auto* sig = identify(pc)) found.emplace(pc, sig);
    std::vector<std::uint32_t> frontier;
    for (const auto& entry : found) frontier.push_back(entry.first);
    while (!frontier.empty()) {
      const auto pc = frontier.back();
      frontier.pop_back();
      const auto next = pc + found.at(pc)->size;
      for (std::uint32_t pad = 0; pad <= 28u; pad += 4u) {
        if (found.contains(next + pad)) break;
        if (const auto* sig = identify(next + pad)) {
          found.emplace(next + pad, sig);
          frontier.push_back(next + pad);
          break;
        }
      }
    }
    // Only seed identities that select an existing native helper whose guest
    // memory access is valid for every address class, including the paged
    // VM aperture (strings are read through the per-page DTLB view; cache
    // publication only notifies the renderer). Everything else is recorded in
    // the service map without influencing the corpus/graph resolver.
    const auto vm_safe_simple = [](std::string_view name) {
      const auto kind = SimpleKindForName(name);
      if (!kind) return false;
      if (*kind >= SimpleHleKind::OsGetRtc) return true;
      switch (*kind) {
      case SimpleHleKind::Strlen: case SimpleHleKind::Strcmp: case SimpleHleKind::Strchr:
      case SimpleHleKind::Strrchr: case SimpleHleKind::Strstr: case SimpleHleKind::Strcspn:
      case SimpleHleKind::Strpbrk: case SimpleHleKind::Strcpy: case SimpleHleKind::Strcat:
      case SimpleHleKind::Strncmp: case SimpleHleKind::Strncpy: case SimpleHleKind::Strncat:
      case SimpleHleKind::Strnlen:
      case SimpleHleKind::Memcpy: case SimpleHleKind::Memmove: case SimpleHleKind::Memset:
      case SimpleHleKind::Memcmp: case SimpleHleKind::Memchr: case SimpleHleKind::Bzero:
      case SimpleHleKind::DcZeroRange: case SimpleHleKind::DcFlushRange:
      case SimpleHleKind::PadRead:
      case SimpleHleKind::DvdFastOpen:
      case SimpleHleKind::GxSetArray:
      case SimpleHleKind::GxLoadNrmMtxImm:
      case SimpleHleKind::GxLoadNrmMtxImm3x3:
      case SimpleHleKind::DcFlushRangeNoSync: case SimpleHleKind::DcStoreRange:
      case SimpleHleKind::DcStoreRangeNoSync: case SimpleHleKind::DcInvalidateRange:
      case SimpleHleKind::DcTouchRange: case SimpleHleKind::IcInvalidateRange:
        return true;
      default:
        return false;
      }
    };
    // Old PAD variants contain inlined reset logic. A shape is insufficient:
    // require their actual calls to independently recognized SDK routines.
    const auto pad2001_admitted=[&](std::uint32_t pc,const auto* sig) {
      const bool supplemental=std::any_of(GekkoAOTSdk::kNativePad2001Corpus.begin(),
          GekkoAOTSdk::kNativePad2001Corpus.end(),[&](const auto& e){return &e==sig;});
      if(!supplemental)return true;
      const auto calls=[&](std::string_view expected){
        const auto* code=runtime.Memory().Resolve(pc,sig->size);if(!code)return false;
        for(unsigned off=0;off<sig->size;off+=4){
          const auto w=GekkoAOTSdk::DeviceWord(code+off);
          if((w&0xfc000003u)!=0x48000001u)continue;
          const auto target=pc+off+(std::uint32_t(std::int32_t(w<<6)>>6)&~3u);
          const auto it=found.find(target);
          if(it!=found.end() && std::string_view(it->second->name)==expected)return true;
        }
        return false;
      };
      const std::string_view name=sig->name;
      if(name=="PADInit")return calls("PADSetSpec") && calls("PADSetSamplingRate") && calls("OSRegisterResetFunction");
      if(name=="PADRead")return calls("SIGetResponse") && calls("SIDisablePolling");
      if(name=="PADReset" || name=="PADRecalibrate" || name=="PADSetAnalogMode")return calls("SIDisablePolling") && calls("OSDisableInterrupts") && calls("OSRestoreInterrupts");
      if(name=="PADControlMotor")return calls("SISetCommand") && calls("OSDisableInterrupts") && calls("OSRestoreInterrupts");
      return false;
    };
    std::size_t seeded = 0;
    for (const auto& [pc, sig] : found) {
      service_map.emplace_back(pc, sig);
      if (discovered.contains(pc) || !vm_safe_simple(sig->name) || !pad2001_admitted(pc,sig)) continue;
      const auto* code = runtime.Memory().Resolve(pc, sig->size);
      if (!code) continue;
      if (std::string_view(sig->name)=="DVDFastOpen" &&
          !GekkoAOTSdk::ProveDvdFastOpen(code,sig->size)) continue;
      if (std::string_view(sig->name)=="GXSetArray" &&
          !GekkoAOTSdk::ProveGxSetArray(code,sig->size)) continue;
      if ((std::string_view(sig->name)=="GXLoadNrmMtxImm" ||
           std::string_view(sig->name)=="GXLoadNrmMtxImm3x3") &&
          !GekkoAOTSdk::ProveGxNormalMatrix(code,sig->size,pc,
             std::string_view(sig->name)=="GXLoadNrmMtxImm3x3",
             [&](std::uint32_t addr,std::size_t bytes){return runtime.Memory().Resolve(addr,bytes);})) continue;
      resolver.AddFunction(pc, sig->size, 0, 0, code, {});
      resolver.Seed(pc, std::string(sig->name), 0.9995, "sdk-service-corpus-v300");
      discovered.insert(pc);
      ++seeded;
    }
    static constexpr std::array<std::string_view, 24> kFamilies{
        "GX", "GD", "DVD", "CARD", "PAD", "SI", "VI", "AI", "ARQ", "AR", "DSP", "AX", "MIX",
        "THP", "OS", "EXI", "DB", "PSMTX", "PSVEC", "C_", "DC", "IC", "LC", "GBA"};
    std::map<std::string_view, std::size_t> families;
    for (const auto& [pc, sig] : service_map) {
      std::string_view name = sig->name;
      if (name.starts_with("__")) name.remove_prefix(2);
      std::string_view family = "MSL";
      for (const auto prefix : kFamilies)
        if (name.starts_with(prefix)) { family = prefix; break; }
      ++families[family];
    }
    std::cerr << "GEKKOAOT_SDK_SERVICE_MAP_V300=1 identified=" << service_map.size()
              << " seeded=" << seeded << " corpus=" << GekkoAOTSdk::kSdkServiceCorpusV1.size();
    for (const auto& [family, count] : families) std::cerr << ' ' << family << '=' << count;
    std::cerr << '\n';
  }

  // Some SDK entrypoints are intentionally referenced indirectly (thread exit
  // trampolines are a common example) and therefore never appear as a `bl`
  // target. Recover only exact corpus bodies that are CONTIGUOUS with an
  // already-proven SDK function. This preserves SDK library clusters without
  // returning to v70's unsafe "match at any aligned instruction" policy.
  struct AdjacentExactCandidate {
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    std::string name;
  };
  std::vector<AdjacentExactCandidate> adjacent_exact;
  std::size_t adjacency_recovered = 0;
  for (const auto& range : ranges) {
    for (std::uint32_t pc = range.start; pc + 12u <= range.end; pc += 4u) {
      if (discovered.contains(pc)) continue;
      const auto* prefix_code = runtime.Memory().Resolve(pc, 12u);
      if (!prefix_code) continue;
      const auto it = prefix3.find(CandidatePrefix3(prefix_code));
      if (it == prefix3.end()) continue;
      std::set<std::string_view> names;
      std::uint32_t selected_size = 0xffffffffu;
      for (const auto index : it->second) {
        const auto& exemplar = GekkoAOTSdk::kGroundTruthExemplars[index];
        const auto size64 = std::uint64_t{exemplar.instruction_count} * 4u;
        if (size64 < 12u || size64 > 0x10000u || pc + size64 > range.end) continue;
        const auto size = static_cast<std::uint32_t>(size64);
        const auto* code = runtime.Memory().Resolve(pc, size);
        if (!code) continue;
        const auto shape = GekkoAOTSdk::BuildStructuralShape(code, size);
        if (!GekkoAOTSdk::GroundTruthExact(
                shape, GekkoAOTSdk::GroundTruthShapes()[index]))
          continue;
        names.insert(exemplar.name);
        selected_size = std::min(selected_size, size);
      }
      if (names.size() == 1u && selected_size != 0xffffffffu)
        adjacent_exact.push_back({pc, selected_size, std::string(*names.begin())});
    }
  }

  const auto padding_only = [&](std::uint32_t begin, std::uint32_t end) {
    if (end < begin || end - begin > 16u || ((end - begin) & 3u)) return false;
    for (std::uint32_t pc = begin; pc < end; pc += 4u) {
      const auto* word = runtime.Memory().Resolve(pc, 4u);
      if (!word) return false;
      const auto raw = GekkoAOTSdk::StructuralReadBE32(word);
      if (raw != 0u && raw != 0x60000000u) return false; // zero / nop padding
    }
    return true;
  };
  for (unsigned pass = 0; pass < 16; ++pass) {
    bool changed = false;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> proven;
    proven.reserve(resolver.Functions().size());
    for (const auto& [address, fn] : resolver.Functions())
      if (resolver.Resolution(address) && !resolver.IsAmbiguous(address))
        proven.emplace_back(address, address + fn.size);

    for (const auto& candidate : adjacent_exact) {
      if (discovered.contains(candidate.address)) continue;
      bool nested = false;
      bool adjacent = false;
      const auto candidate_end = candidate.address + candidate.size;
      for (const auto& [start_pc, end_pc] : proven) {
        if (candidate.address > start_pc && candidate.address < end_pc) {
          nested = true;
          break;
        }
        if (candidate.address == end_pc || candidate_end == start_pc ||
            (candidate.address > end_pc && padding_only(end_pc, candidate.address)) ||
            (candidate_end < start_pc && padding_only(candidate_end, start_pc)))
          adjacent = true;
      }
      if (nested || !adjacent) continue;
      const auto* code = runtime.Memory().Resolve(candidate.address, candidate.size);
      if (!code) continue;
      resolver.AddFunction(candidate.address, candidate.size, 0, 0, code, {});
      resolver.Seed(candidate.address, candidate.name, 0.998,
                    "groundtruth-exact-sdk-adjacency");
      discovered.insert(candidate.address);
      ++stats.exact_functions;
      ++adjacency_recovered;
      changed = true;
    }
    if (!changed) break;
  }

  // Tiny two-instruction SDK leaves are only considered at a proven direct
  // call target (or DOL entry/range start), preventing accidental mid-function
  // matches while still recovering OSGetTick-style helpers.
  for (const auto pc : direct_targets) {
    if (discovered.contains(pc)) continue;
    const auto* range = ContainingRange(ranges, pc);
    if (!range || pc + 8u > range->end) continue;
    const auto* code = runtime.Memory().Resolve(pc, 8u);
    if (!code) continue;
    const auto a = GekkoAOTSdk::StructuralInstructionSkeleton(GekkoAOTSdk::StructuralReadBE32(code));
    const auto b = GekkoAOTSdk::StructuralInstructionSkeleton(GekkoAOTSdk::StructuralReadBE32(code + 4));
    const auto it = prefix2.find(Prefix2(a, b));
    if (it == prefix2.end()) continue;
    std::set<std::string_view> names;
    for (const auto index : it->second) {
      const auto shape = GekkoAOTSdk::BuildStructuralShape(code, 8u);
      if (GekkoAOTSdk::GroundTruthExact(shape, GekkoAOTSdk::GroundTruthShapes()[index]))
        names.insert(GekkoAOTSdk::kGroundTruthExemplars[index].name);
    }
    if (names.empty()) continue;
    resolver.AddFunction(pc, 8u, 0, 0, code, {});
    discovered.insert(pc);
    ++stats.exact_functions;
    if (names.size() == 1)
      resolver.Seed(pc, std::string(*names.begin()), 0.999, "groundtruth-tiny-exact-calltarget");
    else {
      resolver.MarkAmbiguous(pc);
      ++stats.ambiguous_functions;
    }
  }

  // Re-run the corpus graph resolver over the materialized exact functions and
  // then apply the same semantic collision repairs used by the old native SDK
  // path. No absolute game addresses are involved.
  const auto corpus = GekkoAOTSdk::ResolveGroundTruthCorpus(resolver);
  RepairSemanticOsIdentities(resolver);

  std::map<OsKind, std::vector<std::uint32_t>> os_candidates;
  std::vector<std::pair<SimpleHleKind, std::uint32_t>> simple_candidates;
  for (const auto& [address, resolution] : resolver.Resolutions()) {
    if (resolver.IsAmbiguous(address) || resolution.confidence < 0.95) continue;
    ++stats.resolved_functions;
    if (const auto kind = OsKindForName(resolution.name)) {
      os_candidates[*kind].push_back(address);
      ++stats.os_candidates;
    }
    if (const auto kind = SimpleKindForName(resolution.name))
      simple_candidates.emplace_back(*kind, address);
    if (OsKindForName(resolution.name) || SimpleKindForName(resolution.name)) {
      std::cerr << "GEKKOAOT_SDK_RESOLVE name=" << resolution.name
                << " pc=0x" << std::hex << address << std::dec
                << " confidence=" << resolution.confidence
                << " source=" << resolution.source << "\n";
    }
  }

  // OSContext's public FPU wrappers are tiny two-instruction assembly
  // leaves and older ground-truth corpora did not name them.  Recover them
  // only inside the already-proven OSContext cluster, immediately before a
  // structurally resolved OSSetCurrentContext.  This is intentionally much
  // narrower than a global `addi rN,r3,0; b ...` scan: retail SDKs place the
  // wrappers next to __OSLoad/__OSSaveFPUContext and OSSetCurrentContext, and
  // requiring an existing direct call-target prevents dead padding/data from
  // becoming a native hook.
  if (const auto set_it = os_candidates.find(OsKind::SetCurrentContext);
      set_it != os_candidates.end()) {
    constexpr std::uint32_t kMoveContextToR4 = 0x38830000u; // addi r4,r3,0
    constexpr std::uint32_t kMoveContextToR5 = 0x38a30000u; // addi r5,r3,0
    for (const auto set_pc : set_it->second) {
      const auto cluster_begin = set_pc > 0x40u ? set_pc - 0x40u : 0u;
      for (std::uint32_t pc = cluster_begin; pc + 8u <= set_pc; pc += 4u) {
        if (discovered.find(pc) == discovered.end()) continue;
        const auto* code = runtime.Memory().Resolve(pc, 8u);
        if (!code) continue;
        const auto first = GekkoAOTSdk::StructuralReadBE32(code);
        const auto branch = GekkoAOTSdk::StructuralReadBE32(code + 4u);
        if ((branch & 0xfc000003u) != 0x48000000u) continue; // b, not bl/ba

        // Sign-extend the 26-bit relative branch displacement.  The internal
        // FPU helpers live earlier in the same OSContext object.
        std::int32_t displacement = static_cast<std::int32_t>(branch & 0x03fffffcu);
        if (displacement & 0x02000000) displacement |= static_cast<std::int32_t>(0xfc000000u);
        const auto target = static_cast<std::uint32_t>(pc + 4u + displacement);
        if (target >= pc || set_pc - target > 0x800u) continue;

        if (first == kMoveContextToR4) {
          os_candidates[OsKind::LoadFpuContext].push_back(pc);
          resolver.Seed(pc, "OSLoadFPUContext", 0.999, "oscontext-cluster-fpu-wrapper");
          std::cerr << "GEKKOAOT_SDK_RESOLVE name=OSLoadFPUContext pc=0x"
                    << std::hex << pc << std::dec
                    << " confidence=0.999 source=oscontext-cluster-fpu-wrapper\n";
        } else if (first == kMoveContextToR5) {
          os_candidates[OsKind::SaveFpuContext].push_back(pc);
          resolver.Seed(pc, "OSSaveFPUContext", 0.999, "oscontext-cluster-fpu-wrapper");
          std::cerr << "GEKKOAOT_SDK_RESOLVE name=OSSaveFPUContext pc=0x"
                    << std::hex << pc << std::dec
                    << " confidence=0.999 source=oscontext-cluster-fpu-wrapper\n";
        }
      }
    }
    for (auto kind : {OsKind::LoadFpuContext, OsKind::SaveFpuContext}) {
      auto& v = os_candidates[kind];
      std::sort(v.begin(), v.end());
      v.erase(std::unique(v.begin(), v.end()), v.end());
    }
  }

  // OSGetThreadPriority is an ABI leaf, not merely an instruction-shape
  // identity. Its SDK contract is exactly `return thread->priority`, where
  // OSThread::priority lives at ABI offset 0x2d4. A normalized two-instruction
  // shape (`lwz r3, imm(r3); blr`) is far too weak: GHLE69 contains many such
  // accessors and v71 incorrectly selected CBGetBytesAvailableForRead at
  // 0x800d4384 (which loads offset 0x110) as OSGetThreadPriority.
  //
  // First require the exact OSThread ABI leaf `lwz r3, 0x2d4(r3); blr`.
  // A unique exact ABI leaf is already strong evidence (and lets stripped DOLs
  // keep the native getter even when OSSetThreadPriority itself is unreferenced).
  // If multiple exact ABI leaves ever survive, require a unique one in the
  // canonical forward OSThread cluster after a proven OSSetThreadPriority.
  // GetThreadPriority is optional for scheduler closure, so ambiguity fails
  // closed without disabling NativeOS.
  if (auto getter = os_candidates.find(OsKind::GetThreadPriority);
      getter != os_candidates.end()) {
    constexpr std::uint32_t kGetThreadPriorityLwz = 0x806302d4u;
    constexpr std::uint32_t kBlr = 0x4e800020u;
    constexpr std::uint32_t kMaxThreadClusterDistance = 0x180u;

    const auto before = getter->second.size();
    std::vector<std::uint32_t> abi_exact;
    for (const auto get_pc : getter->second) {
      const auto* code = runtime.Memory().Resolve(get_pc, 8u);
      if (!code) continue;
      const auto first = GekkoAOTSdk::StructuralReadBE32(code);
      const auto second = GekkoAOTSdk::StructuralReadBE32(code + 4u);
      if (first == kGetThreadPriorityLwz && second == kBlr)
        abi_exact.push_back(get_pc);
    }

    std::optional<std::uint32_t> selected;
    const char* proof = "none";
    if (abi_exact.size() == 1u) {
      selected = abi_exact.front();
      proof = "unique-abi";
    } else if (abi_exact.size() > 1u) {
      std::vector<std::uint32_t> clustered;
      if (const auto setter = os_candidates.find(OsKind::SetThreadPriority);
          setter != os_candidates.end()) {
        for (const auto get_pc : abi_exact) {
          for (const auto set_pc : setter->second) {
            if (get_pc <= set_pc) continue;
            const auto distance = get_pc - set_pc;
            if (distance >= 8u && distance <= kMaxThreadClusterDistance) {
              clustered.push_back(get_pc);
              break;
            }
          }
        }
      }
      if (clustered.size() == 1u) {
        selected = clustered.front();
        proof = "abi+osthread-cluster";
      }
    }

    if (selected)
      getter->second.assign(1u, *selected);
    else
      getter->second.clear();

    std::cerr << "GEKKOAOT_SDK_LEAF_PROOF_V74=1 name=OSGetThreadPriority"
              << " candidates=" << before
              << " abi_exact=" << abi_exact.size()
              << " abi=lwz-r3-thread-priority-0x2d4+blr"
              << " cluster=OSSetThreadPriority-forward<=0x180"
              << " proof=" << proof << " selected=";
    if (selected)
      std::cerr << "0x" << std::hex << *selected << std::dec;
    else
      std::cerr << "guest-aot";
    std::cerr << "\n";
  }

  std::vector<std::pair<SimpleHleKind,std::uint32_t>> proven_ai_callbacks;
  for(const auto& [handler_pc,sig]:service_map) {
    const auto name=std::string_view(sig->name);
    if(name!="__AIDHandler"&&name!="__AISHandler")continue;
    const auto* handler=runtime.Memory().Resolve(handler_pc,sig->size);if(!handler)continue;
    std::vector<std::uint32_t> candidates;
    for(const auto pc:direct_targets) {
      const auto* body=runtime.Memory().Resolve(pc,128);if(!body)continue;
      if(GekkoAOTSdk::ProveAiCallbackSetter({body,128},{handler,sig->size})) candidates.push_back(pc);
    }
    if(candidates.size()==1) proven_ai_callbacks.emplace_back(name=="__AIDHandler"?SimpleHleKind::AiRegisterDmaCallback:SimpleHleKind::AiRegisterStreamCallback,candidates.front());
  }
  const bool native_ai_callbacks=std::any_of(proven_ai_callbacks.begin(),proven_ai_callbacks.end(),[](const auto& p){return p.first==SimpleHleKind::AiRegisterDmaCallback;});

  // Small synchronous CARD wrappers can be structurally identical across
  // SDK releases (Delete and FastDelete differ only by a relocated bl).
  // Require the actual callee identity before replacing their ABI. A corpus
  // shape alone is deliberately insufficient for a destructive save API.
  const auto calls_expected=[&](std::uint32_t address,std::string_view expected) {
    for(unsigned offset=0;offset<32;offset+=4) {
      const auto* p=runtime.Memory().Resolve(address+offset,4);if(!p)return false;
      const auto w=GekkoAOTSdk::StructuralReadBE32(p);
      if((w&0xfc000003u)!=0x48000001u)continue;
      const auto displacement=static_cast<std::int32_t>(w<<6)>>6;
      const auto target=address+offset+(displacement&~3u);
      for(const auto& [pc,sig]:service_map)if(pc==target&&std::string_view(sig->name)==expected)return true;
      const auto* resolution=resolver.Resolution(target);
      return resolution&&resolution->name==expected&&!resolver.IsAmbiguous(target);
    }
    return false;
  };

  for (const auto& [kind, address] : simple_candidates) {
    // Keep the SDK thread wait/wakeup continuation; only the async request
    // below it performs media I/O, through our permanent native worker.
    if(kind==SimpleHleKind::DvdReadPrio)continue;

    if(kind==SimpleHleKind::AiGetCount) {
      const auto* body=runtime.Memory().Resolve(address,16);
      if(!body || !GekkoAOTSdk::ProveAiCounter({body,16}))continue;
    }

    if(kind==SimpleHleKind::OsUnlockSram || kind==SimpleHleKind::OsUnlockSramEx) {
      const auto first=kind==SimpleHleKind::OsUnlockSramEx?address-36:address;
      const auto* body=runtime.Memory().Resolve(first,88);
      if(!body || !GekkoAOTSdk::ProveSramUnlockFamily({body,88},first))continue;
    }
    // DVDOpen's original FST/path traversal is already hardware-free and
    // preserves the retail current-directory ABI. Keep it compiled until a
    // complete native cwd proof is available; DVDFastOpen/read are native.
    if (kind == SimpleHleKind::DvdOpen) continue;
    if((kind==SimpleHleKind::AiInit||kind==SimpleHleKind::AiReset||kind==SimpleHleKind::AiCheckInit||kind==SimpleHleKind::AiRegisterDmaCallback||kind==SimpleHleKind::AiRegisterStreamCallback)&&!native_ai_callbacks)continue;
    if(const auto expected=CardWrapperCallee(kind);!expected.empty()&&!calls_expected(address,expected)) continue;
    if(kind==SimpleHleKind::GxSetArray &&
       (!EnvEnabled("GEKKOAOT_NATIVE_GX_ARRAY_SERVICE",true) ||
        !GekkoAOTSdk::ProveGxSetArray(runtime.Memory().Resolve(address,140u),140u))) continue;
    if (kind==SimpleHleKind::PadRead && !EnvEnabled("GEKKOAOT_NATIVE_PAD",true)) continue;
    const bool normal = kind==SimpleHleKind::GxLoadNrmMtxImm ||
                        kind==SimpleHleKind::GxLoadNrmMtxImm3x3;
    if (normal && (!EnvEnabled("GEKKOAOT_NATIVE_GX_NORMAL_MATRIX",true) ||
        !GekkoAOTSdk::ProveGxNormalMatrix(runtime.Memory().Resolve(address,80u),80u,address,
            kind==SimpleHleKind::GxLoadNrmMtxImm3x3,
            [&](std::uint32_t pc,std::size_t bytes){return runtime.Memory().Resolve(pc,bytes);})))
      continue;
    if (kind==SimpleHleKind::DvdFastOpen &&
        (!EnvEnabled("GEKKOAOT_NATIVE_DVD_FST",true) ||
         !GekkoAOTSdk::ProveDvdFastOpen(runtime.Memory().Resolve(address,116u),116u))) continue;
    if (IsGxMatrixKind(kind) && !EnvEnabled("GEKKOAOT_GX_MATRIX_HLE", true))
      continue;
    runtime.RegisterSimpleHle(kind, address);
    manifest_hooks.emplace_back(address, SimpleKindName(kind));
    ++stats.simple_hooks;
  }

  // Retail synchronous adapters are frequently absent from the corpus, or
  // collide after relocation normalisation. Prove the complete adapter and
  // its independently named Async/__CARDSync callees instead. In particular,
  // never let a native mount return into an uninitialised guest CARDControl
  // wait loop which inlines CARDGetResultCode.
  struct CardAdapter { const char* async; SimpleHleKind kind; unsigned argc; };
  constexpr CardAdapter adapters[]{
    {"CARDMountAsync", SimpleHleKind::CardMount, 3},
    {"CARDReadAsync", SimpleHleKind::CardRead, 4},
    {"CARDWriteAsync", SimpleHleKind::CardWrite, 4},
    {"CARDCreateAsync", SimpleHleKind::CardCreate, 4},
    {"CARDDeleteAsync", SimpleHleKind::CardDelete, 2},
    {"CARDFastDeleteAsync", SimpleHleKind::CardFastDelete, 2},
    {"CARDSetStatusAsync", SimpleHleKind::CardSetStatus, 3},
    {"CARDCheckAsync", SimpleHleKind::CardCheck, 1},
    {"CARDFormatAsync", SimpleHleKind::CardFormat, 1},
  };
  for (const auto& adapter : adapters)
    for (const auto& [async_pc, sig] : service_map) {
      if (std::string_view(sig->name) != adapter.async) continue;
      for (const auto pc : direct_targets) {
        const auto* code = runtime.Memory().Resolve(pc, 72);
        const auto proof = GekkoAOTSdk::DecodeCardSyncWrapper(code, 72, pc, async_pc, adapter.argc);
        if (!proof) continue;
        const bool wait_proven = std::any_of(service_map.begin(), service_map.end(), [&](const auto& entry) {
          return entry.first == proof->wait && std::string_view(entry.second->name) == "__CARDSync";
        });
        if (!wait_proven) continue;
        runtime.RegisterSimpleHle(adapter.kind, pc);
        manifest_hooks.emplace_back(pc, SimpleKindName(adapter.kind));
        ++stats.simple_hooks;
      }
    }

  const bool native_os_enabled = EnvEnabled("GEKKOAOT_NATIVE_OS", false);
  const auto native_os_bisect = ParseNativeOsBisectMode(
      EnvText("GEKKOAOT_NATIVE_OS_BISECT", "all"));
  std::cerr << "GEKKOAOT_NATIVE_OS_PARITY_V153=1 default=guest-aot active="
            << (native_os_enabled ? "native-sdk" : "guest-aot-optout")
            << " reference=dolsdk2001+dolsdk2004 native-port-semantics\n";
  if (native_os_enabled)
    std::cerr << "GEKKOAOT_NATIVE_OS_BISECT_V147=1 mode="
              << NativeOsBisectModeName(native_os_bisect)
              << " leaf=disable|enable|restore|enable+restore|interrupts"
              << " cumulative=interrupts<scheduler<sync<all\n";
  std::vector<OsKind> missing;
  if (native_os_enabled && !IsInterruptOnlyBisect(native_os_bisect)) {
    for (const auto kind : kAllOsKinds) {
      if (IsOptionalForSchedulerClosure(kind)) continue;
      const auto it = os_candidates.find(kind);
      if (it == os_candidates.end() || it->second.empty()) missing.push_back(kind);
    }
  }
  stats.missing_scheduler = missing.size();

  // GEKKOAOT_NATIVE_OS_INTERRUPT_LEAVES_V150:
  // Keep the three retail MSR[EE] leaves in guest-AOT by default.  They are
  // tiny (OSDisable/OSEnable are five PPC instructions; OSRestore is nine),
  // and OSDisableInterrupts is also a restartable atomic sequence used by
  // OSLoadContext.  Replacing the body at a direct-HLE boundary changes more
  // than the final EE bit: retail variants expose exact scratch-register/CR
  // side effects and instruction-boundary/RAS semantics.  The translated AOT
  // body already runs natively on the host and preserves those semantics at
  // negligible cost, so NativeOS owns the higher-level scheduler/context/sync
  // services while these architectural leaves remain compiled guest code.
  //
  // Keep an explicit opt-in plus the v147 interrupt-only bisectors for
  // conformance work; neither is part of the correctness-first default.
  const bool native_interrupt_leaves =
      EnvEnabled("GEKKOAOT_NATIVE_OS_INTERRUPT_LEAVES", false);
  const bool interrupt_leaf_diagnostic = IsInterruptOnlyBisect(native_os_bisect);
  const bool install_interrupt_leaves =
      native_os_enabled && (native_interrupt_leaves || interrupt_leaf_diagnostic);
  if (native_os_enabled)
    std::cerr << "GEKKOAOT_NATIVE_OS_INTERRUPT_LEAVES_V150="
              << (install_interrupt_leaves ? 1 : 0)
              << " policy="
              << (interrupt_leaf_diagnostic ? "explicit-bisect"
                                             : (native_interrupt_leaves ? "host-optin"
                                                                        : "guest-aot-ras"))
              << "\n";

  if (install_interrupt_leaves) {
    for (const auto kind : {OsKind::DisableInterrupts, OsKind::EnableInterrupts,
                            OsKind::RestoreInterrupts}) {
      if (!InterruptBisectAdmits(kind, native_os_bisect)) continue;
      const auto found = os_candidates.find(kind);
      if (found == os_candidates.end()) continue;
      for (const auto address : found->second) {
        runtime.RegisterOsHook(kind, address);
        manifest_hooks.emplace_back(address, GekkoAOT::NativeOS::Name(kind));
        ++stats.os_hooks;
        std::cerr << "[native-os] HOOK " << GekkoAOT::NativeOS::Name(kind)
                  << " -> 0x" << std::hex << address << std::dec
                  << " source=standalone-structural\n";
      }
    }
  }

  if(native_ai_callbacks)for(const auto& [kind,pc]:proven_ai_callbacks){
    runtime.RegisterSimpleHle(kind,pc);manifest_hooks.emplace_back(pc,SimpleKindName(kind));++stats.simple_hooks;
  }
  // Exact absolute AI counter/trigger leaves at proven call targets. Register
  // constants are part of the proof; relocated lookalike getters are rejected.
  for(const auto pc:direct_targets){
    const auto* p=runtime.Memory().Resolve(pc,12);if(!p)continue;
    const auto a=GekkoAOTSdk::DeviceWord(p),b=GekkoAOTSdk::DeviceWord(p+4),c=GekkoAOTSdk::DeviceWord(p+8);
    if(c!=0x4e800020u)continue;
    std::optional<SimpleHleKind> kind;
    if(a==0x3c60cc00u&&b==0x80636c08u)kind=SimpleHleKind::AiGetCount;
    if(a==0x3c60cc00u&&b==0x80636c0cu)kind=SimpleHleKind::AiGetTrigger;
    if(a==0x3c80cc00u&&b==0x90646c0cu)kind=SimpleHleKind::AiSetTrigger;
    if(kind){runtime.RegisterSimpleHle(*kind,pc);manifest_hooks.emplace_back(pc,SimpleKindName(*kind));++stats.simple_hooks;}
  }

  if (native_os_enabled && IsInterruptOnlyBisect(native_os_bisect)) {
    std::cerr << "GEKKOAOT_NATIVE_OS_CLOSURE_AUTO_V8=1 policy=bisect-"
              << NativeOsBisectModeName(native_os_bisect)
              << " hooks=" << stats.os_hooks << " candidates=" << stats.os_candidates
              << " scheduler=guest-aot sync=guest-aot messages=guest-aot\n";
  } else if (native_os_enabled && missing.empty()) {
    const auto policy = EnvText("GEKKOAOT_NATIVE_OS_ADMISSION", "hot");
    const bool full = policy == "full";
    for (const auto& [kind, addresses] : os_candidates) {
      if (IsInterruptHelper(kind)) continue; // already installed above
      if (!NativeOsBisectAdmits(kind, native_os_bisect)) continue;
      if (!full && IsAlarmGuestByDefault(kind)) continue;
      for (const auto address : addresses) {
        runtime.RegisterOsHook(kind, address);
        manifest_hooks.emplace_back(address, GekkoAOT::NativeOS::Name(kind));
        ++stats.os_hooks;
        std::cerr << "[native-os] HOOK " << GekkoAOT::NativeOS::Name(kind)
                  << " -> 0x" << std::hex << address << std::dec
                  << " source=standalone-structural\n";
      }
    }
    std::cerr << "GEKKOAOT_NATIVE_OS_CLOSURE_AUTO_V8=1 policy=" << policy
              << " bisect=" << NativeOsBisectModeName(native_os_bisect)
              << " hooks=" << stats.os_hooks << " candidates=" << stats.os_candidates
              << "\n";
    std::cerr << "GEKKOAOT_NATIVE_OS_CONTEXT_COMPAT_V70=1 direct-sdk-context=native-sdk-ppc"
              << " scheduler-context=native-sdk-ppc queue-authority=guest+host-mirror"
              << " admission=" << (full ? "full" : "hot")
              << " bisect=" << NativeOsBisectModeName(native_os_bisect) << "\n";
  } else if (native_os_enabled) {
    std::cerr << "GEKKOAOT_NATIVE_OS_CLOSURE_AUTO_V8=0 action=guest-aot missing=";
    for (std::size_t i = 0; i < missing.size(); ++i) {
      if (i) std::cerr << ',';
      std::cerr << GekkoAOT::NativeOS::Name(missing[i]);
    }
    std::cerr << " safe-interrupt-hooks=" << stats.os_hooks << "\n";
  }

  std::vector<std::pair<std::uint32_t,std::string>> device_support;
  for(const auto support : {"OSDisableScheduler","OSEnableScheduler","__OSSetInterruptHandler","OSRegisterResetFunction","__DVDFSInit"}) {
    std::uint32_t selected=0; bool ambiguous=false;
    for(const auto& [pc,sig]:service_map) if(std::string_view(sig->name)==support) {
      if(selected && selected!=pc) ambiguous=true;
      selected=pc;
    }
    if(selected&&!ambiguous) {runtime.RegisterDeviceSupport(support,selected);device_support.emplace_back(selected,support);}
  }

  // Memory initialization has several retail compiler variants. Recognize
  // its actual five handler registrations/reset continuation rather than a
  // body hash or a game PC. This is the same proof required at execution.
  const auto support_pc=[&](std::string_view name){
    for(const auto& [pc,n]:device_support)if(n==name)return pc;
    return std::uint32_t(0);
  };
  const auto set_handler=support_pc("__OSSetInterruptHandler");
  const auto register_reset=support_pc("OSRegisterResetFunction");
  if(set_handler && register_reset)for(const auto pc:direct_targets){
    const auto* code=runtime.Memory().Resolve(pc,512);
    if(!code || GekkoAOTSdk::DeviceWord(code)!=0x7c0802a6u)continue;
    if(!GekkoAOTSdk::ProveMemoryInit({code,512},pc,set_handler,register_reset))continue;
    runtime.RegisterSimpleHle(SimpleHleKind::OsInitMemoryProtection,pc);
    manifest_hooks.emplace_back(pc,"__OSInitMemoryProtection");++stats.simple_hooks;
  }

  if (!manifest_output.empty() && !service_map.empty()) {
    // Per-game SDK service map (recognition only; hooks stay in the
    // manifest). Local state, never part of the repository.
    std::ofstream map_out(manifest_output.parent_path() / "sdk-service-map-v300.txt", std::ios::trunc);
    if (map_out) {
      map_out << "# GEKKOAOT_SDK_SERVICE_MAP_V300 address size name\n";
      for (const auto& [pc, sig] : service_map)
        map_out << "0x" << std::hex << std::setw(8) << std::setfill('0') << pc << std::dec
                << ' ' << sig->size << ' ' << sig->name << '\n';
    }
  }
  if (!manifest_output.empty()) {
    std::sort(manifest_hooks.begin(), manifest_hooks.end());
    manifest_hooks.erase(std::unique(manifest_hooks.begin(), manifest_hooks.end()),
                         manifest_hooks.end());
    std::filesystem::create_directories(manifest_output.parent_path());
    std::ofstream out(manifest_output, std::ios::trunc);
    if (!out) {
      std::cerr << "GEKKOAOT_NATIVE_SDK_MANIFEST_V2=0 reason=open-failed path=\""
                << manifest_output.string() << "\"\n";
    } else {
      out << "# GEKKOAOT_NATIVE_SDK_MANIFEST_V2\n";
      out << "# address name direct-token abi-class optional-native-binding\n";
      for(const auto& [pc,name]:device_support) out << "# device-support 0x" << std::hex << pc << std::dec << " " << name << "\n";
      std::size_t context_capture = 0;
      for (const auto& [address, name] : manifest_hooks) {
        const auto token = StaticDirectToken(name);
        out << "0x" << std::hex << std::setw(8) << std::setfill('0') << address
            << std::dec << ' ' << name;
        if (token != 0u)
          out << " 0x" << std::hex << std::setw(8) << std::setfill('0') << token << std::dec;
        if (const auto kind = OsKindForName(name); kind && IsFullFpContextBoundary(*kind)) {
          out << " full-fp-context";
          ++context_capture;
        } else if (const auto kind = OsKindForName(name); kind && IsContextCaptureBoundary(*kind)) {
          out << " context-capture";
          ++context_capture;
        }
        // All exact, admitted native services use generated named bindings.
        // Recognition and scheduler-closure admission above remain authoritative.
        const char* binding=nullptr;
        switch(token) {
#define SDK_SIMPLE(kind, text, abi) case (0xfffc0000u|unsigned(SimpleHleKind::kind)): binding="native:simple_" #kind; break;
#define SDK_OS(kind, text, abi) case (0xfffd0000u|unsigned(OsKind::kind)): binding="native:os_" #kind; break;
#include "sdk/native_services.def"
#undef SDK_SIMPLE
#undef SDK_OS
        }
        if(binding) {
          if(!OsKindForName(name) || !IsContextCaptureBoundary(*OsKindForName(name)))out << " leaf";
          out << ' ' << binding;
        }
        out << '\n';
      }
      std::cerr << "GEKKOAOT_NATIVE_OS_CONTEXT_ABI_V76=1 class=context-capture hooks="
                << context_capture
                << " state=nonvolatile-int+control+gqr+fp-classed(full-on-oscontext)\n";
      std::cerr << "GEKKOAOT_NATIVE_SDK_MANIFEST_V2=1 mode=write hooks="
                << manifest_hooks.size() << " path=\"" << manifest_output.string()
                << "\"\n";
    }
  }

  std::cerr << "GEKKOAOT_SDK_ENTRY_PROOF_V71=1 policy=direct-bl+sdk-adjacency"
            << " adjacency-recovered=" << adjacency_recovered
            << " arbitrary-aligned-hooks=0"
            << " closure=linked-core\n";
  std::cerr << "GEKKOAOT_NATIVE_OS_LINKAGE_V71=1 core=atomic"
            << " high-level=install-when-linked-and-proven"
            << " absent-sdk-entrypoints=non-blocking\n";

  std::cerr << "GEKKOAOT_NATIVE_SDK_AUTO_V1=1 scanned_words=" << stats.scanned_words
            << " call_targets=" << stats.direct_call_targets
            << " exact=" << stats.exact_functions
            << " ambiguous=" << stats.ambiguous_functions
            << " resolved=" << stats.resolved_functions
            << " os_candidates=" << stats.os_candidates
            << " os_hooks=" << stats.os_hooks
            << " simple_hooks=" << stats.simple_hooks
            << " corpus_exact=" << corpus.exact
            << " corpus_fuzzy=" << corpus.fuzzy
            << " corpus_graph=" << corpus.graph << "\n";
  return stats;
}

bool RegisterNativeSdkManifest(HostRuntime& runtime,
                               const std::filesystem::path& manifest_path) {
  std::ifstream in(manifest_path);
  if (!in) {
    std::cerr << "GEKKOAOT_NATIVE_SDK_MANIFEST_V2=0 reason=open-failed path=\""
              << manifest_path.string() << "\"\n";
    return false;
  }

  std::size_t hooks = 0;
  std::string line;
  std::map<std::uint32_t,SimpleHleKind> declared;
  while(std::getline(in,line)) {
    if(line.empty()||line[0]=='#')continue;
    std::istringstream row(line);std::string pc,name;
    if(!(row>>pc>>name))return false;
    if(const auto kind=SimpleKindForName(name)) {
      char* end=nullptr;const auto address=std::strtoul(pc.c_str(),&end,0);
      if(!end||*end||!address||address>0xfffffffful)return false;
      const auto [it,inserted]=declared.emplace(std::uint32_t(address),*kind);
      if(!inserted&&it->second!=*kind)return false;
    }
  }
  in.clear();in.seekg(0);
  while (std::getline(in, line)) {
    if(line.starts_with("# device-support ")) {
      std::istringstream support(line.substr(17));std::string pc,name;
      if(!(support>>pc>>name))return false;
      char* end=nullptr;const auto address=std::strtoul(pc.c_str(),&end,0);
      if(!end||*end||!address||address>0xfffffffful)return false;
      runtime.RegisterDeviceSupport(name,static_cast<std::uint32_t>(address));continue;
    }
    if (line.empty() || line[0] == '#') continue;
    std::istringstream row(line);
    std::string address_text, name;
    if (!(row >> address_text >> name)) return false;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(address_text.c_str(), &end, 0);
    if (!end || *end != '\0' || parsed == 0 || parsed > 0xfffffffful) return false;
    const auto address = static_cast<std::uint32_t>(parsed);
    if (const auto kind = OsKindForName(name)) {
      runtime.RegisterOsHook(*kind, address);
    } else if (const auto kind = SimpleKindForName(name)) {
      if(const auto expected=CardWrapperCallee(*kind);!expected.empty()) {
        const auto async=SimpleKindForName(expected);
        const auto* body=runtime.Memory().Resolve(address,32);
        bool proven=false;
        if(body&&async)for(unsigned off=0;off<32;off+=4) {
          const auto w=GekkoAOTSdk::DeviceWord(body+off);
          if((w&0xfc000003u)!=0x48000001u)continue;
          const auto target=address+off+(std::uint32_t(std::int32_t(w<<6)>>6)&~3u);
          const auto found=declared.find(target);
          proven=found!=declared.end()&&found->second==*async;break;
        }
        if(!proven) {
          std::cerr<<"GEKKOAOT_NATIVE_SDK_MANIFEST_V2=0 reason=unproven-card-callee name="<<name<<" action=recompile-with-current-resolver\n";
          return false;
        }
      }
      if (IsGxMatrixKind(*kind) && !EnvEnabled("GEKKOAOT_GX_MATRIX_HLE", true))
        continue;
      runtime.RegisterSimpleHle(*kind, address);
    } else {
      std::cerr << "GEKKOAOT_NATIVE_SDK_MANIFEST_V2=0 reason=unknown-hook name="
                << name << "\n";
      return false;
    }
    ++hooks;
  }
  std::cerr << "GEKKOAOT_NATIVE_SDK_MANIFEST_V2=1 mode=load hooks=" << hooks
            << " path=\"" << manifest_path.string() << "\"\n";
  return true;
}

} // namespace GekkoAOT::Native
