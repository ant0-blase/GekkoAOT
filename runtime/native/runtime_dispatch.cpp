// SPDX-License-Identifier: GPL-3.0-or-later
// HostRuntime: cycle accounting, secondary modules and the run loop.
#include "native/runtime_internal.h"

namespace GekkoAOT::Native {

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

  // CPU-domain-only "ASAP" experiment: progress TB and DEC with the actual
  // retired AOT charge rather than with real-time 486 MHz cycles. This runs
  // even when the host hardware clock does not need a new sample. The GPU,
  // video, disc, DSP and audio continue using *hardware_cycles* below.
  // During a genuine idle wait or a zero-charge host callback, preserve wall
  // time so scheduler alarms/DEC cannot stop progressing completely.
  if (guest_work_cpu_timers_) {
    const auto cpu_timer_cycles = CpuTimerCharge(
        true, charged, hardware_cycles, idle_wait);
    if (cpu_timer_cycles != 0u) AdvanceCpuTimers(cpu_timer_cycles);
  }

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
  if(!native_only_) ServiceDiscCompletion(hardware_cycles);

  PollLiveVideoConfig(hardware_cycles);
  if (!guest_work_cpu_timers_) AdvanceCpuTimers(hardware_cycles);
  if(!native_only_) {si_.AdvanceCycles(hardware_cycles);SyncNativeSIInterrupt();}
  vi_.AdvanceCycles(hardware_cycles);
  const bool vi_interrupt_update = vi_.ConsumeInterruptUpdate();
  if (vi_interrupt_update)
    SyncNativeVIInterrupt();

  odd_field_boundary = vi_.ConsumeOddFieldBoundary();
  even_field_boundary = vi_.ConsumeEvenFieldBoundary();
  if (odd_field_boundary || even_field_boundary)
    Capture::Mark(Capture::Kind::ViFieldObserved, perf_vi_boundaries_total_, odd_field_boundary ? 1u : 2u);
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
    // Guest-owned VI boundary: apply data-only Gecko RAM codes without
    // introducing an extra dispatch, thread or virtual AR device.
    if ((++cheat_reload_frames_ % 30u) == 0u) {
      std::string error;
      if (!cheats_.ReloadIfChanged(&error)) {
        std::fprintf(stderr, "GEKKOAOT_CHEATS_RELOAD_ERROR %s\n", error.c_str());
      }
    }
    if (cheats_.Count()) {
      cheats_.Apply(memory_, [this](std::uint32_t address, std::uint32_t length) {
        if (native_gx_.Ready()) native_gx_.NotifyMemoryWrite(address, length);
      });
    }
    ServiceWidescreenFrustumAssist(true);
  }

  // Only after a complete VI frame has elapsed can an already-handled
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
    const std::uint64_t kNativeInputPollCycles = NativeInputPeriod();
    native_input_hw_phase_ += hardware_cycles;
    if (native_input_hw_phase_ >= kNativeInputPollCycles) {
      native_input_hw_phase_ %= kNativeInputPollCycles;
      RefreshNativeInput(!native_only_);
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

  // A fixed secondary DOL/ELF may overlay the main executable at the
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
  // This used to scan the whole 24 MiB MEM1 every time a secondary
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

  // Once ICBI/hash verification has established the active fixed image,
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

  // A PC whose MEM1 page no fixed secondary executable covers has at
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

  // Keep a direct-mapped page route cache in addition to the single
  // current-range cache. Fixed side executables often bounce between a handful
  // of main/overlay chunks each frame; one range slot cannot retain that working
  // set and paid FindModuleChunk/candidate scans again on every bounce. Entries
  // are valid only in the current architectural I-cache publication epoch.
  const std::uint32_t route_page = runtime_address >> 12u;
  // MEM1 is 32 MiB = exactly 8192 4K pages.  Use the guest page
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

  // Routes that never read guest memory are resolved once per
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

// The cache probes above run on every AOT boundary of titles with a
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

  // Once v155 has proven which fixed executable owns the current
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

  // Fixed executable identity is an instruction-cache epoch, not a
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

  // A game can patch a few words of its own chunk at run time: the SDK VM
  // library writes the original vector instruction and a branch back into its
  // DSI/ISI handlers, so that chunk never hashes as compiled again (Medal of
  // Honor: Rising Sun stopped after its first level load). When no chunk
  // matches exactly, the image whose neighbouring chunks all still match is
  // the one in memory: only the patched words differ from its compiled code.
  // A nearly identical image still needs most of its neighbours to match.
  if (matching_count == 0u && candidate_count > 1u) {
    constexpr int kRadius = 8;
    std::size_t best = candidate_count;
    unsigned best_matches = 0u;
    bool tie = false;
    for (std::size_t ci = 0; ci < candidate_count; ++ci) {
      const auto& candidate = candidates[ci];
      unsigned matches = 0u, mismatches = 0u;
      const int center = static_cast<int>(candidate.chunk);
      for (int distance = 1; distance <= kRadius && matches + mismatches < 12u; ++distance) {
        for (const int idx : {center - distance, center + distance}) {
          if (idx < 0 || idx >= static_cast<int>(candidate.descriptor->num_chunk_ranges)) continue;
          const auto range = candidate.descriptor->chunk_ranges[idx];
          const auto expected = candidate.descriptor->chunk_hashes[idx];
          if (!expected || range.end <= range.start) continue;
          const std::size_t size = static_cast<std::size_t>(range.end - range.start);
          if (size > 4u * 1024u * 1024u) continue;
          const auto* bytes = memory_.Resolve(range.start, size);
          if (!bytes) continue;
          if (Fnv1a64(bytes, size) == expected) ++matches;
          else ++mismatches;
        }
      }
      // The VM library patches two neighbouring chunks (DSI and ISI handlers).
      if (matches < 4u || mismatches > 2u) continue;
      if (best != candidate_count && matches == best_matches) tie = true;
      if (best == candidate_count || matches > best_matches) {
        best = ci;
        best_matches = matches;
        tie = false;
      }
    }
    if (best != candidate_count && !tie)
      return commit(candidates[best], "select-patched-chunk", static_cast<int>(best_matches),
                    best_matches);
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
  // An overlay image reloaded over the bootstrap executable (MoH
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
    // A REL's canonical window and its own OSLink placement can overlap
    // (Twilight Princess module 55: canonical 80c6814c, runtime 80c615ac).
    // The address-query sentinel carries canonical provenance. A numerical
    // overlap with a live data section must not turn that canonical address
    // into an identity mapping (large RELs can overlap their own link window).
    static const bool canonical_first = RuntimeEnvBool("GEKKOAOT_REL_OWNER_CANONICAL_FIRST", true);
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

  // The synthetic REL link window and OSLink's live MEM1 placements can
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
    const bool first_dispatch_ok = Capture::Invoke(Capture::Kind::AotSecondary, address, [&] {
      return descriptor->dispatch(&cpu_, address);
    });
    native_region_secondary_owner_ = previous_secondary_owner;
    native_region_probe_capture_ = false;
    if (!first_dispatch_ok) return false;

    // An entry-time region guard can return to the same REL PC before
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

    // A REL is compiled at a synthetic canonical link address, while
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
        // Only registers that can legitimately carry pointer state across a
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
    const std::uint64_t kNativeInputPollCycles = NativeInputPeriod();
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
  // PE token/finish events come from the GX worker, asynchronously to
  // every modelled deadline above. While it still owns queued commands (and
  // after publishing our partial WGPIPE batch), sleep on its event channel so a
  // DrawDone/token wait resumes when the GP work completes, not up to the next
  // VI/maintenance deadline (2 ms) later.
  const auto idle_start = runtime_profile_enabled_ ? std::chrono::steady_clock::now()
                                                  : std::chrono::steady_clock::time_point{};
  // A DVD read in flight is the other asynchronous event an idle guest
  // waits for (every loading screen). Its completion used to be noticed only
  // at the next VI/maintenance deadline, up to 2 ms per request, although the
  // read itself takes microseconds. Wake on the worker's completion instead;
  // slices of at most 500 us keep a concurrent GX event equally prompt.
  if (disc_io_worker_.Busy()) {
    const auto slice = std::min(until, std::chrono::steady_clock::now() + std::chrono::microseconds(500));
    Capture::Scope capture(Capture::Kind::IdleWait, 3u);
    ++idle_disc_waits_;
    disc_io_worker_.WaitUntilComplete(slice);
    AdvanceRuntimeCycles(0u, result, true);
    return;
  }
  const bool gx_wait = Capture::Invoke(Capture::Kind::IdleWait, 1u, [&] {
    return runtime_parallel_workers_ && native_gx_.WaitForWorkerEvent(until);
  });
  if (!gx_wait) {
    Capture::Scope capture(Capture::Kind::IdleWait, 2u);
    std::this_thread::sleep_until(until);
  }
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
  // OSYieldThread with no ready peer of equal or higher priority returns
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
  // A device status register read over and over with an unchanged value
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
  // An AOT wait loop side-exits at its header whenever its cycle budget
  // runs out. When the same header keeps coming back, interpret one iteration
  // in the spin-probe sandbox; if it is a fixed point (same registers, only
  // re-stores of bytes RAM already holds, no HLE/MMIO/SPR/FP), nothing but an
  // interrupt, DMA or another thread can end it: park until the next event.
  // Nemo's `while (VIGetRetraceCount() - start < 2)` frame pacing burned ~40%
  // of its main thread this way.
  if(!fixed_point_park_enabled_ || !idle_event_wait_enabled_ || cpu_.exception ||
     (cpu_.msr&kPpcMsrEe)==0u || pi_.InterruptPending() || decrementer_pending_) return;
  const auto pc=cpu_.pc;
  // A wait loop that side-exits at its header comes straight back to it. A
  // loop whose iteration crosses a few boundaries (calls into AOT functions
  // that toggle MSR[EE], e.g. a queue polled under OSDisableInterrupts)
  // comes back within the last kFixedPointCycle boundaries; anything longer
  // is real work, not a candidate wait.
  bool repeated=false;
  for(const auto recent : fixed_point_recent_) repeated=repeated || recent==pc;
  fixed_point_recent_[fixed_point_recent_next_++ % fixed_point_recent_.size()]=pc;
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
  state.cr=cpu_.cr;state.xer=cpu_.xer;state.lr=cpu_.lr;state.ctr=cpu_.ctr;state.msr=cpu_.msr;
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

  // This helper sits on every safe AOT boundary, but the SDK idle-poll
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

  // The guest OS is idle, i.e. waiting for an interrupt (GXDrawDone waits
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
  // With no runnable thread, a decrementer due again within the
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
  // The guest SDK idle loop gets the same event-driven park as NativeOS.
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
  guest_work_cpu_timers_ = RuntimeEnvBool("GEKKOAOT_CPU_TIMER_GUEST_WORK", false);
  if (!run_banner_reported_) std::fprintf(stderr,
               "GEKKOAOT_CPU_TIMER_POLICY_V1=1 source=%s vi=realtime audio=realtime "
               "dsp=realtime guest_rate_ceiling_pct=%u experimental=%u\n",
               guest_work_cpu_timers_ ? "retired-aot-work" : "realtime-486mhz",
               guest_rate_ceiling_pct_, guest_work_cpu_timers_ ? 1u : 0u);
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

  // The NativeOS service is a process singleton; resolve it once instead
  // of twice per host boundary.
  auto& native_os = GekkoAOT::NativeOS::Service::Get();
  // Opt-in diagnosis for an unexpected guest return through address zero. Keep
  // the normal dispatch path untouched; this is used for retail images whose
  // process appears to exit cleanly after a broken context/exception transfer.
  const bool trace_zero_pc = RuntimeEnvBool("GEKKOAOT_TRACE_ZERO_PC", false);
  std::uint32_t zero_source_pc = 0u, zero_source_lr = 0u, zero_source_sp = 0u;
  std::uint32_t zero_source_thread = 0u;
  bool zero_pc_reported = false;
  const char* zero_source_stage = "run-entry";
  const auto mark_zero_pc_source = [&](const char* stage) {
    if (!trace_zero_pc) return;
    zero_source_pc = cpu_.pc;
    zero_source_lr = cpu_.lr;
    zero_source_sp = cpu_.gpr[1];
    memory_.Read32(0x800000e4u, &zero_source_thread);
    zero_source_stage = stage;
  };
  const auto report_zero_pc = [&](const char* stage, std::uint32_t source_pc,
                                  std::uint32_t source_lr, std::uint32_t source_sp) {
    if (!trace_zero_pc || cpu_.pc != 0u) return;
    zero_pc_reported = true;
    std::uint32_t thread = 0u, stack_lr = 0u, source_insn = 0u;
    memory_.Read32(0x800000e4u, &thread);
    memory_.Read32(source_sp + 4u, &stack_lr);
    memory_.Read32(source_pc, &source_insn);
    std::fprintf(stderr,
                 "GEKKOAOT_GUEST_ZERO_PC_V1 stage=%s source_pc=%08x source_lr=%08x source_sp=%08x source_insn=%08x current_lr=%08x current_sp=%08x current_thread=%08x msr=%08x srr0=%08x srr1=%08x exception=%08x stack_lr=%08x\n",
                 stage, source_pc, source_lr, source_sp, source_insn, cpu_.lr,
                 cpu_.gpr[1], thread, cpu_.msr, cpu_.srr0, cpu_.srr1,
                 cpu_.exception, stack_lr);
  };
  while (dispatch_limit == 0 || result.dispatches < dispatch_limit) {
    mark_zero_pc_source("ServiceDeviceEvents");
    ServiceDeviceEvents();
    if (cpu_.pc == 0u) {
      if (trace_zero_pc && !zero_pc_reported) {
        std::uint32_t current_thread = 0u, current_context = 0u, context_pc = 0u;
        std::uint32_t context_srr0 = 0u, context_srr1 = 0u, stack_lr = 0u;
        std::uint32_t source_instruction = 0u;
        memory_.Read32(0x800000e4u, &current_thread);
        memory_.Read32(0x800000d4u, &current_context);
        memory_.Read32(zero_source_sp + 4u, &stack_lr);
        memory_.Read32(zero_source_pc, &source_instruction);
        if (current_context != 0u) {
          memory_.Read32(current_context + 0x198u, &context_pc);
          memory_.Read32(current_context + 0x19cu, &context_srr0);
          memory_.Read32(current_context + 0x1a0u, &context_srr1);
        }
        std::fprintf(stderr,
                     "GEKKOAOT_GUEST_ZERO_PC_V1 stage=%s source_pc=%08x source_lr=%08x source_sp=%08x source_thread=%08x source_insn=%08x current_lr=%08x current_sp=%08x current_thread=%08x current_ctx=%08x ctx_pc=%08x ctx_srr0=%08x ctx_srr1=%08x msr=%08x srr0=%08x srr1=%08x exception=%08x stack_lr=%08x\n",
                     zero_source_stage, zero_source_pc, zero_source_lr, zero_source_sp,
                     zero_source_thread, source_instruction, cpu_.lr, cpu_.gpr[1],
                     current_thread, current_context, context_pc, context_srr0,
                     context_srr1, cpu_.msr, cpu_.srr0, cpu_.srr1,
                     cpu_.exception, stack_lr);
      }
      result.status = RunStatus::Completed;
      break;
    }

    // A DSI latch must be consumed by entering the 0x300 vector. A host
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
      // GEKKOAOT_BOUNDARY_PROFILE=1 histograms host boundaries by guest
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

      mark_zero_pc_source("NativeOS::Poll(idle)");
      if (native_os.Poll(&cpu_, cpu_.pc)) {
        ++hle_calls_;
        continue;
      }
      if (cpu_.pc != native_os.IdlePc()) continue;

      // Idle means waiting for an interrupt; make queued GPU work visible
      // before delivering a pending one (see ParkSdkIdlePoll).
      native_gx_.PublishPendingWrites();
      // Pending IRQ/DEC latches are almost always clear while the guest
      // is idle. Avoid entering the full exception helpers on that common path.
      if ((pi_.InterruptPending() && TryTakeExternalInterrupt()) ||
          (decrementer_pending_ && TryTakeDecrementerInterrupt()))
        continue;

      if (idle_event_wait_enabled_) {
        // No runnable OSThread means there is no useful PPC work until a
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
    // Keep exact per-boundary interrupt visibility, but make the normal
    // no-event boundary two cheap latch tests instead of two helper calls.
    if (pi_.InterruptPending()) TryTakeExternalInterrupt();
    if (decrementer_pending_) TryTakeDecrementerInterrupt();
    if (time_poll_return_ != 0u) {
      Capture::Scope capture(Capture::Kind::ClockPark);
      ParkClockSpin(result);
    }
    if (guest_rate_ceiling_pct_ != 0u && rate_charged_ - rate_mark_charged_ >= 65536u)
      ThrottleGuestRate(result);
    { Capture::Scope capture(Capture::Kind::PollPark);
      ParkSdkIdlePoll(result);
      ParkNativeCounterPoll(result);
      ParkNativeReadPoll(result);
      ParkFixedPointPoll(result);
    }
    if (result.status == RunStatus::HostRequestedExit) break;

    const std::uint32_t native_os_pc = cpu_.pc;
    mark_zero_pc_source("NativeOS::Poll");
    if (native_os.Poll(&cpu_, native_os_pc)) {
      report_zero_pc("NativeOS::Poll", native_os_pc, zero_source_lr, zero_source_sp);
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
    // Low-memory vectors are exceptional. Avoid entering the sizeable
    // interpreter helper at every ordinary MEM1 function boundary; the cheap
    // page-mask prefilter is identical to the helper's first semantic test.
    const std::uint32_t dispatch_physical = AddressSpace::ToPhysical(dispatch_pc);
    bool native_low_vector = false;
    if (IsLowExceptionVectorAddress(dispatch_physical) &&
        ((native_low_vector = ExecuteNativeLowMemoryVector(dispatch_pc, &low_memory_cycles)) ||
         ExecuteLowMemoryInstruction(dispatch_pc, &low_memory_cycles))) {
      // First-level exception vectors run with MSR[EE]=0, so no
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

    // Some retail SDK builds leave OSGetTime/OSGetTick as tiny leaf
    // helpers instead of exposing a resolver-friendly symbol. Executing these
    // through overlapping fixed executable images can turn an otherwise trivial
    // timebase read into a permanent busy loop.
    //
    // Safe host boundaries can revisit the same PCs millions of times.
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
        // Classify exact integer leaf tails (see TryExecuteLeafTail).
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

    // Fixed DOL/ELF executables may replace each other at identical
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
      mark_zero_pc_source("DispatchFixedExecutableModule");
      if (DispatchFixedExecutableModule(fixed_descriptor, dispatch_pc, result)) {
        report_zero_pc("DispatchFixedExecutableModule", dispatch_pc, zero_source_lr,
                       zero_source_sp);
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
    mark_zero_pc_source("AOT-main-dispatch");
    const std::uint32_t dispatch_lr_before = cpu_.lr;
    const std::uint32_t dispatch_sp_before = cpu_.gpr[1];
    const bool dispatched_main = Capture::Invoke(Capture::Kind::AotMain, dispatch_pc, [&] {
          return descriptor->dispatch(&cpu_, dispatch_pc);
        });
    report_zero_pc("AOT-main-dispatch", dispatch_pc, dispatch_lr_before,
                   dispatch_sp_before);
    if (!dispatched_main) {
      // Runtime-loaded RELs live at OSLink-selected addresses that are not part
      // of main.dol. Translate that live PC to the secondary module's stable
      // DolRecomp link address and run its native dispatcher first.
      mark_zero_pc_source("DispatchSecondaryModule");
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
      mark_zero_pc_source("DispatchHostCall");
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
