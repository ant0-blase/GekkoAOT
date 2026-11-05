// SPDX-License-Identifier: GPL-3.0-or-later
// HostRuntime: host calls, SPRs, exceptions, low memory, metrics and widescreen assist.
#include "native/runtime_internal.h"

namespace GekkoAOT::Native {

bool HostRuntime::HostCall(CPUState* cpu, std::uint32_t address) {
  auto* self = Self(cpu);
  if (!self) return false;
  // Idle-loop effects are counted by DispatchHostCall (real HLE only);
  // admission, REL-translation and indirect-routing probes are not events.
  // Clock getters are proven scalar SDK leaves in the static manifest.
  // Keep the generated callback ABI and host-boundary epoch, but avoid entering
  // the general SDK dispatcher and its libc/VM scratch-buffer machinery. The
  // guest timebase stays authoritative; advancing events is still the runner's
  // responsibility at the same boundary as before.
  if (address == (kStaticDirectSimpleBase | static_cast<std::uint32_t>(SimpleHleKind::OsGetTick))) {
    ++self->hle_calls_;
    ++self->simple_hle_kind_calls_[static_cast<std::size_t>(SimpleHleKind::OsGetTick)];
    self->NoteClockCall(false);
    GekkoAOT::Time::GetTick(&self->cpu_);
    if (self->track_host_effects_) ++self->host_effects_;
    BumpHostWriteEpoch(cpu);
    return true;
  }
  if (address == (kStaticDirectSimpleBase | static_cast<std::uint32_t>(SimpleHleKind::OsGetTime))) {
    ++self->hle_calls_;
    ++self->simple_hle_kind_calls_[static_cast<std::size_t>(SimpleHleKind::OsGetTime)];
    self->NoteClockCall(true);
    GekkoAOT::Time::GetTime(&self->cpu_);
    if (self->track_host_effects_) ++self->host_effects_;
    BumpHostWriteEpoch(cpu);
    return true;
  }
  // Statically lowered device SDK calls have a known positive destination:
  // avoid generic host-call routing and all libc string scratch setup.
  if((address&kStaticDirectMask)==kStaticDirectSimpleBase) {
    const auto kind=static_cast<unsigned>(address&0xffu);
    if((kind>=static_cast<unsigned>(SimpleHleKind::OsGetRtc)||kind==static_cast<unsigned>(SimpleHleKind::PadRead))&&kind<static_cast<unsigned>(SimpleHleKind::Count)) {
      // A stale module can contain a positive token from an older resolver
      // (notably relocation-identical CARD format/delete wrappers). Validate
      // the current exact plan before any device/storage side effect.
      if(!self->AdmitNativeSdkKind(static_cast<SimpleHleKind>(kind)))return false;
      const bool handled=self->DispatchSimpleHle(static_cast<SimpleHleKind>(kind),cpu->pc);
      if(handled){if(self->track_host_effects_)++self->host_effects_;BumpHostWriteEpoch(cpu);}
      return handled;
    }
  }
  // Strcmp is a pure leaf that EA titles call ~1M times per second.
  // With both strings in cached MEM1 compare in place: no dispatcher, no
  // scratch copies, and no host-write epoch (nothing but r3/PC changes, so
  // timer/interrupt state is unaffected). Anything else -- VM pages, other
  // apertures, a string running off MEM1 -- takes the general VM-safe path.
  if(address==(kStaticDirectSimpleBase|unsigned(SimpleHleKind::Strcmp)) && self->TryNativeStrcmpFast())return true;
  // Game-library leaves touch guest memory, GQRs and PC only, so like
  // the libc leaves they keep the native superchain running.
  if((address&kStaticDirectMask)==GekkoAOT::Native::EaMpeg::kGameLibraryBase)
    return self->DispatchGameLibrary(address&0xffu);
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
  // Libc leaves only touch guest GPRs and plain memory (a raised DSI
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

  // Bounded transition trace for the real CP FIFO breakpoint path. This
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
  pi_.SetInterrupt(GekkoAOT::HW::PI::NativePI::Audio, !devices_.ai_owned && ai_.InterruptPending());
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

  // Distinguish a missing VI latch from a correctly latched interrupt
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
  // One deadlock is unambiguous and recovered whatever the policy: the
  // SDK overflow handler suspended the FIFO writer (FIFO at/over the high
  // watermark, overflow IRQ off, underflow IRQ on: it waits for a drain)
  // while the GP is held at an armed breakpoint that raises no interrupt, so
  // nothing can ever release it. Star Fox Adventures hit this after minutes
  // of play (ThornTail Hollow, writer thread suspended, game frozen).
  const auto control = cp_.Control();
  const bool writer_waits_for_drain =
      cp_.FifoReadWriteDistance() >= cp_.FifoHiWatermark() &&
      (control & HW::CP::NativeCP::kCtrlUnderflowIntEnable) != 0u &&
      (control & HW::CP::NativeCP::kCtrlOverflowIntEnable) == 0u;
  if (!frame_boundary || !(cp_idle_breakpoint_recovery_enabled_ || writer_waits_for_drain)) return;

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
  // The suspended writer resumes only once the FIFO drains below its low
  // watermark: let the GP continue past the released breakpoint.
  if (writer_waits_for_drain) cp_.ResumeDrainAfterStep();

  SyncNativeGXInterrupts();
  SyncNativeCPInterrupt();
  ++cp_idle_breakpoint_recoveries_;

  std::fprintf(stderr,
               "GEKKOAOT_CP_IDLE_BREAKPOINT_RECOVERY_V160=1 phase=step count=%llu "
               "bp=%08x read=%08x->%08x distance=%08x->%08x pe_token=%04x->%04x "
               "current=0 active=%08x overflow_deadlock=%u\n",
               static_cast<unsigned long long>(cp_idle_breakpoint_recoveries_),
               breakpoint, read_before, cp_.FifoReadPointer(), distance_before,
               cp_.FifoReadWriteDistance(), unsigned(token_before), unsigned(pe_.Token()),
               active_head, writer_waits_for_drain ? 1u : 0u);

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
  // NativePI caches the derived cause&mask level.  Reject the normal
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

  // A thread that yields with a very short alarm (EA's THREAD_yieldticks(10),
  // 250 ns) re-arms the decrementer faster than it can reach the idle loop, where
  // v355 coalesces: GoldenEye took 570k exceptions/s and the guest OS scheduler
  // consumed the whole main thread. Once DEC exceptions run back to back, hold
  // them to one per quantum, as if the CPU had been busy for that long.
  if (idle_dec_quantum_ticks_) {
    const std::uint64_t since = cpu_.timebase - dec_taken_tb_;
    if (dec_storm_ && since < idle_dec_quantum_ticks_) return false;
    if (since < idle_dec_quantum_ticks_ / 4u) {
      if (dec_short_intervals_ < 16u && ++dec_short_intervals_ == 16u) dec_storm_ = true;
    } else if (since >= idle_dec_quantum_ticks_ * 2u) {
      dec_short_intervals_ = 0u;
      dec_storm_ = false;
    }
    dec_taken_tb_ = cpu_.timebase;
  }

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
  if(self->native_only_&&(spr==kSprDmau||spr==kSprDmal)) {
    std::fprintf(stderr,"GEKKOAOT_NATIVE_ONLY_UNSUPPORTED subsystem=LC operation=spr-read spr=%u pc=%08x\n",spr,address);
    self->SetFault(FaultKind::SprRead,address,spr);return 0;
  }
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
      // Fixed-address DOL/ELF files discovered on disc are not necessarily
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
    // Sample guest DEC programming (bounded) to diagnose decrementer storms.
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
  if(self->native_only_ && (spr==kSprDmau || spr==kSprDmal)) {
    std::fprintf(stderr,"GEKKOAOT_NATIVE_ONLY_UNSUPPORTED subsystem=LC operation=spr-write spr=%u pc=%08x\n",spr,address);
    self->SetFault(FaultKind::SprWrite,address,spr); return;
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
    // A line outside every fixed-image chunk cannot change a chunk hash,
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
  // Fingerprint (see sdk/word_proof.h): the two LIS/ADDI range pairs are
  // patched by the apploader, the rest of the routine is fixed.
  static constexpr GekkoAOTSdk::WordProof kCopiedCacheStubProof{"HHHHFFFFFFFFFFFFFFF", 0x0bab55d675a9f8edull};
  if (!cycles || (address & 3u) || (cpu_.msr & kPpcMsrPr)) return false;
  const auto* code = memory_.Resolve(address, 76u);
  if (!code) return false;
  const auto word = [&](unsigned i) {
    const auto* p = code + i * 4u;
    return (std::uint32_t(p[0]) << 24u) | (std::uint32_t(p[1]) << 16u) |
           (std::uint32_t(p[2]) << 8u) | p[3];
  };
  if (!GekkoAOTSdk::MatchesWordProof(code, 76u, kCopiedCacheStubProof)) return false;
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

// Called at every HostRuntime boundary (top of Run's loop).
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

// Generated code sets MSR[EE] without leaving AOT execution unless this
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

// OSDisableInterrupts/OSEnableInterrupts/OSRestoreInterrupts end with
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
  // Dispatch probes used to be flushed (128 KiB) on every icbi epoch.
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

  if (primary >= 24u && primary <= 29u) {
    const std::uint32_t rs = (instruction >> 21) & 31u;
    const std::uint32_t ra = (instruction >> 16) & 31u;
    const std::uint32_t immediate = instruction & 0xffffu;
    std::uint32_t value = cpu_.gpr[rs];
    switch (primary) {
    case 24u: value |= immediate; break;                         // ori
    case 25u: value |= immediate << 16; break;                   // oris
    case 26u: value ^= immediate; break;                         // xori
    case 27u: value ^= immediate << 16; break;                   // xoris
    case 28u: value &= immediate; break;                         // andi.
    case 29u: value &= immediate << 16; break;                   // andis.
    }
    cpu_.gpr[ra] = value;
    if (primary == 28u || primary == 29u) SetCr0FromResult(&cpu_, value);
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
  // Idle/event waits call with charged=0 after a real wall-clock park.
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
  Capture::Mark(Capture::Kind::PresentRequest, xfb_top, host_present_pending_ ? 1u : 0u);
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
  Capture::Mark(Capture::Kind::GuestFrameObserved, perf_guest_frames_total_);
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
    // Advance by whole periods so the cadence holds its exact rate
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
    // The in-game HUD shows these live, with this thread's CPU load
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
  // Opt-in guest OS thread dump (GEKKOAOT_GUEST_THREAD_DUMP=1, every
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
  // Interrupt delivery rates; a storm (an unacknowledged cause or an
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
  // Host-boundary telemetry; every optimization that keeps guest code
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
    // Busiest native services (s<SimpleHleKind>, o<NativeOS::Kind>).
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
    std::fprintf(stderr,"GEKKOAOT_NATIVE_GX_SDK_LEAVES interval_s=%.6f alpha_compare_calls=%llu z_mode_calls=%llu color_update_calls=%llu alpha_update_calls=%llu\n",seconds,
      (unsigned long long)simple_hle_kind_calls_[unsigned(SimpleHleKind::GxSetAlphaCompare)],
      (unsigned long long)simple_hle_kind_calls_[unsigned(SimpleHleKind::GxSetZMode)],
      (unsigned long long)simple_hle_kind_calls_[unsigned(SimpleHleKind::GxSetColorUpdate)],
      (unsigned long long)simple_hle_kind_calls_[unsigned(SimpleHleKind::GxSetAlphaUpdate)]);
    // Cumulative counts survive periodic reports. No clocks or strings in
    // the service hot path; stringify only when profiling was requested.
    static constexpr const char* sdk_names[]={
#define SDK_SIMPLE(kind,name,abi) name,
#define SDK_OS(kind,name,abi) name,
#include "sdk/native_services.def"
#undef SDK_SIMPLE
#undef SDK_OS
    };
    for(unsigned slot=0;slot<GekkoAOTNativeSdkCount;++slot)if(native_sdk_direct_calls_[slot] || native_sdk_direct_declines_[slot])
      std::fprintf(stderr,"GEKKOAOT_NATIVE_SDK_DIRECT_V371 name=%s accepted_calls=%llu declined_calls=%llu counter_scope=runtime-lifetime\n",
        sdk_names[slot],(unsigned long long)native_sdk_direct_calls_[slot],(unsigned long long)native_sdk_direct_declines_[slot]);
    for(const auto& [key,calls]:native_sdk_entry_calls_)
      std::fprintf(stderr,"GEKKOAOT_NATIVE_SDK_ENTRY_V371 pc=%08x name=%s accepted_calls=%llu declined_calls=%llu counter_scope=runtime-lifetime\n",
        unsigned(key>>32u),sdk_names[unsigned(key)],(unsigned long long)calls.accepted,(unsigned long long)calls.declined);
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
      GekkoAOT::HW::VI::NativeVI::CpuClockHz / 60u;
  if (live_video_config_poll_cycles_ < kPollCycles) return;
  live_video_config_poll_cycles_ %= kPollCycles;

  const auto acknowledge=[&]{
    const auto pending=native_gx_.PendingTextureBytes();const bool known=pending!=~std::uint64_t(0),busy=known&&pending>0;
    const bool revised=graphics_revision_!=graphics_ack_revision_;
    if(!graphics_revision_ || (!revised && (busy==graphics_ack_pending_ || graphics_ack_polls_%60)))return;
    const auto text=std::string("{\"revision\":")+std::to_string(graphics_revision_)+",\"accepted\":"+(graphics_ack_accepted_?"true":"false")+
      ",\"pending_status_known\":"+(known?"true":"false")+",\"pending_texture_bytes\":"+std::to_string(known?pending:0)+"}\n";
    if(ReplaceFile(live_video_config_path_.parent_path()/"graphics-applied.json",{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()})){
      graphics_ack_revision_=graphics_revision_;graphics_ack_pending_=busy;
    }
  };
  ++graphics_ack_polls_;acknowledge();
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
      const auto trim=[](std::string text){const auto first=text.find_first_not_of(" \t");return first==text.npos?std::string{}:text.substr(first,text.find_last_not_of(" \t")-first+1);};
      const auto key=trim(line.substr(0,equals)),value=trim(line.substr(equals+1));
      if(key=="graphics_revision"){char* end=nullptr;const auto revision=std::strtoull(value.c_str(),&end,10);if(end!=value.c_str()&&end&&!*end)graphics_revision_=revision;}

      if(key=="fps_limit"){
        char* end=nullptr;const auto parsed=std::strtoul(value.c_str(),&end,10);
        if(end!=value.c_str() && end && !*end && parsed>=1u && parsed<=1000u){next_fps=static_cast<std::uint32_t>(parsed);have_fps=true;}
      }
      if(key=="aspect_ratio"){aspect_mode=value;have_aspect=true;}
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
  graphics_ack_accepted_=native_gx_.ApplyGraphicsSettings(next_graphics);
  if (graphics_ack_accepted_) {
    live_graphics_settings_ = next_graphics;
    const bool interpolation = next_graphics.interpolation && native_gx_.HasFrameInterpolation();
    if (interpolation != frame_interpolation_enabled_) {
      frame_interpolation_enabled_ = interpolation;
      native_gx_.SetFrameReadyFence(interpolation);
      interpolation_pair_valid_ = false;
      host_present_started_ = false;
    }
  }
  acknowledge();
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
        const auto equals=line.find('=');
        if(equals!=std::string::npos){
          const auto trim=[](std::string text){const auto a=text.find_first_not_of(" \t");return a==text.npos?std::string{}:text.substr(a,text.find_last_not_of(" \t")-a+1);};
          if(trim(line.substr(0,equals))=="paused"){
            const auto value=trim(line.substr(equals+1));pause_requested=value=="true"||value=="1";
          }
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

void HostRuntime::ServiceReciprocalFrustumSubmission() {
  for(const auto& provider:reciprocal_camera_providers_){
    if(cpu_.pc<provider.projection_sdk || cpu_.pc>=provider.projection_sdk+164u)continue;
    if(!GekkoAOTSdk::ProveReciprocalCameraFrustum(provider.consumer,[&](auto a,auto n){return memory_.Resolve(a,n);}))continue;
    // GXSetProjection is a leaf. Its caller's stack must return directly to
    // one of the two proven builder callsites in the six-plane consumer.
    std::uint32_t callerReturn=0,consumerFrame=0,cameraAddress=0;
    if(!memory_.Read32(cpu_.gpr[1]+0x8c,&callerReturn) ||
       (callerReturn!=provider.consumer+0x2a8 && callerReturn!=provider.consumer+0x2c8) ||
       !memory_.Read32(cpu_.gpr[1],&consumerFrame) ||
       !memory_.Read32(cpu_.gpr[13]+std::int32_t(provider.camera_sda),&cameraAddress))continue;
    const auto* camera=memory_.Resolve(cameraAddress,0x108);
    if(!camera || (camera[0xdb]&1))continue;
    const float x=GX::ReadBigEndianFloat(camera+0xdc),y=GX::ReadBigEndianFloat(camera+0xe0);
    std::array<float,6> observed{};float fov=0,aspect=0;
    if(!native_gx_.CurrentPerspectiveProjection(observed)||!native_gx_.CurrentPerspectiveCamera(&fov,&aspect))continue;
    const auto close=[](float a,float b){return std::isfinite(a)&&std::isfinite(b)&&std::abs(a-b)<std::max(.00001f,std::abs(b)*.002f);};
    const float near=GX::ReadBigEndianFloat(camera+0xe4),far=GX::ReadBigEndianFloat(camera+0xe8);
    if(!close(x,std::abs(observed[0])) || !close(y,std::abs(observed[2])) ||
       !close(-near/(far-near),observed[4]) || !close(-far*near/(far-near),observed[5]))continue;
    const float fovScale=GX::PerspectiveFovScale(y,live_graphics_settings_.fov_y);
    const float target=native_gx_.WidescreenTargetAspect();
    const bool alreadyWide=std::isfinite(x)&&x>0 && std::abs(y/x-target)<=target*.005f;
    const float wideScale=native_gx_.WidescreenActive()&&!alreadyWide?aspect/target:1.f;
    auto* planes=memory_.Resolve(provider.planes,96);
    auto* bounds=memory_.Resolve(consumerFrame+8,16);
    if(!GX::AdjustReciprocalFrustum(x,y,x*wideScale*fovScale,y*fovScale,planes,bounds))continue;
    native_gx_.NotifyMemoryWrite(provider.planes,64);
    native_gx_.NotifyMemoryWrite(consumerFrame+8,16);
    static std::uint64_t updates=0;static std::uint32_t previousCamera=0;
    const bool cameraChanged=previousCamera!=cameraAddress;previousCamera=cameraAddress;
    if(++updates<=8 || cameraChanged || updates%3600==0)std::fprintf(stderr,
      "GEKKOAOT_CAMERA_FRUSTUM provider=reciprocal-submission camera=%08x planes=%08x bounds=%08x lens=%.6f,%.6f->%.6f,%.6f updates=%llu culling=guest-six-planes+portal-bounds\n",
      cameraAddress,provider.planes,consumerFrame+8,x,y,x*wideScale*fovScale,y*fovScale,(unsigned long long)updates);
  }
}

void HostRuntime::ServiceWidescreenFrustumAssist(bool frame_boundary) {
  if (!frame_boundary || !widescreen_frustum_assist_enabled_) return;

  // Whole-body camera providers work with cached AOT modules: adjust their
  // shared inputs at the frame boundary; the original guest builder still
  // rebuilds and tests its own six planes. No new static intercept is needed.
  if(reciprocal_camera_scan_epoch_!=fixed_exec_structure_epoch_){
    reciprocal_camera_scan_epoch_=fixed_exec_structure_epoch_;reciprocal_camera_providers_.clear();j3d_camera_provider_=false;
    const auto scan=[&](std::uint32_t begin,std::uint32_t end){
      for(auto pc=begin;std::uint64_t(pc)+840<=end;pc+=4){
        const auto* prefix=memory_.Resolve(pc,4);
        if(prefix && GekkoAOTSdk::StructuralReadBE32(prefix)==0x9421ffb0u &&
           GekkoAOTSdk::ProveJ3dFrustum(pc,[&](auto a,auto n){return memory_.Resolve(a,n);})){
          RegisterSimpleHle(SimpleHleKind::TangentCameraFrustum,pc);j3d_camera_provider_=true;
          std::fprintf(stderr,"GEKKOAOT_CAMERA_PROVIDER kind=j3d-frustum pc=%08x proof=entire-builder+math-callees\n",pc);
        }
        if(!prefix||prefix[0]!=0x94||prefix[1]!=0x21||prefix[2]!=0xff||prefix[3]!=0xa8)continue;
        if(auto offset=GekkoAOTSdk::ProveReciprocalCameraFrustum(pc,[&](auto address,auto bytes){return memory_.Resolve(address,bytes);})){reciprocal_camera_providers_.push_back(*offset);
          std::fprintf(stderr,"GEKKOAOT_CAMERA_PROVIDER kind=reciprocal-shared-frustum pc=%08x proof=entire-plane-builder sda=%d\n",pc,int(offset->camera_sda));}
      }
    };
    for(const auto& range:loaded_dol_code_ranges_)scan(range.start,range.end);
  }
  // These providers consume per-frame rebuilt lenses: the submission boundary
  // below repairs their freshly built CPU planes, never stale camera fields.
  if(!reciprocal_camera_providers_.empty() || j3d_camera_provider_)return;

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
  // Poll one atomic completion flag. Media work and buffer growth stay on
  // the permanent worker; cancellation never joins it on this path.
  if (di_async_completion_pending_ &&
      ((di_.DMAControl() & 1u) == 0u || di_.CommandSequence() != di_async_sequence_)) {
    di_async_completion_pending_ = false;
    di_io_request_queued_ = false;
    di_async_staging_.clear();
  }
  if (di_async_completion_pending_)
    di_async_cycles_remaining_ = cycles >= di_async_cycles_remaining_ ? 0u : di_async_cycles_remaining_ - cycles;
  if (di_io_inflight_) {
    DiscIoWorker::Result io;
    if (!disc_io_worker_.TryCollect(io, di_async_staging_)) return;
    di_io_inflight_ = false;
    if (!di_async_completion_pending_ || io.sequence != di_async_sequence_ || !io.ok)
      di_async_staging_.clear();
  }
  if (di_io_request_queued_ && di_async_completion_pending_) {
    if (disc_io_worker_.Submit(di_async_sequence_, di_async_disc_offset_, di_async_bytes_)) {
      di_io_request_queued_ = false;
      di_io_inflight_ = true;
    }
    return;
  }
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
      di_async_staging_.clear();
    } else if (di_async_cycles_remaining_ == 0u) {
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
    }
  }
}

} // namespace GekkoAOT::Native
