#pragma once

#include "core/cpu_state.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GekkoAOT::NativeOS {

// Host-side implementation of the common Dolphin OS SDK scheduling surface.
// It deliberately remains cooperative: one CPUState is running at a time, just
// like the single-core GameCube CPU.  Guest OSThread/OSContext objects stay
// authoritative ABI-visible state while run/wait queues are accelerated here.
enum class Kind : std::uint8_t {
  InitThreadQueue,
  GetCurrentThread,
  IsThreadTerminated,
  DisableScheduler,
  EnableScheduler,
  SelectThread,
  Reschedule,
  YieldThread,
  CreateThread,
  ExitThread,
  CancelThread,
  JoinThread,
  DetachThread,
  ResumeThread,
  SuspendThread,
  SleepThread,
  WakeupThread,
  SetThreadPriority,
  GetThreadPriority,
  SaveContext,
  LoadContext,
  ClearContext,
  InitContext,
  GetCurrentContext,
  SetCurrentContext,
  InitMutex,
  LockMutex,
  UnlockMutex,
  TryLockMutex,
  InitCond,
  WaitCond,
  SignalCond,
  InitMessageQueue,
  SendMessage,
  ReceiveMessage,
  JamMessage,
  InitAlarm,
  CreateAlarm,
  SetAlarm,
  SetAbsAlarm,
  SetPeriodicAlarm,
  CancelAlarm,
  CheckAlarmQueue,
  DisableInterrupts,
  EnableInterrupts,
  RestoreInterrupts,
  Count,
};

inline const char* Name(Kind kind) {
  switch (kind) {
  case Kind::InitThreadQueue: return "OSInitThreadQueue";
  case Kind::GetCurrentThread: return "OSGetCurrentThread";
  case Kind::IsThreadTerminated: return "OSIsThreadTerminated";
  case Kind::DisableScheduler: return "OSDisableScheduler";
  case Kind::EnableScheduler: return "OSEnableScheduler";
  case Kind::SelectThread: return "SelectThread";
  case Kind::Reschedule: return "__OSReschedule";
  case Kind::YieldThread: return "OSYieldThread";
  case Kind::CreateThread: return "OSCreateThread";
  case Kind::ExitThread: return "OSExitThread";
  case Kind::CancelThread: return "OSCancelThread";
  case Kind::JoinThread: return "OSJoinThread";
  case Kind::DetachThread: return "OSDetachThread";
  case Kind::ResumeThread: return "OSResumeThread";
  case Kind::SuspendThread: return "OSSuspendThread";
  case Kind::SleepThread: return "OSSleepThread";
  case Kind::WakeupThread: return "OSWakeupThread";
  case Kind::SetThreadPriority: return "OSSetThreadPriority";
  case Kind::GetThreadPriority: return "OSGetThreadPriority";
  case Kind::SaveContext: return "OSSaveContext";
  case Kind::LoadContext: return "OSLoadContext";
  case Kind::ClearContext: return "OSClearContext";
  case Kind::InitContext: return "OSInitContext";
  case Kind::GetCurrentContext: return "OSGetCurrentContext";
  case Kind::SetCurrentContext: return "OSSetCurrentContext";
  case Kind::InitMutex: return "OSInitMutex";
  case Kind::LockMutex: return "OSLockMutex";
  case Kind::UnlockMutex: return "OSUnlockMutex";
  case Kind::TryLockMutex: return "OSTryLockMutex";
  case Kind::InitCond: return "OSInitCond";
  case Kind::WaitCond: return "OSWaitCond";
  case Kind::SignalCond: return "OSSignalCond";
  case Kind::InitMessageQueue: return "OSInitMessageQueue";
  case Kind::SendMessage: return "OSSendMessage";
  case Kind::ReceiveMessage: return "OSReceiveMessage";
  case Kind::JamMessage: return "OSJamMessage";
  case Kind::InitAlarm: return "OSInitAlarm";
  case Kind::CreateAlarm: return "OSCreateAlarm";
  case Kind::SetAlarm: return "OSSetAlarm";
  case Kind::SetAbsAlarm: return "OSSetAbsAlarm";
  case Kind::SetPeriodicAlarm: return "OSSetPeriodicAlarm";
  case Kind::CancelAlarm: return "OSCancelAlarm";
  case Kind::CheckAlarmQueue: return "OSCheckAlarmQueue";
  case Kind::DisableInterrupts: return "OSDisableInterrupts";
  case Kind::EnableInterrupts: return "OSEnableInterrupts";
  case Kind::RestoreInterrupts: return "OSRestoreInterrupts";
  case Kind::Count: break;
  }
  return "OS?";
}

class Service {
  // Low-memory globals installed by the GameCube SDK.
  static constexpr std::uint32_t kCurrentContext = 0x800000D4u;
  static constexpr std::uint32_t kActiveThreadHead = 0x800000DCu;
  static constexpr std::uint32_t kCurrentThread = 0x800000E4u;
  static constexpr std::uint32_t kPhysicalCurrentContext = 0x800000C0u;
  static constexpr std::uint32_t kFpuContext = 0x800000D8u;
  static constexpr std::uint32_t kMsrEE = 0x00008000u;
  static constexpr std::uint32_t kMsrFP = 0x00002000u;
  static constexpr std::uint32_t kMsrRI = 0x00000002u;
  static constexpr std::uint32_t kMsrRfiMask = 0x87c0ffffu;
  static constexpr std::uint32_t kMsrPow = 0x00040000u;

  // SelectThread() in Dolphin OS switches to a dedicated IdleContext when no
  // runnable OSThread exists, enables EE and spins until an interrupt makes a
  // thread runnable. GekkoAOT needs the same state because the host scheduler
  // must never treat an empty run queue as an HLE failure. 0x80002D00 lives in
  // the Dolphin-OS reserved low-memory range below the normal 0x80003100 game
  // arena and gives the standalone host a guest-visible context for exception
  // vector save/restore while the scheduler is idle.
  static constexpr std::uint32_t kHostIdleContext = 0x80002D00u;
  static constexpr std::uint32_t kHostIdlePc = 0xFFFFFFFCu;

  static constexpr std::uint16_t kThreadReady = 1;
  static constexpr std::uint16_t kThreadRunning = 2;
  static constexpr std::uint16_t kThreadWaiting = 4;
  static constexpr std::uint16_t kThreadMoribund = 8;
  static constexpr std::uint16_t kThreadDetach = 1;

  // OSThread offsets (Dolphin SDK ABI).
  static constexpr std::uint32_t kThreadState = 0x2C8;
  static constexpr std::uint32_t kThreadAttr = 0x2CA;
  static constexpr std::uint32_t kThreadSuspend = 0x2CC;
  static constexpr std::uint32_t kThreadPriority = 0x2D0;
  static constexpr std::uint32_t kThreadBase = 0x2D4;
  static constexpr std::uint32_t kThreadValue = 0x2D8;
  static constexpr std::uint32_t kThreadQueue = 0x2DC;
  static constexpr std::uint32_t kThreadLinkNext = 0x2E0;
  static constexpr std::uint32_t kThreadLinkPrev = 0x2E4;
  static constexpr std::uint32_t kThreadJoinQueue = 0x2E8;
  static constexpr std::uint32_t kThreadMutex = 0x2F0;
  static constexpr std::uint32_t kThreadMutexQueue = 0x2F4;
  static constexpr std::uint32_t kThreadActiveNext = 0x2FC;
  static constexpr std::uint32_t kThreadActivePrev = 0x300;
  static constexpr std::uint32_t kThreadStackBase = 0x304;
  static constexpr std::uint32_t kThreadStackEnd = 0x308;
  static constexpr std::uint32_t kThreadSize = 0x30C;

  // OSContext offsets.
  static constexpr std::uint32_t kCtxCr = 0x080;
  static constexpr std::uint32_t kCtxLr = 0x084;
  static constexpr std::uint32_t kCtxCtr = 0x088;
  static constexpr std::uint32_t kCtxXer = 0x08C;
  static constexpr std::uint32_t kCtxFpr = 0x090;
  static constexpr std::uint32_t kCtxFpscr = 0x194;
  static constexpr std::uint32_t kCtxSrr0 = 0x198;
  static constexpr std::uint32_t kCtxSrr1 = 0x19C;
  static constexpr std::uint32_t kCtxMode = 0x1A0;
  static constexpr std::uint32_t kCtxState = 0x1A2;
  static constexpr std::uint32_t kCtxGqr = 0x1A4;
  static constexpr std::uint32_t kCtxPsf = 0x1C8;
  static constexpr std::uint32_t kCtxSize = 0x2C8;

  struct PendingSend { std::uint32_t mq = 0; std::uint32_t message = 0; bool jam = false; };
  struct PendingReceive { std::uint32_t mq = 0; std::uint32_t output = 0; };
  struct PendingCond { std::uint32_t mutex = 0; std::uint32_t count = 1; };
  struct Alarm {
    std::uint32_t address = 0;
    std::uint32_t handler = 0;
    std::uint64_t fire = 0;
    std::uint64_t period = 0;
    std::uint64_t start = 0;
    bool active = false;
  };
  struct AlarmReturn {
    CPUState state{};
    std::uint32_t address = 0;
    std::uint32_t stack_pointer = 0;
    bool valid = false;
  };

  std::vector<std::uint32_t> known_threads_;
  std::vector<std::uint32_t> ready_;
  struct FpShadow {
    std::uint32_t context = 0;
    std::array<double, 32> fpr{};
    std::array<double, 32> ps1{};
    std::uint32_t fpscr = 0;
    bool valid = false;
  };
  std::vector<FpShadow> fp_shadows_;
  std::unordered_map<std::uint32_t, std::deque<std::uint32_t>> wait_queues_;
  std::unordered_map<std::uint32_t, PendingSend> pending_sends_;
  std::unordered_map<std::uint32_t, PendingReceive> pending_receives_;
  std::unordered_map<std::uint32_t, PendingCond> pending_cond_;
  std::array<std::uint32_t, static_cast<std::size_t>(Kind::Count)> entrypoints_{};
  std::vector<std::uint32_t> entrypoint_addresses_;
  static constexpr std::uint32_t kEntrypointPageShift = 12;
  static constexpr std::size_t kEntrypointPageWords = 1u << (32 - kEntrypointPageShift - 6);
  std::array<std::uint64_t, kEntrypointPageWords> entrypoint_pages_{};
  std::unordered_map<std::uint32_t, Alarm> alarms_;
  AlarmReturn alarm_return_{};
  std::uint32_t current_thread_ = 0;
  std::uint32_t scheduler_disable_ = 0;
  std::uint64_t switches_ = 0;
  std::uint64_t hle_calls_ = 0;
  std::uint64_t alarm_callbacks_ = 0;
  std::uint64_t idle_entries_ = 0;
  std::uint64_t ready_reconcile_calls_ = 0;
  std::uint64_t ready_reconcile_skips_ = 0;
  std::uint64_t ready_probe_reads_ = 0;
  std::uint64_t ready_snapshot_reads_ = 0;
  std::uint64_t ready_compactions_ = 0;
  std::uint64_t ready_peek_calls_ = 0;
  std::uint64_t thread_sanity_rejects_ = 0;
  std::uint64_t active_link_faults_ = 0;
  std::uint32_t active_head_cache_ = 0;
  std::size_t ready_probe_cursor_ = 0;
  bool active_head_valid_ = false;
  bool scheduler_idle_ = false;
  // SDK SelectThread refuses to switch while an interrupt handler owns a
  // temporary OSContext.  Wakeups during VI/DI/DSP IRQs must therefore make
  // threads runnable now and defer the actual context switch until the handler
  // restores the interrupted OSThread context.
  bool reschedule_pending_ = false;
  bool initialized_ = false;

  static std::uint16_t ReadBE16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
  }
  static std::uint32_t ReadBE32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
  }
  static std::uint64_t ReadBE64(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>(ReadBE32(p)) << 32) | ReadBE32(p + 4);
  }
  static void WriteBE16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8); p[1] = static_cast<std::uint8_t>(v);
  }
  static void WriteBE32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24); p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8); p[3] = static_cast<std::uint8_t>(v);
  }
  static void WriteBE64(std::uint8_t* p, std::uint64_t v) {
    WriteBE32(p, static_cast<std::uint32_t>(v >> 32)); WriteBE32(p + 4, static_cast<std::uint32_t>(v));
  }

  static std::uint8_t* Ptr(CPUState* state, std::uint32_t address, std::uint32_t size) {
    if (!state || !state->external_pointer || size == 0) return nullptr;
    return static_cast<std::uint8_t*>(state->external_pointer(state, address, size));
  }
  static const std::uint8_t* Ptr(const CPUState* state, std::uint32_t address, std::uint32_t size) {
    if (!state || !state->external_pointer || size == 0) return nullptr;
    return static_cast<const std::uint8_t*>(state->external_pointer(const_cast<CPUState*>(state), address, size));
  }
  static bool Read16(CPUState* state, std::uint32_t address, std::uint16_t* out) {
    const auto* p = Ptr(state, address, 2); if (!p || !out) return false; *out = ReadBE16(p); return true;
  }
  static bool Read32(CPUState* state, std::uint32_t address, std::uint32_t* out) {
    const auto* p = Ptr(state, address, 4); if (!p || !out) return false; *out = ReadBE32(p); return true;
  }
  static bool Read64(CPUState* state, std::uint32_t address, std::uint64_t* out) {
    const auto* p = Ptr(state, address, 8); if (!p || !out) return false; *out = ReadBE64(p); return true;
  }
  static bool Write16(CPUState* state, std::uint32_t address, std::uint16_t v) {
    auto* p = Ptr(state, address, 2); if (!p) return false; WriteBE16(p, v); return true;
  }
  static bool Write32(CPUState* state, std::uint32_t address, std::uint32_t v) {
    auto* p = Ptr(state, address, 4); if (!p) return false; WriteBE32(p, v); return true;
  }
  static bool Write64(CPUState* state, std::uint32_t address, std::uint64_t v) {
    auto* p = Ptr(state, address, 8); if (!p) return false; WriteBE64(p, v); return true;
  }

  static std::uint64_t DoubleBits(double value) {
    union { double d; std::uint64_t u; } cvt{}; cvt.d = value; return cvt.u;
  }
  static double BitsDouble(std::uint64_t value) {
    union { double d; std::uint64_t u; } cvt{}; cvt.u = value; return cvt.d;
  }

  void RememberThread(std::uint32_t thread) {
    if (!thread) return;
    if (std::find(known_threads_.begin(), known_threads_.end(), thread) == known_threads_.end())
      known_threads_.push_back(thread);
  }

  FpShadow* FindFpShadow(std::uint32_t context) {
    for (auto& shadow : fp_shadows_) if (shadow.context == context) return &shadow;
    return nullptr;
  }
  const FpShadow* FindFpShadow(std::uint32_t context) const {
    for (const auto& shadow : fp_shadows_) if (shadow.context == context) return &shadow;
    return nullptr;
  }
  void InvalidateFpShadow(std::uint32_t context) {
    if (auto* shadow = FindFpShadow(context)) shadow->valid = false;
  }
  void SaveFpShadow(const CPUState* state, std::uint32_t context) {
    if (!state || !context) return;
    auto* shadow = FindFpShadow(context);
    if (!shadow) {
      fp_shadows_.push_back({});
      shadow = &fp_shadows_.back();
      shadow->context = context;
    }
    std::copy_n(state->fpr, 32, shadow->fpr.begin());
    std::copy_n(state->ps1, 32, shadow->ps1.begin());
    shadow->fpscr = state->fpscr;
    shadow->valid = true;
  }
  bool RestoreFpShadow(CPUState* state, std::uint32_t context) const {
    if (!state) return false;
    const auto* shadow = FindFpShadow(context);
    if (!shadow || !shadow->valid) return false;
    std::copy_n(shadow->fpr.begin(), 32, state->fpr);
    std::copy_n(shadow->ps1.begin(), 32, state->ps1);
    state->fpscr = shadow->fpscr;
    return true;
  }

  bool InitializeContext(CPUState* state, std::uint32_t context,
                         std::uint32_t pc, std::uint32_t sp) {
    auto* out = Ptr(state, context, kCtxSize);
    if (!out) return false;

    // GEKKOAOT_NATIVE_OS_SDK_CONTEXT_V68:
    // Mirror the retail OSInitContext field contract observed in the SDK PPC
    // implementation instead of memset()ing the whole OSContext.  FPR/PS data
    // are lazy and are intentionally left alone while mode/state invalidate
    // them.  The initialized integer/GQR fields match what OSLoadContext may
    // consume before the new thread has ever run.
    WriteBE32(out + kCtxSrr0, pc);
    WriteBE32(out + 1 * 4, sp);
    WriteBE32(out + kCtxSrr1, 0x9032u);
    WriteBE32(out + kCtxCr, 0);
    WriteBE32(out + kCtxXer, 0);
    WriteBE32(out + 2 * 4, state->gpr[2]);
    WriteBE32(out + 13 * 4, state->gpr[13]);
    for (unsigned i = 3; i <= 12; ++i) WriteBE32(out + i * 4, 0);
    for (unsigned i = 14; i < 32; ++i) WriteBE32(out + i * 4, 0);
    for (unsigned i = 0; i < 8; ++i) WriteBE32(out + kCtxGqr + i * 4, 0);
    WriteBE16(out + kCtxMode, 0);
    WriteBE16(out + kCtxState, 0);
    InvalidateFpShadow(context);
    std::uint32_t owner = 0;
    if (Read32(state, kFpuContext, &owner) && owner == context)
      Write32(state, kFpuContext, 0);
    return true;
  }

  bool SaveContext(CPUState* state, std::uint32_t context, std::uint32_t resume_pc,
                   bool resumed_value = false) {
    auto* out = Ptr(state, context, kCtxSize);
    if (!out) return false;

    // GEKKOAOT_NATIVE_OS_CONTEXT_SHADOW_V65:
    // Match the retail OSSaveContext ABI instead of serializing the entire
    // CPUState on every cooperative switch. The SDK only saves the nonvolatile
    // integer registers, GQR1-GQR7 and control state; r3 in the *saved* context
    // becomes 1 so a later OSLoadContext resumes through the non-zero path.
    // Floating/paired-single registers are lazy in the SDK. GekkoAOT keeps a
    // host shadow per OSContext so AOT code remains correct even though it does
    // not currently take a hardware FP-unavailable exception on every thread
    // ownership transition. This removes two 32-double endian copies from the
    // ordinary scheduler hot path without weakening guest-visible semantics.
    for (unsigned i = 13; i < 32; ++i) WriteBE32(out + i * 4, state->gpr[i]);
    WriteBE32(out + 1 * 4, state->gpr[1]);
    WriteBE32(out + 2 * 4, state->gpr[2]);
    // Direct OSSaveContext must resume with r3=1, but NativeOS also uses this
    // helper to save continuations for *other* SDK calls (SelectThread,
    // SuspendThread, message waits, etc.).  Those continuations must preserve
    // the return value already staged in r3; forcing 1 there corrupts the
    // caller-visible result and can trap the scheduler in a context-switch
    // loop.
    WriteBE32(out + 3 * 4, resumed_value ? 1u : state->gpr[3]);
    WriteBE32(out + kCtxCr, state->cr);
    WriteBE32(out + kCtxLr, state->lr);
    WriteBE32(out + kCtxCtr, state->ctr);
    WriteBE32(out + kCtxXer, state->xer);
    WriteBE32(out + kCtxSrr0, resume_pc);
    WriteBE32(out + kCtxSrr1, state->msr);
    for (unsigned i = 1; i < 8; ++i) WriteBE32(out + kCtxGqr + i * 4, state->gqr[i]);
    SaveFpShadow(state, context);
    return true;
  }

  bool LoadContext(CPUState* state, std::uint32_t context) {
    auto* in = Ptr(state, context, kCtxSize);
    if (!in) return false;

    // Match the SDK's handwritten OSLoadContext rather than treating OSContext
    // as a flat register dump.  OSSaveContext only saves r13-r31; exception
    // contexts set state bit 1 and are the only contexts for which r5-r12 are
    // valid.  Restoring stale r5-r12 on an ordinary thread switch can corrupt
    // guest state, which is why the context family stayed guest-AOT until now.
    std::uint32_t resume_pc = ReadBE32(in + kCtxSrr0);
    bool ras_rewound = false;
    const auto ras_begin = Entrypoint(Kind::DisableInterrupts);
    if (ras_begin != 0u) {
      // __RAS_OSDisableInterrupts_end is the instruction immediately before
      // the final blr: begin + 0x10 for the retail SDK leaf used by GC titles.
      const auto ras_end = ras_begin + 0x10u;
      // Retail OSLoadContext uses strict bounds for the restartable atomic
      // sequence: only PCs *inside* the RAS are rewound.  The entry itself
      // and the final instruction are already valid restart points.
      if (resume_pc > ras_begin && resume_pc < ras_end) {
        resume_pc = ras_begin;
        WriteBE32(in + kCtxSrr0, resume_pc);
        ras_rewound = true;
      }
    }

    state->gpr[0] = ReadBE32(in + 0 * 4);
    state->gpr[1] = ReadBE32(in + 1 * 4);
    state->gpr[2] = ReadBE32(in + 2 * 4);

    auto context_state = ReadBE16(in + kCtxState);
    const bool exception_saved = (context_state & 2u) != 0;
    if (exception_saved) {
      context_state = static_cast<std::uint16_t>(context_state & ~2u);
      WriteBE16(in + kCtxState, context_state);
      for (unsigned i = 5; i < 32; ++i)
        state->gpr[i] = ReadBE32(in + i * 4);
    } else {
      for (unsigned i = 13; i < 32; ++i)
        state->gpr[i] = ReadBE32(in + i * 4);
    }

    state->cr = ReadBE32(in + kCtxCr);
    state->lr = ReadBE32(in + kCtxLr);
    state->ctr = ReadBE32(in + kCtxCtr);
    state->xer = ReadBE32(in + kCtxXer);

    // If the guest explicitly marked an FPU payload valid, it is authoritative.
    // Otherwise restore the host shadow captured by the native scheduler. This
    // mirrors the SDK's lazy-FPU ownership without forcing 512+ bytes of guest
    // endian traffic on every thread switch.
    if ((context_state & 1u) != 0) {
      for (unsigned i = 0; i < 32; ++i)
        state->fpr[i] = BitsDouble(ReadBE64(in + kCtxFpr + i * 8));
      state->fpscr = ReadBE32(in + kCtxFpscr);
      for (unsigned i = 0; i < 32; ++i)
        state->ps1[i] = BitsDouble(ReadBE64(in + kCtxPsf + i * 8));
      SaveFpShadow(state, context);
    } else {
      RestoreFpShadow(state, context);
    }

    // OSLoadContext restores GQR1-GQR7; GQR0 is deliberately untouched.
    for (unsigned i = 1; i < 8; ++i)
      state->gqr[i] = ReadBE32(in + kCtxGqr + i * 4);

    const auto saved_msr = ReadBE32(in + kCtxSrr1);
    state->srr0 = resume_pc;
    state->srr1 = saved_msr;
    // Mirror the runtime's PPC rfi helper: only architecturally writable RFI
    // fields come from SRR1 and POW is cleared on return from interrupt.
    state->msr = (state->msr & ~kMsrRfiMask) | (saved_msr & kMsrRfiMask);
    state->msr &= ~kMsrPow;
    state->pc = resume_pc & ~3u;

    // r3/r4 are restored last by the retail assembly, after all scratch use.
    state->gpr[4] = ReadBE32(in + 4 * 4);
    state->gpr[3] = ReadBE32(in + 3 * 4);

    static unsigned load_logs = 0u;
    if (load_logs++ < 32u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_OS_LOAD_CONTEXT_V64=1 ctx=%08x pc=%08x exc=%u fp=%u ras=%u\n",
                   context, state->pc, exception_saved ? 1u : 0u,
                   (context_state & 1u) ? 1u : 0u, ras_rewound ? 1u : 0u);
    return state->pc != 0;
  }

  void ReturnU32(CPUState* state, std::uint32_t value) {
    state->gpr[3] = value; state->pc = state->lr;
  }
  void ReturnVoid(CPUState* state) { state->pc = state->lr; }

  bool SetCurrentContextPointer(CPUState* state, std::uint32_t context) {
    if (!Write32(state, kCurrentContext, context)) return false;
    return Write32(state, kPhysicalCurrentContext, context & 0x3fffffffu);
  }

  bool ApplyCurrentContext(CPUState* state, std::uint32_t context) {
    if (!state || !Ptr(state, context, kCtxSize)) return false;
    std::uint32_t owner = 0, saved_msr = 0;
    if (!Read32(state, kFpuContext, &owner) ||
        !Read32(state, context + kCtxSrr1, &saved_msr) ||
        !SetCurrentContextPointer(state, context))
      return false;

    // Exact retail OSSetCurrentContext lazy-FPU transition.  If this context
    // already owns the FPU, the SDK marks FP in the *saved* SRR1 but does not
    // force the live MSR FP bit on.  If another context owns it, both are
    // cleared.  RI is then enabled in the live MSR.
    if (owner == context) {
      saved_msr |= kMsrFP;
    } else {
      saved_msr &= ~kMsrFP;
      state->msr &= ~kMsrFP;
    }
    Write32(state, context + kCtxSrr1, saved_msr);
    state->msr |= kMsrRI;
    return true;
  }

  bool EnterIdle(CPUState* state) {
    if (!state) return false;
    auto* idle_context = Ptr(state, kHostIdleContext, kCtxSize);
    if (!idle_context) return false;

    // Mirror SelectThread's IdleContext path: no current OSThread, a distinct
    // current OSContext for asynchronous exception save state, and EE enabled
    // while waiting for a hardware interrupt to make a thread runnable.
    std::fill_n(idle_context, kCtxSize, std::uint8_t{0});
    if (!Write32(state, kCurrentThread, 0) ||
        !ApplyCurrentContext(state, kHostIdleContext))
      return false;
    current_thread_ = 0;
    scheduler_idle_ = true;
    state->msr |= kMsrEE;
    state->pc = kHostIdlePc;
    ++idle_entries_;
    return true;
  }

  bool SwitchOrIdle(CPUState* state) {
    const auto next = PopNext(state);
    if (next) return SetCurrent(state, next);
    return EnterIdle(state);
  }

  bool Bootstrap(CPUState* state) {
    if (!state) return false;
    // NativeOS owns __OSCurrentThread after the first successful import. Do not
    // re-resolve low memory and reinsert the same thread into a hash container
    // on every HLE call. This was visible as ~8% std::_Hashtable::_M_emplace_uniq
    // in retail games with frequent context switches.
    if (initialized_) return scheduler_idle_ || current_thread_ != 0;

    std::uint32_t current = 0;
    if (Read32(state, kCurrentThread, &current) && current && Ptr(state, current, kThreadSize)) {
      current_thread_ = current;
      RememberThread(current);
      std::uint16_t thread_state = 0;
      if (Read16(state, current + kThreadState, &thread_state) && thread_state == 0)
        Write16(state, current + kThreadState, kThreadRunning);
      initialized_ = true;
      return true;
    }
    return false;
  }

  std::int32_t Priority(CPUState* state, std::uint32_t thread) const {
    std::uint32_t p = 31; Read32(state, thread + kThreadPriority, &p);
    return static_cast<std::int32_t>(p);
  }
  std::int32_t SuspendCount(CPUState* state, std::uint32_t thread) const {
    std::uint32_t v = 0; Read32(state, thread + kThreadSuspend, &v);
    return static_cast<std::int32_t>(v);
  }
  std::uint16_t ThreadState(CPUState* state, std::uint32_t thread) const {
    std::uint16_t v = 0; Read16(state, thread + kThreadState, &v); return v;
  }

  struct ThreadReadySnapshot {
    std::uint16_t state = 0;
    std::int32_t suspend = 0;
    std::int32_t priority = 31;
    std::uint32_t active_next = 0;
    std::uint32_t active_prev = 0;
    std::uint32_t stack_base = 0;
    std::uint32_t stack_end = 0;
  };

  bool ReadReadySnapshot(CPUState* state, std::uint32_t thread,
                         ThreadReadySnapshot* out) {
    if (!out || !thread) return false;
    const auto* p = Ptr(state, thread, kThreadSize);
    if (!p) return false;
    ++ready_snapshot_reads_;
    out->state = ReadBE16(p + kThreadState);
    out->suspend = static_cast<std::int32_t>(ReadBE32(p + kThreadSuspend));
    out->priority = static_cast<std::int32_t>(ReadBE32(p + kThreadPriority));
    out->active_next = ReadBE32(p + kThreadActiveNext);
    out->active_prev = ReadBE32(p + kThreadActivePrev);
    out->stack_base = ReadBE32(p + kThreadStackBase);
    out->stack_end = ReadBE32(p + kThreadStackEnd);
    return true;
  }

  bool SchedulerThreadLooksSane(CPUState* state, std::uint32_t thread,
                                const ThreadReadySnapshot& snapshot,
                                std::uint32_t expected_prev, bool active_walk) {
    const bool valid_state = snapshot.state == 0 || snapshot.state == kThreadReady ||
                             snapshot.state == kThreadRunning ||
                             snapshot.state == kThreadWaiting ||
                             snapshot.state == kThreadMoribund;
    const bool valid_priority = snapshot.priority >= 0 && snapshot.priority <= 31;
    const bool valid_links = (!snapshot.active_next || Ptr(state, snapshot.active_next, kThreadSize)) &&
                             (!snapshot.active_prev || Ptr(state, snapshot.active_prev, kThreadSize));
    bool valid_stack = true;
    std::uint32_t stack_magic = 0;
    if (snapshot.stack_base || snapshot.stack_end) {
      valid_stack = snapshot.stack_base > snapshot.stack_end &&
                    Read32(state, snapshot.stack_end, &stack_magic) &&
                    stack_magic == 0xDEADBABEu;
    }
    const bool valid_prev = !active_walk || snapshot.active_prev == expected_prev;
    if (valid_state && valid_priority && valid_links && valid_stack && valid_prev) return true;

    ++thread_sanity_rejects_;
    if (!valid_prev) ++active_link_faults_;
    static unsigned logs = 0;
    if (logs++ < 64u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_OS_THREAD_SANITY_V87=0 thread=%08x state=%u priority=%d prev=%08x expected_prev=%08x next=%08x stack=%08x-%08x magic=%08x reason=%s%s%s%s%s action=skip-invalid\n",
                   thread, unsigned(snapshot.state), snapshot.priority,
                   snapshot.active_prev, expected_prev, snapshot.active_next,
                   snapshot.stack_end, snapshot.stack_base, stack_magic,
                   valid_state ? "" : "state,", valid_priority ? "" : "priority,",
                   valid_links ? "" : "links,", valid_stack ? "" : "stack,",
                   valid_prev ? "" : "prev-link,");
    return false;
  }

  void RemoveReady(std::uint32_t thread) {
    ready_.erase(std::remove(ready_.begin(), ready_.end(), thread), ready_.end());
  }
  bool AddReady(CPUState* state, std::uint32_t thread) {
    if (!thread || !Ptr(state, thread, kThreadSize)) return false;

    // SDK OSWakeupThread changes WAITING -> READY even when suspend > 0.  A
    // suspended READY thread is simply not inserted into the runnable queue
    // until OSResumeThread drops its suspend count to zero.
    Write16(state, thread + kThreadState, kThreadReady);
    Write32(state, thread + kThreadQueue, 0);
    if (SuspendCount(state, thread) > 0) {
      RemoveReady(thread);
      return false;
    }
    if (std::find(ready_.begin(), ready_.end(), thread) == ready_.end())
      ready_.push_back(thread);
    return true;
  }
  // GEKKOAOT_NATIVE_OS_GUEST_READY_IMPORT_V19: import runnable guest threads
  // from the ABI-visible active OSThread queue. This continues to work while
  // __OSCurrentThread is NULL in the native idle path, unlike the old walk
  // which required a current thread as its anchor.
  //
  // v82 keeps the exact reconciliation policy but snapshots the fields used by
  // scheduling with one guest-memory resolution per OSThread. The previous
  // implementation repeatedly called ExternalPointer for state, suspend,
  // priority and active-next, which dominated profiles in scheduler-heavy
  // titles even though all fields live in the same 0x30c-byte OSThread.
  void ReconcileGuestReady(CPUState* state) {
    if (!state) return;
    ++ready_reconcile_calls_;

    const auto remember = [&](std::uint32_t thread, ThreadReadySnapshot* snapshot,
                              std::uint32_t expected_prev, bool active_walk) {
      if (!ReadReadySnapshot(state, thread, snapshot) ||
          !SchedulerThreadLooksSane(state, thread, *snapshot, expected_prev, active_walk))
        return false;
      const auto thread_state = snapshot->state;
      RememberThread(thread);
      if (thread != current_thread_ && thread_state == kThreadReady &&
          snapshot->suspend <= 0 &&
          std::find(ready_.begin(), ready_.end(), thread) == ready_.end())
        ready_.push_back(thread);
      return true;
    };

    std::uint32_t head = 0;
    const bool have_head = Read32(state, kActiveThreadHead, &head);
    if (have_head) {
      active_head_cache_ = head;
      active_head_valid_ = true;
    }
    if (have_head && head) {
      std::array<std::uint32_t, 256> seen{};
      unsigned seen_count = 0;
      auto cursor = head;
      std::uint32_t previous = 0;
      for (unsigned depth = 0; cursor && depth < seen.size(); ++depth) {
        ThreadReadySnapshot snapshot{};
        if (std::find(seen.begin(), seen.begin() + seen_count, cursor) !=
                seen.begin() + seen_count ||
            !remember(cursor, &snapshot, previous, true))
          break;
        seen[seen_count++] = cursor;
        previous = cursor;
        cursor = snapshot.active_next;
      }
    }

    // Keep host-created/previously-seen threads authoritative too. This covers
    // the short window before a title has linked a freshly-created thread into
    // its active list and lets idle wakeups observe state changes immediately.
    for (const auto thread : known_threads_) {
      if (thread == current_thread_) continue;
      ThreadReadySnapshot snapshot{};
      if (ReadReadySnapshot(state, thread, &snapshot) &&
          snapshot.state == kThreadReady && snapshot.suspend <= 0 &&
          std::find(ready_.begin(), ready_.end(), thread) == ready_.end())
        ready_.push_back(thread);
    }
  }

  void AuditOneKnownReady(CPUState* state) {
    if (!state || known_threads_.empty()) return;
    const std::size_t count = known_threads_.size();
    for (std::size_t attempt = 0; attempt < count; ++attempt) {
      const auto index = ready_probe_cursor_++ % count;
      const auto thread = known_threads_[index];
      if (!thread || thread == current_thread_) continue;
      ThreadReadySnapshot snapshot{};
      ++ready_probe_reads_;
      if (!ReadReadySnapshot(state, thread, &snapshot) ||
          snapshot.state != kThreadReady || snapshot.suspend > 0) {
        const auto before = ready_.size();
        RemoveReady(thread);
        if (ready_.size() != before) ++ready_compactions_;
        return;
      }
      if (std::find(ready_.begin(), ready_.end(), thread) == ready_.end())
        ready_.push_back(thread);
      return;
    }
  }

  void MaybeReconcileGuestReady(CPUState* state) {
    if (!state) return;
    constexpr std::uint64_t kFullAuditPeriod = 32u;
    ++ready_peek_calls_;

    // The SDK inserts newly created OSThreads into the active list; a changed
    // head is therefore an immediate structural invalidation. Between those
    // events, NativeOS' HLE paths already mirror state transitions into ready_.
    // Probe one known thread per PeekNext to observe guest-only wrappers, and
    // still perform a full ABI-authoritative walk at a bounded cadence. This
    // preserves scheduling semantics while removing the O(thread-count) walk
    // from every host scheduler poll.
    std::uint32_t head = 0;
    const bool have_head = Read32(state, kActiveThreadHead, &head);
    const bool structural_change = !have_head || !active_head_valid_ ||
                                   head != active_head_cache_;
    const bool periodic = (ready_peek_calls_ % kFullAuditPeriod) == 0u;
    if (structural_change || periodic) {
      ReconcileGuestReady(state);
      return;
    }

    ++ready_reconcile_skips_;
    AuditOneKnownReady(state);

    static bool logged = false;
    if (!logged) {
      logged = true;
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_OS_READY_AUDIT_V86=1 full-period=%llu structural=head-change incremental=round-robin semantics=priority-preserved\n",
                   static_cast<unsigned long long>(kFullAuditPeriod));
    }
  }

  std::uint32_t PeekNext(CPUState* state) {
    MaybeReconcileGuestReady(state);
    std::uint32_t best = 0;
    std::int32_t best_priority = std::numeric_limits<std::int32_t>::max();
    for (std::size_t i = 0; i < ready_.size();) {
      const auto thread = ready_[i];
      ThreadReadySnapshot snapshot{};
      if (!ReadReadySnapshot(state, thread, &snapshot) || snapshot.suspend > 0 ||
          snapshot.state != kThreadReady) {
        ready_.erase(ready_.begin() + static_cast<std::ptrdiff_t>(i));
        ++ready_compactions_;
        continue;
      }
      if (!best || snapshot.priority < best_priority) {
        best = thread;
        best_priority = snapshot.priority;
      }
      ++i;
    }
    return best;
  }
  std::uint32_t PopNext(CPUState* state) {
    const auto next = PeekNext(state); if (next) RemoveReady(next); return next;
  }

  void WriteQueueLinks(CPUState* state, std::uint32_t queue) {
    auto it = wait_queues_.find(queue);
    if (it == wait_queues_.end() || it->second.empty()) {
      Write32(state, queue, 0); Write32(state, queue + 4, 0); return;
    }
    const auto& q = it->second;
    Write32(state, queue, q.front()); Write32(state, queue + 4, q.back());
    for (std::size_t i = 0; i < q.size(); ++i) {
      Write32(state, q[i] + kThreadLinkPrev, i ? q[i - 1] : 0);
      Write32(state, q[i] + kThreadLinkNext, i + 1 < q.size() ? q[i + 1] : 0);
      Write32(state, q[i] + kThreadQueue, queue);
    }
  }
  void EnqueueWait(CPUState* state, std::uint32_t queue, std::uint32_t thread) {
    RemoveReady(thread);
    auto& q = wait_queues_[queue];
    if (std::find(q.begin(), q.end(), thread) == q.end()) {
      // Retail OSSleepThread keeps wait queues sorted by effective priority,
      // preserving FIFO order between equal-priority waiters.
      const auto priority = Priority(state, thread);
      const auto pos = std::find_if(q.begin(), q.end(), [&](std::uint32_t other) {
        return Priority(state, other) > priority;
      });
      q.insert(pos, thread);
    }
    Write16(state, thread + kThreadState, kThreadWaiting);
    WriteQueueLinks(state, queue);
  }
  void RemoveWait(CPUState* state, std::uint32_t queue, std::uint32_t thread) {
    auto it = wait_queues_.find(queue); if (it == wait_queues_.end()) return;
    auto& q = it->second; q.erase(std::remove(q.begin(), q.end(), thread), q.end());
    WriteQueueLinks(state, queue);
    if (q.empty()) wait_queues_.erase(it);
  }

  std::vector<std::uint32_t> SnapshotGuestWaitQueue(CPUState* state,
                                                    std::uint32_t queue) {
    std::vector<std::uint32_t> threads;
    std::uint32_t cursor = 0;
    if (!Read32(state, queue, &cursor)) return threads;
    threads.reserve(8);
    for (unsigned depth = 0; cursor && depth < 256; ++depth) {
      if (!Ptr(state, cursor, kThreadSize) ||
          std::find(threads.begin(), threads.end(), cursor) != threads.end())
        break;
      threads.push_back(cursor);
      std::uint32_t next = 0;
      if (!Read32(state, cursor + kThreadLinkNext, &next)) break;
      cursor = next;
    }
    return threads;
  }

  bool WakeQueue(CPUState* state, std::uint32_t queue) {
    // Guest OSThreadQueue remains ABI-authoritative.  Import it here as well as
    // the host mirror so a queue populated by an unhooked SDK wrapper (or before
    // NativeOS bootstrap) can still be woken correctly.
    auto threads = SnapshotGuestWaitQueue(state, queue);
    if (auto it = wait_queues_.find(queue); it != wait_queues_.end()) {
      for (const auto thread : it->second)
        if (std::find(threads.begin(), threads.end(), thread) == threads.end())
          threads.push_back(thread);
      wait_queues_.erase(it);
    }
    Write32(state, queue, 0);
    Write32(state, queue + 4, 0);

    bool runnable = false;
    for (const auto thread : threads)
      runnable |= WakeThread(state, thread);
    return runnable;
  }

  bool SetCurrent(CPUState* state, std::uint32_t next) {
    if (!next || !Ptr(state, next, kThreadSize)) return false;
    const bool was_idle = scheduler_idle_;

    // Retail SelectThread publishes the chosen OSThread/current OSContext before
    // tail-calling OSLoadContext.  Keep the same ordering so low-memory queries
    // and lazy-FPU ownership observe the new thread while its context is loaded.
    current_thread_ = next;
    scheduler_idle_ = false;
    Write16(state, next + kThreadState, kThreadRunning);
    Write32(state, next + kThreadQueue, 0);
    Write32(state, kCurrentThread, next);
    if (!ApplyCurrentContext(state, next)) return false; // OSContext is first member.
    if (!LoadContext(state, next)) return false;

    if (was_idle) {
      if (auto* idle_context = Ptr(state, kHostIdleContext, kCtxSize))
        std::fill_n(idle_context, kCtxSize, std::uint8_t{0});
    }
    ++switches_;
    return true;
  }

  bool ContextExceptionSaved(CPUState* state, std::uint32_t context) const {
    std::uint16_t context_state = 0;
    return state && context && Read16(state, context + kCtxState, &context_state) &&
           (context_state & 2u) != 0u;
  }

  bool SaveSchedulerContinuation(CPUState* state, std::uint32_t context,
                                 std::uint32_t resume_pc) {
    // GEKKOAOT_NATIVE_OS_EXC_PRESERVE_V73:
    // Retail SelectThread must not call OSSaveContext when the OSThread's
    // OSContext is an exception frame (OS_CONTEXT_STATE_EXC).  The external
    // interrupt trampoline has already saved SRR0/SRR1 and the volatile GPRs
    // into that context.  Saving the scheduler/IRQ-handler continuation over it
    // destroys the interrupted PC and creates an OSLoadContext/IRQ loop.
    if (ContextExceptionSaved(state, context)) {
      static unsigned preserve_logs = 0u;
      if (preserve_logs++ < 64u) {
        std::uint32_t saved_pc = 0;
        Read32(state, context + kCtxSrr0, &saved_pc);
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_OS_EXC_PRESERVE_V73=1 action=preserve ctx=%08x srr0=%08x live_pc=%08x lr=%08x\n",
                     context, saved_pc, state ? state->pc : 0u,
                     state ? state->lr : 0u);
      }
      return true;
    }
    return SaveContext(state, context, resume_pc);
  }

  bool SwitchAway(CPUState* state, std::uint32_t resume_pc, bool requeue_current) {
    if (!Bootstrap(state) || static_cast<std::int32_t>(scheduler_disable_) > 0) return false;
    const auto next = PeekNext(state);
    if (!next || next == current_thread_) return false;
    const auto old = current_thread_;
    if (!SaveSchedulerContinuation(state, old, resume_pc)) return false;
    if (requeue_current) AddReady(state, old);
    PopNext(state);
    return SetCurrent(state, next);
  }

  bool WakeThread(CPUState* state, std::uint32_t thread) {
    if (!thread || !Ptr(state, thread, kThreadSize)) return false;
    Write32(state, thread + kThreadQueue, 0);
    Write32(state, thread + kThreadLinkNext, 0);
    Write32(state, thread + kThreadLinkPrev, 0);
    const bool runnable = AddReady(state, thread);
    if (runnable) reschedule_pending_ = true;
    return runnable;
  }

  bool CurrentThreadOwnsContext(CPUState* state, std::uint32_t* context_out = nullptr) {
    if (!state || !current_thread_) return false;
    std::uint32_t context = 0;
    if (!Read32(state, kCurrentContext, &context)) return false;
    if (context_out) *context_out = context;
    // OSContext is the first field of OSThread, so the SDK compares these
    // pointers directly in SelectThread().
    return context == current_thread_;
  }

  void MaybePreempt(CPUState* state) {
    if (!state || static_cast<std::int32_t>(scheduler_disable_) > 0) return;
    if (scheduler_idle_) {
      reschedule_pending_ = true;
      return;
    }
    if (!Bootstrap(state)) return;

    std::uint32_t context = 0;
    if (!CurrentThreadOwnsContext(state, &context)) {
      // Match retail SelectThread(): a temporary IRQ context cannot be switched
      // away from.  Preserve an already-raised RunQueueHint equivalent; the SDK
      // dispatcher will retry through __OSReschedule after the handler returns.
      static unsigned defer_logs = 0;
      if (defer_logs++ < 32u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_OS_DEFERRED_RESCHEDULE_V59=1 action=defer current=%08x context=%08x ready=%zu\n",
                     current_thread_, context, ready_.size());
      return;
    }

    const auto next = PeekNext(state);
    if (!next) return;
    if (Priority(state, next) < Priority(state, current_thread_)) {
      // The caller already has its SDK return value in gpr3. Save the HLE
      // continuation at LR, exactly as the cooperative fast path does.
      const auto old = current_thread_;
      reschedule_pending_ = false;
      if (!SwitchAway(state, state->lr, true)) {
        reschedule_pending_ = true;
        return;
      }
      static unsigned apply_logs = 0;
      if (apply_logs++ < 32u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_OS_DEFERRED_RESCHEDULE_V59=1 action=apply from=%08x to=%08x ready=%zu\n",
                     old, current_thread_, ready_.size());
      return;
    }
    // The SDK's RunQueueHint remains asserted when SelectThread(0) declines a
    // preemption because the current thread still has equal/higher priority.
    // Preserve the hint until a real switch consumes it.
  }

  bool SelectThreadNative(CPUState* state, bool yield) {
    if (!state) return false;
    if (static_cast<std::int32_t>(scheduler_disable_) > 0) {
      ReturnU32(state, 0);
      return true;
    }
    if (!Bootstrap(state)) return false;

    // SelectThread is occasionally called directly by retail SDK wrappers
    // which remain guest AOT.  Never let those wrappers fall back to the
    // guest RunQueueBits/RunQueue state once NativeOS owns scheduling.
    // Otherwise the guest scheduler and the host mirror diverge and the SDK
    // can spin forever in its idle loop even though ready_ contains work.
    if (scheduler_idle_) {
      const auto next = PopNext(state);
      if (!next) { ReturnU32(state, 0); return true; }
      reschedule_pending_ = false;
      return SetCurrent(state, next);
    }

    std::uint32_t context = 0;
    if (!CurrentThreadOwnsContext(state, &context)) {
      ReturnU32(state, 0);
      return true;
    }

    const auto old = current_thread_;
    const auto old_state = ThreadState(state, old);
    const auto next = PeekNext(state);

    if (old_state == kThreadRunning) {
      if (!yield) {
        // Retail SelectThread(0): only preempt for a strictly higher-priority
        // ready thread (lower numeric priority value).
        if (!next || Priority(state, old) <= Priority(state, next)) {
          ReturnU32(state, 0);
          return true;
        }
      } else {
        // Retail SelectThread(1): requeue the current thread at the tail of
        // its priority, so a same-priority peer may run, but never yield to a
        // lower-priority thread.
        if (!next || Priority(state, old) < Priority(state, next)) {
          ReturnU32(state, 0);
          return true;
        }
      }
    }

    // Save the continuation at SelectThread's LR.  If this OSThread later
    // resumes, it observes SelectThread returning NULL just like OSSaveContext
    // taking its non-zero resume path in the retail SDK.
    state->gpr[3] = 0;
    // Nintendo SDK SelectThread checks OS_CONTEXT_STATE_EXC before the
    // OSSaveContext call.  Exception-saved contexts are already complete and
    // must be preserved verbatim until OSLoadContext consumes/clears EXC.
    if (!SaveSchedulerContinuation(state, old, state->lr)) return false;
    if (old_state == kThreadRunning) AddReady(state, old);

    const auto chosen = PopNext(state);
    if (chosen) {
      const auto from = old;
      reschedule_pending_ = false;
      if (!SetCurrent(state, chosen)) return false;
      static unsigned select_logs = 0u;
      if (select_logs++ < 64u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_OS_SELECTTHREAD_V63=1 yield=%u from=%08x to=%08x old_prio=%d new_prio=%d ready=%zu\n",
                     yield ? 1u : 0u, from, chosen, Priority(state, from),
                     Priority(state, chosen), ready_.size());
      return true;
    }

    reschedule_pending_ = false;
    return EnterIdle(state);
  }

  bool TransferMutex(CPUState* state, std::uint32_t mutex) {
    auto it = wait_queues_.find(mutex); if (it == wait_queues_.end() || it->second.empty()) return false;
    auto& q = it->second;
    auto best_it = q.begin();
    for (auto cur = q.begin(); cur != q.end(); ++cur)
      if (Priority(state, *cur) < Priority(state, *best_it)) best_it = cur;
    const auto thread = *best_it; q.erase(best_it); WriteQueueLinks(state, mutex);
    std::uint32_t count = 1;
    if (const auto cond = pending_cond_.find(thread); cond != pending_cond_.end()) {
      count = std::max<std::uint32_t>(1, cond->second.count); pending_cond_.erase(cond);
    }
    Write32(state, mutex + 8, thread); Write32(state, mutex + 12, count);
    Write32(state, thread + kThreadMutex, 0);
    WakeThread(state, thread);
    return true;
  }

  bool MessageDeliverPendingReceiver(CPUState* state, std::uint32_t mq, std::uint32_t message) {
    // OSSleepThread orders this ABI-visible queue by effective priority.
    // unordered_map iteration would give the message to an arbitrary waiter.
    const auto queue = wait_queues_.find(mq + 8u);
    if (queue == wait_queues_.end()) return false;
    for (const auto thread : queue->second) {
      const auto pending = pending_receives_.find(thread);
      if (pending == pending_receives_.end() || pending->second.mq != mq) continue;
      const auto output = pending->second.output;
      if (output && !Write32(state, output, message)) return false;
      RemoveWait(state, mq + 8, thread);
      pending_receives_.erase(pending);
      WakeThread(state, thread);
      return true;
    }
    return false;
  }

  void MessageCompleteOneSender(CPUState* state, std::uint32_t mq) {
    auto* header = Ptr(state, mq, 32); if (!header) return;
    const std::uint32_t array = ReadBE32(header + 16);
    const std::int32_t count = static_cast<std::int32_t>(ReadBE32(header + 20));
    std::int32_t first = static_cast<std::int32_t>(ReadBE32(header + 24));
    std::int32_t used = static_cast<std::int32_t>(ReadBE32(header + 28));
    if (count <= 0 || used >= count) return;
    // A receive frees one slot. Select the first blocking sender in the same
    // priority-ordered queue used by guest OSSleepThread.
    auto chosen = pending_sends_.end();
    if (const auto queue = wait_queues_.find(mq); queue != wait_queues_.end()) {
      for (const auto waiter : queue->second) {
        const auto pending = pending_sends_.find(waiter);
        if (pending != pending_sends_.end() && pending->second.mq == mq) {
          chosen = pending;
          break;
        }
      }
    }
    if (chosen == pending_sends_.end()) return;
    const auto thread = chosen->first;
    const auto pending = chosen->second;
    if (MessageDeliverPendingReceiver(state, mq, pending.message)) {
      RemoveWait(state, mq, thread); pending_sends_.erase(chosen); WakeThread(state, thread); return;
    }
    if (pending.jam) {
      first = (first + count - 1) % count;
      Write32(state, array + static_cast<std::uint32_t>(first) * 4, pending.message);
    } else {
      const auto index = (first + used) % count;
      Write32(state, array + static_cast<std::uint32_t>(index) * 4, pending.message);
    }
    ++used; WriteBE32(header + 24, static_cast<std::uint32_t>(first)); WriteBE32(header + 28, static_cast<std::uint32_t>(used));
    RemoveWait(state, mq, thread); pending_sends_.erase(chosen); WakeThread(state, thread);
  }

  bool HandleMessageSend(CPUState* state, bool jam) {
    const std::uint32_t mq = state->gpr[3], message = state->gpr[4];
    const bool block = state->gpr[5] != 0;
    auto* header = Ptr(state, mq, 32); if (!header) return false;
    if (MessageDeliverPendingReceiver(state, mq, message)) { ReturnU32(state, 1); MaybePreempt(state); return true; }
    const std::uint32_t array = ReadBE32(header + 16);
    const std::int32_t count = static_cast<std::int32_t>(ReadBE32(header + 20));
    std::int32_t first = static_cast<std::int32_t>(ReadBE32(header + 24));
    std::int32_t used = static_cast<std::int32_t>(ReadBE32(header + 28));
    if (count <= 0 || !Ptr(state, array, static_cast<std::uint32_t>(count) * 4)) return false;
    if (used < count) {
      if (jam) { first = (first + count - 1) % count; Write32(state, array + static_cast<std::uint32_t>(first) * 4, message); }
      else { const auto index = (first + used) % count; Write32(state, array + static_cast<std::uint32_t>(index) * 4, message); }
      ++used; WriteBE32(header + 24, static_cast<std::uint32_t>(first)); WriteBE32(header + 28, static_cast<std::uint32_t>(used));
      ReturnU32(state, 1); return true;
    }
    if (!block) { ReturnU32(state, 0); return true; }
    if (!Bootstrap(state)) return false;
    const auto thread = current_thread_; pending_sends_[thread] = {mq, message, jam}; state->gpr[3] = 1;
    if (!SaveContext(state, thread, state->lr)) return false;
    EnqueueWait(state, mq, thread);
    return SwitchOrIdle(state);
  }

  bool HandleMessageReceive(CPUState* state) {
    const std::uint32_t mq = state->gpr[3], output = state->gpr[4];
    const bool block = state->gpr[5] != 0;
    auto* header = Ptr(state, mq, 32); if (!header) return false;
    const std::uint32_t array = ReadBE32(header + 16);
    const std::int32_t count = static_cast<std::int32_t>(ReadBE32(header + 20));
    std::int32_t first = static_cast<std::int32_t>(ReadBE32(header + 24));
    std::int32_t used = static_cast<std::int32_t>(ReadBE32(header + 28));
    if (count <= 0 || !Ptr(state, array, static_cast<std::uint32_t>(count) * 4)) return false;
    if (used > 0) {
      std::uint32_t message = 0; if (!Read32(state, array + static_cast<std::uint32_t>(first) * 4, &message)) return false;
      if (output && !Write32(state, output, message)) return false;
      first = (first + 1) % count; --used; WriteBE32(header + 24, static_cast<std::uint32_t>(first)); WriteBE32(header + 28, static_cast<std::uint32_t>(used));
      MessageCompleteOneSender(state, mq); ReturnU32(state, 1); MaybePreempt(state); return true;
    }
    if (!block) { ReturnU32(state, 0); return true; }
    if (!Bootstrap(state)) return false;
    const auto thread = current_thread_; pending_receives_[thread] = {mq, output}; state->gpr[3] = 1;
    if (!SaveContext(state, thread, state->lr)) return false;
    EnqueueWait(state, mq + 8, thread);
    return SwitchOrIdle(state);
  }

  static std::uint64_t Arg64(const CPUState* state, unsigned hi) {
    return (static_cast<std::uint64_t>(state->gpr[hi]) << 32) | state->gpr[hi + 1];
  }

  bool SetAlarmCommon(CPUState* state, bool absolute, bool periodic) {
    const std::uint32_t address = state->gpr[3];
    if (!Ptr(state, address, 40)) return false;
    Alarm alarm{}; alarm.address = address; alarm.active = true;
    if (periodic) {
      alarm.start = Arg64(state, 4); alarm.period = Arg64(state, 6); alarm.handler = state->gpr[8];
      alarm.fire = alarm.start;
    } else {
      const auto when = Arg64(state, 4); alarm.handler = state->gpr[6];
      alarm.fire = absolute ? when : state->timebase + when;
    }
    if (!alarm.handler) return false;
    alarms_[address] = alarm;
    Write32(state, address, alarm.handler); Write32(state, address + 4, 0);
    Write64(state, address + 8, alarm.fire); Write32(state, address + 16, 0); Write32(state, address + 20, 0);
    Write64(state, address + 24, alarm.period); Write64(state, address + 32, alarm.start);
    ReturnVoid(state); return true;
  }

public:
  static Service& Get() { static Service service; return service; }

  void Reset() {
    known_threads_.clear(); ready_.clear(); wait_queues_.clear(); pending_sends_.clear();
    pending_receives_.clear(); pending_cond_.clear(); entrypoints_.fill(0); alarms_.clear();
    fp_shadows_.clear();
    if (known_threads_.capacity() < 32) known_threads_.reserve(32);
    if (fp_shadows_.capacity() < 32) fp_shadows_.reserve(32);
    entrypoint_addresses_.clear(); entrypoint_pages_.fill(0);
    alarm_return_ = {}; current_thread_ = 0; scheduler_disable_ = 0; switches_ = 0;
    hle_calls_ = 0; alarm_callbacks_ = 0; idle_entries_ = 0;
    ready_reconcile_calls_ = 0; ready_reconcile_skips_ = 0; ready_probe_reads_ = 0;
    ready_snapshot_reads_ = 0; ready_compactions_ = 0; ready_peek_calls_ = 0;
    thread_sanity_rejects_ = 0; active_link_faults_ = 0;
    active_head_cache_ = 0; ready_probe_cursor_ = 0; active_head_valid_ = false;
    scheduler_idle_ = false; reschedule_pending_ = false; initialized_ = false;
  }

  bool IsIdle() const { return scheduler_idle_; }
  static constexpr std::uint32_t IdlePc() { return kHostIdlePc; }
  bool ResumeIdleIfReady(CPUState* state) {
    if (!scheduler_idle_ || !state) return false;
    const auto next = PopNext(state);
    if (!next) return false;
    reschedule_pending_ = false;
    return SetCurrent(state, next);
  }

  void Register(Kind kind, std::uint32_t address) {
    if (!address || kind == Kind::Count) return;
    entrypoints_[static_cast<std::size_t>(kind)] = address;
    const auto it = std::lower_bound(entrypoint_addresses_.begin(), entrypoint_addresses_.end(), address);
    if (it == entrypoint_addresses_.end() || *it != address)
      entrypoint_addresses_.insert(it, address);
    const auto page = address >> kEntrypointPageShift;
    entrypoint_pages_[page >> 6] |= (std::uint64_t{1} << (page & 63));
  }
  std::uint32_t Entrypoint(Kind kind) const {
    if (kind == Kind::Count) return 0;
    return entrypoints_[static_cast<std::size_t>(kind)];
  }
  bool HandlesEntrypoint(std::uint32_t address) const {
    const auto page = address >> kEntrypointPageShift;
    if ((entrypoint_pages_[page >> 6] & (std::uint64_t{1} << (page & 63))) == 0)
      return false;
    return std::binary_search(entrypoint_addresses_.begin(), entrypoint_addresses_.end(), address);
  }
  bool HandlesEntrypointRange(std::uint32_t start, std::uint32_t end) const {
    if (start >= end || entrypoint_addresses_.empty()) return false;
    const auto first_page = start >> kEntrypointPageShift;
    const auto last_page = (end - 1u) >> kEntrypointPageShift;
    const auto first_word = first_page >> 6;
    const auto last_word = last_page >> 6;
    const auto first_mask = ~std::uint64_t{0} << (first_page & 63);
    const auto last_mask = ~std::uint64_t{0} >> (63 - (last_page & 63));
    bool maybe = false;
    if (first_word == last_word)
      maybe = (entrypoint_pages_[first_word] & first_mask & last_mask) != 0;
    else {
      maybe = (entrypoint_pages_[first_word] & first_mask) != 0;
      for (std::size_t word = first_word + 1; !maybe && word < last_word; ++word)
        maybe = entrypoint_pages_[word] != 0;
      if (!maybe) maybe = (entrypoint_pages_[last_word] & last_mask) != 0;
    }
    if (!maybe) return false;
    const auto it = std::lower_bound(entrypoint_addresses_.begin(), entrypoint_addresses_.end(), start);
    return it != entrypoint_addresses_.end() && *it < end;
  }

  // GEKKOAOT_NATIVE_OS_INLINE_FAST_V29
  // Tiny high-frequency SDK helpers never need scheduler bootstrap or the
  // full Dispatch switch. Keep them in the header so -O3 can inline the whole
  // operation into ModManager::DirectAOTCall.
  static constexpr bool IsInlineFastKind(Kind kind) {
    switch (kind) {
    case Kind::GetCurrentThread:
    case Kind::IsThreadTerminated:
    case Kind::DisableScheduler:
    case Kind::EnableScheduler:
    case Kind::GetThreadPriority:
    case Kind::GetCurrentContext:
    case Kind::SaveContext:
    case Kind::LoadContext:
    case Kind::ClearContext:
    case Kind::InitContext:
    case Kind::DisableInterrupts:
    case Kind::EnableInterrupts:
    case Kind::RestoreInterrupts:
      return true;
    default:
      return false;
    }
  }

  bool DispatchInlineFast(Kind kind, CPUState* state) {
    if (!state) return false;
    ++hle_calls_;
    switch (kind) {
    case Kind::GetCurrentThread: {
      std::uint32_t current = current_thread_;
      if (!current) Read32(state, kCurrentThread, &current);
      ReturnU32(state, current);
      return true;
    }
    case Kind::IsThreadTerminated: {
      const auto thread = state->gpr[3];
      if (!Ptr(state, thread, kThreadSize)) return false;
      const auto thread_state = ThreadState(state, thread);
      ReturnU32(state, thread_state == 0 || thread_state == kThreadMoribund);
      return true;
    }
    case Kind::DisableScheduler: {
      const auto old = scheduler_disable_;
      ++scheduler_disable_;
      ReturnU32(state, static_cast<std::uint32_t>(old));
      return true;
    }
    case Kind::EnableScheduler: {
      const auto old = scheduler_disable_;
      --scheduler_disable_;
      ReturnU32(state, old);
      return true;
    }
    case Kind::GetThreadPriority: {
      const auto thread = state->gpr[3];
      if (!Ptr(state, thread, kThreadSize)) return false;
      // Retail SDK OSGetThreadPriority returns OSThread::base (+0x2D4), not
      // the inherited/effective scheduling priority at +0x2D0.
      std::uint32_t base_priority = 31u;
      if (!Read32(state, thread + kThreadBase, &base_priority)) return false;
      ReturnU32(state, base_priority);
      return true;
    }
    case Kind::GetCurrentContext: {
      std::uint32_t context = 0;
      if (!Read32(state, kCurrentContext, &context)) return false;
      ReturnU32(state, context);
      return true;
    }
    case Kind::SaveContext: {
      const auto context = state->gpr[3];
      if (!SaveContext(state, context, state->lr, true)) return false;
      ReturnU32(state, 0);
      return true;
    }
    case Kind::LoadContext:
      return LoadContext(state, state->gpr[3]);
    case Kind::ClearContext: {
      const auto context = state->gpr[3];
      if (!Ptr(state, context, kCtxSize)) return false;
      Write16(state, context + kCtxMode, 0);
      Write16(state, context + kCtxState, 0);
      InvalidateFpShadow(context);
      std::uint32_t owner = 0;
      if (Read32(state, kFpuContext, &owner) && owner == context)
        Write32(state, kFpuContext, 0);
      ReturnVoid(state);
      return true;
    }
    case Kind::InitContext: {
      if (!InitializeContext(state, state->gpr[3], state->gpr[4], state->gpr[5]))
        return false;
      ReturnVoid(state);
      return true;
    }
    case Kind::DisableInterrupts: {
      const bool old = (state->msr & kMsrEE) != 0;
      state->msr &= ~kMsrEE;
      ReturnU32(state, old ? 1u : 0u);
      return true;
    }
    case Kind::EnableInterrupts: {
      const bool old = (state->msr & kMsrEE) != 0;
      state->msr |= kMsrEE;
      ReturnU32(state, old ? 1u : 0u);
      return true;
    }
    case Kind::RestoreInterrupts: {
      const bool old = (state->msr & kMsrEE) != 0;
      if (state->gpr[3]) state->msr |= kMsrEE;
      else state->msr &= ~kMsrEE;
      ReturnU32(state, old ? 1u : 0u);
      return true;
    }
    default:
      return false;
    }
  }

  bool NeedsGlobalPoll() const {
    if (alarm_return_.valid) return true;
    for (const auto& [_, alarm] : alarms_) if (alarm.active) return true;
    return false;
  }
  bool HandlesAddress(std::uint32_t address) const {
    return alarm_return_.valid && alarm_return_.address == address;
  }

  // Called before normal address dispatch.  A due alarm is invoked as a guest
  // callback.  Because the hook runs at an AOT function-entry boundary, the
  // complete CPUState can be restored when the callback returns to that exact
  // address without re-executing a partially-completed instruction sequence.
  bool Poll(CPUState* state, std::uint32_t address) {
    if (!state) return false;
    // GEKKOAOT_NATIVE_OS_POLL_FAST_V20: Dispatch() reaches Poll at every HLE
    // boundary. Most games have no live OS alarm; reject that overwhelmingly
    // common case before touching the alarm map.
    if (!alarm_return_.valid && alarms_.empty()) return false;
    if (alarm_return_.valid && alarm_return_.address == address &&
        alarm_return_.stack_pointer == state->gpr[1]) {
      *state = alarm_return_.state; alarm_return_ = {}; return false;
    }
    if (alarm_return_.valid || (state->msr & kMsrEE) == 0) return false;
    Alarm* selected = nullptr;
    for (auto& [_, alarm] : alarms_) {
      if (!alarm.active || alarm.fire > state->timebase) continue;
      if (!selected || alarm.fire < selected->fire) selected = &alarm;
    }
    if (!selected) return false;
    alarm_return_.state = *state; alarm_return_.address = address;
    alarm_return_.stack_pointer = state->gpr[1]; alarm_return_.valid = true;
    const auto handler = selected->handler; const auto alarm_address = selected->address;
    if (selected->period) {
      do selected->fire += selected->period; while (selected->fire <= state->timebase);
      Write64(state, selected->address + 8, selected->fire);
    } else {
      selected->active = false;
    }
    state->gpr[3] = alarm_address;
    std::uint32_t context = 0; Read32(state, kCurrentContext, &context); state->gpr[4] = context;
    state->lr = address; state->pc = handler; ++alarm_callbacks_;
    return true;
  }

  bool Dispatch(Kind kind, CPUState* state) {
    if (!state) return false;
    ++hle_calls_;
    // GEKKOAOT_NATIVE_OS_HOTPATH_V17: environment lookup is process policy,
    // not per-HLE-call work.  perf previously showed getenv dominating Dispatch.
    static const bool trace_enabled = [] {
      const char* trace = std::getenv("GEKKOAOT_NATIVE_OS_TRACE");
      return trace && *trace == '1';
    }();
    if (trace_enabled && (hle_calls_ <= 64 || (hle_calls_ & (hle_calls_ - 1)) == 0))
      std::fprintf(stderr, "GEKKOAOT_NATIVE_OS_CALL n=%llu name=%s pc=%08x lr=%08x r3=%08x\n",
                   static_cast<unsigned long long>(hle_calls_), Name(kind),
                   state->pc, state->lr, state->gpr[3]);
    switch (kind) {
    case Kind::InitThreadQueue: {
      const auto q = state->gpr[3]; if (!Ptr(state, q, 8)) return false;
      wait_queues_.erase(q); Write32(state, q, 0); Write32(state, q + 4, 0); ReturnVoid(state); return true;
    }
    case Kind::GetCurrentThread: {
      std::uint32_t current = current_thread_; if (!current) Read32(state, kCurrentThread, &current);
      ReturnU32(state, current); return true;
    }
    case Kind::IsThreadTerminated: {
      const auto thread = state->gpr[3]; if (!Ptr(state, thread, kThreadSize)) return false;
      const auto s = ThreadState(state, thread); ReturnU32(state, s == 0 || s == kThreadMoribund); return true;
    }
    case Kind::DisableScheduler: { const auto old = scheduler_disable_; ++scheduler_disable_; ReturnU32(state, static_cast<std::uint32_t>(old)); return true; }
    // SDK enable/disable only update the counter and return its old value.
    // Rescheduling belongs to the caller; enabling must not switch contexts.
    case Kind::EnableScheduler: { const auto old = scheduler_disable_; --scheduler_disable_; ReturnU32(state, old); return true; }
    case Kind::SelectThread: return SelectThreadNative(state, state->gpr[3] != 0);
    case Kind::Reschedule: {
      // Retail __OSReschedule checks RunQueueHint and, when set, executes the
      // complete SelectThread(0) path.  reschedule_pending_ is NativeOS' host
      // mirror of that hint; do not collapse this into MaybePreempt because the
      // full path also handles non-RUNNING current threads and idle transitions.
      if (!reschedule_pending_) { ReturnVoid(state); return true; }
      return SelectThreadNative(state, false);
    }
    case Kind::YieldThread: return SelectThreadNative(state, true);
    case Kind::CreateThread: {
      const auto thread = state->gpr[3], func = state->gpr[4], param = state->gpr[5], stack = state->gpr[6];
      const auto stack_size = state->gpr[7]; const auto priority = static_cast<std::int32_t>(state->gpr[8]); const auto attr = static_cast<std::uint16_t>(state->gpr[9]);
      if (priority < 0 || priority > 31 || stack_size < 16 || !Ptr(state, thread, kThreadSize) || !func) { ReturnU32(state, 0); return true; }
      const auto exit_thread = Entrypoint(Kind::ExitThread);
      const std::uint32_t aligned = stack & ~7u;
      if (!exit_thread || stack < stack_size || aligned < 8 ||
          !Ptr(state, aligned - 8, 8) || !Ptr(state, stack - stack_size, 4)) return false;
      const auto sp = aligned - 8;
      if (!InitializeContext(state, thread, func, sp)) return false;
      Write32(state, sp, 0); Write32(state, sp + 4, 0);
      Write32(state, thread + 12, param);
      Write32(state, thread + kCtxLr, exit_thread);
      Write16(state, thread + kThreadState, kThreadReady); Write16(state, thread + kThreadAttr, attr & kThreadDetach);
      Write32(state, thread + kThreadSuspend, 1); Write32(state, thread + kThreadPriority, static_cast<std::uint32_t>(priority));
      Write32(state, thread + kThreadBase, static_cast<std::uint32_t>(priority)); Write32(state, thread + kThreadValue, 0xffffffffu);
      Write32(state, thread + kThreadQueue, 0); Write32(state, thread + kThreadJoinQueue, 0); Write32(state, thread + kThreadJoinQueue + 4, 0);
      Write32(state, thread + kThreadMutex, 0); Write32(state, thread + kThreadMutexQueue, 0); Write32(state, thread + kThreadMutexQueue + 4, 0);
      Write32(state, thread + kThreadActiveNext, 0); Write32(state, thread + kThreadActivePrev, 0);
      Write32(state, thread + kThreadStackBase, stack); Write32(state, thread + kThreadStackEnd, stack - stack_size);
      Write32(state, stack - stack_size, 0xDEADBABEu); RememberThread(thread); ReturnU32(state, 1); return true;
    }
    case Kind::ExitThread: {
      if (!Bootstrap(state)) return false;
      const auto thread = current_thread_;
      const auto value = state->gpr[3]; Write32(state, thread + kThreadValue, value); std::uint16_t attr = 0; Read16(state, thread + kThreadAttr, &attr);
      Write16(state, thread + kThreadState, (attr & kThreadDetach) ? 0 : kThreadMoribund);
      auto join = wait_queues_.find(thread + kThreadJoinQueue);
      if (join != wait_queues_.end()) {
        const auto waiters = join->second;
        for (auto waiter : waiters) {
          if (const auto pending = pending_receives_.find(waiter); pending != pending_receives_.end()) {
            if (pending->second.output) Write32(state, pending->second.output, value);
            pending_receives_.erase(pending);
          }
          // Saved OSJoinThread continuation must observe TRUE in r3.
          Write32(state, waiter + 12, 1);
          WakeThread(state, waiter);
        }
        wait_queues_.erase(join); Write32(state, thread + kThreadJoinQueue, 0); Write32(state, thread + kThreadJoinQueue + 4, 0);
      }
      return SwitchOrIdle(state);
    }
    case Kind::CancelThread: {
      if (!Bootstrap(state)) return false;
      const auto thread = state->gpr[3]; if (!Ptr(state, thread, kThreadSize)) return false;
      if (thread == current_thread_) { state->gpr[3] = 0; return Dispatch(Kind::ExitThread, state); }
      RemoveReady(thread); std::uint32_t queue = 0; Read32(state, thread + kThreadQueue, &queue); if (queue) RemoveWait(state, queue, thread);
      std::uint16_t attr = 0; Read16(state, thread + kThreadAttr, &attr); Write16(state, thread + kThreadState, (attr & kThreadDetach) ? 0 : kThreadMoribund); ReturnVoid(state); return true;
    }
    case Kind::JoinThread: {
      const auto target = state->gpr[3], value_out = state->gpr[4]; if (!Ptr(state, target, kThreadSize)) return false;
      const auto s = ThreadState(state, target); std::uint16_t attr = 0; Read16(state, target + kThreadAttr, &attr);
      if (attr & kThreadDetach) { ReturnU32(state, 0); return true; }
      if (s == kThreadMoribund || s == 0) { std::uint32_t value = 0; Read32(state, target + kThreadValue, &value); if (value_out) Write32(state, value_out, value); Write16(state, target + kThreadState, 0); ReturnU32(state, 1); return true; }
      if (!Bootstrap(state)) return false;
      const auto thread = current_thread_; state->gpr[3] = 1; if (!SaveContext(state, thread, state->lr)) return false;
      // Remember the requested value pointer in an otherwise host-only map by reusing PendingReceive.
      pending_receives_[thread] = {target + kThreadJoinQueue, value_out}; EnqueueWait(state, target + kThreadJoinQueue, thread);
      return SwitchOrIdle(state);
    }
    case Kind::DetachThread: {
      const auto thread = state->gpr[3]; std::uint16_t attr = 0; if (!Read16(state, thread + kThreadAttr, &attr)) return false;
      Write16(state, thread + kThreadAttr, attr | kThreadDetach); if (ThreadState(state, thread) == kThreadMoribund) Write16(state, thread + kThreadState, 0); ReturnVoid(state); return true;
    }
    case Kind::ResumeThread: {
      const auto thread = state->gpr[3]; std::uint32_t raw = 0; if (!Read32(state, thread + kThreadSuspend, &raw)) return false;
      const auto old = static_cast<std::int32_t>(raw); const auto now = std::max<std::int32_t>(0, old - 1); Write32(state, thread + kThreadSuspend, static_cast<std::uint32_t>(now));
      if (old > 0 && now == 0 && ThreadState(state, thread) == kThreadReady) AddReady(state, thread);
      ReturnU32(state, static_cast<std::uint32_t>(old));
      MaybePreempt(state);
      return true;
    }
    case Kind::SuspendThread: {
      if (!Bootstrap(state)) return false;
      const auto thread = state->gpr[3]; std::uint32_t raw = 0; if (!Read32(state, thread + kThreadSuspend, &raw)) return false;
      const auto old = static_cast<std::int32_t>(raw); Write32(state, thread + kThreadSuspend, static_cast<std::uint32_t>(old + 1)); RemoveReady(thread);
      state->gpr[3] = static_cast<std::uint32_t>(old);
      if (thread == current_thread_ && old == 0) {
        if (!SaveContext(state, thread, state->lr)) return false;
        Write16(state, thread + kThreadState, kThreadReady);
        return SwitchOrIdle(state);
      }
      state->pc = state->lr;
      return true;
    }
    case Kind::SleepThread: {
      const auto queue = state->gpr[3]; if (!Bootstrap(state) || !Ptr(state, queue, 8)) return false;
      const auto thread = current_thread_;
      if (!SaveContext(state, thread, state->lr)) return false;
      EnqueueWait(state, queue, thread);
      return SwitchOrIdle(state);
    }
    case Kind::WakeupThread: {
      const auto queue = state->gpr[3];
      if (!Ptr(state, queue, 8)) return false;
      WakeQueue(state, queue);
      // SDK OSWakeupThread attempts SelectThread(0) whenever waking a runnable
      // thread raised RunQueueHint.  SelectThread itself suppresses switching
      // while the scheduler is disabled (e.g. inside __OSDispatchInterrupt).
      if (reschedule_pending_) return SelectThreadNative(state, false);
      ReturnVoid(state);
      return true;
    }
    case Kind::SetThreadPriority: {
      const auto thread = state->gpr[3];
      const auto priority = static_cast<std::int32_t>(state->gpr[4]);
      if (!Ptr(state, thread, kThreadSize) || priority < 0 || priority > 31) {
        ReturnU32(state, 0);
        return true;
      }

      const auto old_priority = Priority(state, thread);
      Write32(state, thread + kThreadBase, static_cast<std::uint32_t>(priority));
      Write32(state, thread + kThreadPriority, static_cast<std::uint32_t>(priority));

      // NativeOS keeps ready queues host-side, so changing the ABI-visible
      // priority is enough for PeekNext(). Waiting queues, however, are SDK
      // priority ordered; keep their host mirror in the same order too.
      std::uint32_t queue = 0;
      if (Read32(state, thread + kThreadQueue, &queue) && queue) {
        auto it = wait_queues_.find(queue);
        if (it != wait_queues_.end()) {
          auto& q = it->second;
          std::stable_sort(q.begin(), q.end(), [&](std::uint32_t a, std::uint32_t b) {
            return Priority(state, a) < Priority(state, b);
          });
          WriteQueueLinks(state, queue);
        }
      }

      static unsigned priority_logs = 0u;
      if (priority_logs++ < 32u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_OS_SET_PRIORITY_V60=1 thread=%08x old=%d new=%d state=%u current=%08x\n",
                     thread, old_priority, priority,
                     static_cast<unsigned>(ThreadState(state, thread)), current_thread_);
      ReturnU32(state, 1);
      MaybePreempt(state);
      return true;
    }
    case Kind::GetThreadPriority: {
      const auto t = state->gpr[3];
      if (!Ptr(state, t, kThreadSize)) return false;
      // Retail SDK OSGetThreadPriority returns OSThread::base (+0x2D4), not
      // the inherited/effective scheduling priority at +0x2D0.
      std::uint32_t base_priority = 31u;
      if (!Read32(state, t + kThreadBase, &base_priority)) return false;
      ReturnU32(state, base_priority);
      return true;
    }
    case Kind::SaveContext: {
      const auto ctx = state->gpr[3]; if (!SaveContext(state, ctx, state->lr, true)) return false; ReturnU32(state, 0); return true;
    }
    case Kind::LoadContext: { return LoadContext(state, state->gpr[3]); }
    case Kind::ClearContext: {
      const auto ctx = state->gpr[3];
      if (!Ptr(state, ctx, kCtxSize)) return false;
      // SDK clears validity/ownership, preserving the saved register payload.
      Write16(state, ctx + kCtxMode, 0);
      Write16(state, ctx + kCtxState, 0);
      InvalidateFpShadow(ctx);
      std::uint32_t owner = 0;
      if (Read32(state, kFpuContext, &owner) && owner == ctx)
        Write32(state, kFpuContext, 0);
      ReturnVoid(state); return true;
    }
    case Kind::InitContext: {
      if (!InitializeContext(state, state->gpr[3], state->gpr[4], state->gpr[5])) return false;
      ReturnVoid(state); return true;
    }
    case Kind::GetCurrentContext: { std::uint32_t ctx = 0; Read32(state, kCurrentContext, &ctx); ReturnU32(state, ctx); return true; }
    case Kind::SetCurrentContext: {
      const auto ctx = state->gpr[3];
      if (!ApplyCurrentContext(state, ctx)) return false;
      ReturnVoid(state);
      // __VIRetraceHandler and other SDK IRQ handlers call OSWakeupThread while
      // an exception context is current, then restore the interrupted context
      // with OSSetCurrentContext near the end of the handler. This is the first
      // safe point at which the deferred SDK reschedule may actually switch.
      if (ctx == current_thread_ && reschedule_pending_) MaybePreempt(state);
      return true;
    }
    case Kind::InitMutex: {
      const auto m = state->gpr[3]; auto* p = Ptr(state, m, 24); if (!p) return false; std::fill_n(p, 24, 0); wait_queues_.erase(m); ReturnVoid(state); return true;
    }
    case Kind::LockMutex: {
      const auto m = state->gpr[3]; if (!Ptr(state, m, 24) || !Bootstrap(state)) return false; std::uint32_t owner = 0, count = 0; Read32(state, m + 8, &owner); Read32(state, m + 12, &count);
      if (!owner || owner == current_thread_) { Write32(state, m + 8, current_thread_); Write32(state, m + 12, count + 1); ReturnVoid(state); return true; }
      const auto thread = current_thread_;
      if (!SaveContext(state, thread, state->lr)) return false;
      Write32(state, thread + kThreadMutex, m);
      EnqueueWait(state, m, thread);
      return SwitchOrIdle(state);
    }
    case Kind::UnlockMutex: {
      const auto m = state->gpr[3]; if (!Bootstrap(state)) return false; std::uint32_t owner = 0, count = 0; if (!Read32(state, m + 8, &owner) || !Read32(state, m + 12, &count)) return false;
      if (owner == current_thread_ && count) { if (--count) Write32(state, m + 12, count); else { Write32(state, m + 8, 0); Write32(state, m + 12, 0); TransferMutex(state, m); } }
      ReturnVoid(state); MaybePreempt(state); return true;
    }
    case Kind::TryLockMutex: {
      const auto m = state->gpr[3]; if (!Bootstrap(state)) return false; std::uint32_t owner = 0, count = 0; if (!Read32(state, m + 8, &owner) || !Read32(state, m + 12, &count)) return false;
      if (!owner || owner == current_thread_) { Write32(state, m + 8, current_thread_); Write32(state, m + 12, count + 1); ReturnU32(state, 1); } else ReturnU32(state, 0); return true;
    }
    case Kind::InitCond: { const auto c = state->gpr[3]; if (!Ptr(state, c, 8)) return false; Write32(state, c, 0); Write32(state, c + 4, 0); wait_queues_.erase(c); ReturnVoid(state); return true; }
    case Kind::WaitCond: {
      const auto cond = state->gpr[3], mutex = state->gpr[4]; if (!Bootstrap(state)) return false;
      std::uint32_t owner = 0, count = 0; if (!Read32(state, mutex + 8, &owner) || owner != current_thread_ || !Read32(state, mutex + 12, &count)) return false;
      const auto thread = current_thread_; pending_cond_[thread] = {mutex, std::max<std::uint32_t>(1, count)}; Write32(state, mutex + 8, 0); Write32(state, mutex + 12, 0); TransferMutex(state, mutex);
      if (!SaveContext(state, thread, state->lr)) return false;
      EnqueueWait(state, cond, thread);
      return SwitchOrIdle(state);
    }
    case Kind::SignalCond: {
      const auto cond = state->gpr[3]; auto it = wait_queues_.find(cond); if (it != wait_queues_.end()) {
        const auto threads = it->second; wait_queues_.erase(it); Write32(state, cond, 0); Write32(state, cond + 4, 0);
        for (auto thread : threads) { const auto p = pending_cond_.find(thread); if (p == pending_cond_.end()) { WakeThread(state, thread); continue; } const auto mutex = p->second.mutex; std::uint32_t owner = 0; Read32(state, mutex + 8, &owner); if (!owner) { Write32(state, mutex + 8, thread); Write32(state, mutex + 12, p->second.count); pending_cond_.erase(p); WakeThread(state, thread); } else EnqueueWait(state, mutex, thread); }
      }
      ReturnVoid(state); MaybePreempt(state); return true;
    }
    case Kind::InitMessageQueue: {
      const auto mq = state->gpr[3], array = state->gpr[4]; const auto count = state->gpr[5]; auto* p = Ptr(state, mq, 32); if (!p || !count || count > UINT32_MAX / 4 || !Ptr(state, array, count * 4)) return false;
      std::fill_n(p, 32, 0); WriteBE32(p + 16, array); WriteBE32(p + 20, count); ReturnVoid(state); return true;
    }
    case Kind::SendMessage: return HandleMessageSend(state, false);
    case Kind::JamMessage: return HandleMessageSend(state, true);
    case Kind::ReceiveMessage: return HandleMessageReceive(state);
    case Kind::InitAlarm: { alarms_.clear(); ReturnVoid(state); return true; }
    case Kind::CreateAlarm: { const auto a = state->gpr[3]; auto* p = Ptr(state, a, 40); if (!p) return false; std::fill_n(p, 40, 0); alarms_.erase(a); ReturnVoid(state); return true; }
    case Kind::SetAlarm: return SetAlarmCommon(state, false, false);
    case Kind::SetAbsAlarm: return SetAlarmCommon(state, true, false);
    case Kind::SetPeriodicAlarm: return SetAlarmCommon(state, true, true);
    case Kind::CancelAlarm: { const auto a = state->gpr[3]; if (auto it = alarms_.find(a); it != alarms_.end()) it->second.active = false; ReturnVoid(state); return true; }
    case Kind::CheckAlarmQueue: { bool active = false; for (const auto& [_, a] : alarms_) active |= a.active; ReturnU32(state, active ? 1u : 0u); return true; }
    case Kind::DisableInterrupts: { const bool old = (state->msr & kMsrEE) != 0; state->msr &= ~kMsrEE; ReturnU32(state, old ? 1u : 0u); return true; }
    case Kind::EnableInterrupts: { const bool old = (state->msr & kMsrEE) != 0; state->msr |= kMsrEE; ReturnU32(state, old ? 1u : 0u); return true; }
    case Kind::RestoreInterrupts: { const bool old = (state->msr & kMsrEE) != 0; if (state->gpr[3]) state->msr |= kMsrEE; else state->msr &= ~kMsrEE; ReturnU32(state, old ? 1u : 0u); return true; }
    case Kind::Count:
      break;
    }
    return false;
  }

  void DumpStats() const {
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_OS_STATS calls=%llu switches=%llu idle_entries=%llu idle=%u threads=%zu ready=%zu waits=%zu alarms=%zu callbacks=%llu reconcile=%llu reconcile_skips=%llu probes=%llu snapshots=%llu compacted=%llu sanity_rejects=%llu active_link_faults=%llu\n",
                 static_cast<unsigned long long>(hle_calls_), static_cast<unsigned long long>(switches_),
                 static_cast<unsigned long long>(idle_entries_), scheduler_idle_ ? 1u : 0u,
                 known_threads_.size(), ready_.size(), wait_queues_.size(), alarms_.size(),
                 static_cast<unsigned long long>(alarm_callbacks_),
                 static_cast<unsigned long long>(ready_reconcile_calls_),
                 static_cast<unsigned long long>(ready_reconcile_skips_),
                 static_cast<unsigned long long>(ready_probe_reads_),
                 static_cast<unsigned long long>(ready_snapshot_reads_),
                 static_cast<unsigned long long>(ready_compactions_),
                 static_cast<unsigned long long>(thread_sanity_rejects_),
                 static_cast<unsigned long long>(active_link_faults_));
  }
};

} // namespace GekkoAOT::NativeOS
