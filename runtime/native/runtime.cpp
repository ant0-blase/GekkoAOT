// SPDX-License-Identifier: GPL-3.0-or-later
// HostRuntime: construction, reset, module loading, disc and DI.
#include "native/runtime_internal.h"

namespace GekkoAOT::Native {

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

  // AOT execution itself is never throttled. The GameCube-visible clock
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
  native_only_=RuntimeEnvBool("GEKKOAOT_NATIVE_ONLY",false);
  sram_control_address_=0;
  ResetDeviceServices();
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
  reciprocal_camera_providers_.clear();
  reciprocal_camera_scan_epoch_ = 0u;
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
  graphics_revision_=graphics_ack_revision_=graphics_ack_polls_=0;
  graphics_ack_pending_=false;graphics_ack_accepted_=true;
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
  gx_set_array_proof_ = {};
  native_hook_proof_last_ = nullptr; native_hook_proof_last_address_ = 0u;
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
  idle_disc_waits_ = 0u;
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
  si_.SetTransferEvents(GekkoAOT::NativeHW::SI::TransferEvents(
      native_hw_.SI(), RuntimeEnvBool("GEKKOAOT_NATIVE_SI_TRANSFER_EVENTS", false)));
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
  const bool sdk_bound=module_.BindNativeSdk(NativeSdkServices());
  if(!sdk_bound && (module_.Capabilities()&GEKKOAOT_MODULE_CAP_NATIVE_SDK)) {
    module_.Close();return ModuleStatus::NativeSdkBindingRejected;
  }
  if(sdk_bound)
    std::fprintf(stderr,"GEKKOAOT_NATIVE_SDK_BIND_V2=1 scope=primary services=generated-registry cpu-abi=unchanged\n");
  if (module_.BindNativeHardware(NativeHardwareServices()))
    std::fprintf(stderr, "GEKKOAOT_NATIVE_HARDWARE_BIND_V243=1 scope=primary binding=available cpu-abi=unchanged\n");
  if (module_.BindNativeWgpipeTry(NativeWgpipeTryServices()))
    std::fprintf(stderr, "GEKKOAOT_NATIVE_WGPIPE_TRY_BIND_V244=1 scope=primary binding=available cpu-abi=unchanged inline-window=v372\n");
  else if (module_.BindNativeWgpipeTry(NativeWgpipeTryServicesV1()))
    std::fprintf(stderr, "GEKKOAOT_NATIVE_WGPIPE_TRY_BIND_V244=1 scope=primary binding=available cpu-abi=unchanged inline-window=none\n");
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
  // The IPL hands over with a stack and the small-data bases set (the
  // values Dolphin's emulated BS2 uses). The SDK's __start sets them again,
  // but some titles enter a plain C function (Tony Hawk's Pro Skater 3:
  // `stwu r1,-8(r1)` at the entry) and faulted at 0xfffffff8.
  if (cpu_.gpr[1] == 0u) cpu_.gpr[1] = 0x816ffff0u;
  if (cpu_.gpr[2] == 0u) cpu_.gpr[2] = 0x81465cc0u;
  if (cpu_.gpr[13] == 0u) cpu_.gpr[13] = 0x81465320u;
  if (descriptor->on_state_loaded) descriptor->on_state_loaded(&cpu_);
  return ModuleStatus::Ok;
}

ModuleStatus HostRuntime::LoadSecondaryModule(const std::filesystem::path& path,
                                              const char* expected_game_id) {
  auto library = std::make_unique<ModuleLibrary>();
  // REL side modules carry a native entry per guest instruction, so one
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
  if(!library->BindNativeSdk(NativeSdkServices()) && (library->Capabilities()&GEKKOAOT_MODULE_CAP_NATIVE_SDK)) {
    secondary_module_last_error_=ModuleStatusName(ModuleStatus::NativeSdkBindingRejected);
    return ModuleStatus::NativeSdkBindingRejected;
  }
  if (!library->BindNativeWgpipeTry(NativeWgpipeTryServices()))
    (void)library->BindNativeWgpipeTry(NativeWgpipeTryServicesV1());
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
  disc_io_worker_.Drain();
  di_io_inflight_ = false;
  di_io_request_queued_ = false;
}

void HostRuntime::DetachDiscImage() {
  QuiesceDiscIo();
  disc_io_worker_.Stop();
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

  disc_io_worker_.Start(this, &HostRuntime::DiscAudioRead);
  di_.SetDiscPresent(true);
  di_.SetDeviceHooks({this, &HostRuntime::DiscCommand, &HostRuntime::DiscAudioRead});
  return true;
}

bool HostRuntime::ReadDiscImage(std::uint64_t offset, void* destination, std::uint32_t size) {
  GekkoAOT::Perf::Scope latency(GekkoAOT::Perf::Metric::IoRead);
  std::lock_guard<std::mutex> io_lock(disc_io_mutex_);
  if (!destination && size) return false;
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

    // The asynchronous host read owns its staging buffer until completion.
    // Publish the result before notifying the caller, and discard cancelled
    // requests. This also protects a buffer reused for another request.
    if (!self->memory_.Resolve(request.dma_address, transfer)) {
      ++self->di_read_failures_;
      response.error_code = 0x00031100u;
      return response;
    }
    self->di_async_staging_.clear();
    // A cancelled read may still be finishing in the worker. Queue its
    // successor here without joining, allocating or waiting on the AOT thread.
    self->di_io_request_queued_ = true;

    ++self->di_reads_;
    self->di_read_bytes_ += transfer;

    // Native service completion has no synthetic optical-drive delay.
    self->di_async_completion_pending_ = true;
    const bool native_vfs_timing =
        GekkoAOT::NativeHW::DI::HostCompletion(
            self->native_hw_.DI(), self->native_vfs_.Ready(),
            EnvNativeVfs(), EnvVfsGcTiming());
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
    if (self->disc_io_worker_.Idle() &&
        self->disc_io_worker_.Submit(request.sequence, offset, transfer)) {
      self->di_io_request_queued_ = false;
      self->di_io_inflight_ = true;
    }
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
                   "persistent-worker");

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
  if(kind==SimpleHleKind::DvdReadPrio) {
    // This is a software wait adapter around DVDReadAbsAsyncPrio. Preserve
    // its OSSleepThread/wakeup ABI: an immediate blocking host read starves
    // other game threads. Its actual I/O is still the native permanent worker.
    rejected_simple_hle_hooks_[address]=kind;
    return;
  }
  // Camera/GX overlay candidates may be registered before their guest code
  // is loaded. They execute only after DispatchSimpleHle proves the ENTIRE
  // currently resident body on every call; bootstrap chunk hashes cannot
  // admit or permanently reject these versioned renderer providers.
  if(kind==SimpleHleKind::AiGetCount) {
    const auto* body=memory_.Resolve(address,16);
    if(!body || !GekkoAOTSdk::ProveAiCounter({body,16})) {
      rejected_simple_hle_hooks_[address]=kind;
      std::fprintf(stderr,"GEKKOAOT_NATIVE_SDK_REJECT name=%s pc=%08x reason=unproven-ai-counter-address\n",SimpleServiceName(unsigned(kind)),address);
      return;
    }
  }
  if(kind==SimpleHleKind::OsUnlockSram || kind==SimpleHleKind::OsUnlockSramEx) {
    const auto first=kind==SimpleHleKind::OsUnlockSramEx?address-36:address;
    const auto* body=memory_.Resolve(first,88);
    if(!body || !GekkoAOTSdk::ProveSramUnlockFamily({body,88},first)) {
      rejected_simple_hle_hooks_[address]=kind;
      std::fprintf(stderr,"GEKKOAOT_NATIVE_SDK_REJECT name=%s pc=%08x reason=unproven-sram-wrapper-callee\n",SimpleServiceName(unsigned(kind)),address);
      return;
    }
  }
  if(kind==SimpleHleKind::CardInit) {
    if(const auto* body=memory_.Resolve(address,348))
      if(const auto proof=GekkoAOTSdk::ProveLegacyCardControl({body,348});
         proof && memory_.Resolve(proof->base,2*0x108) && memory_.Resolve(proof->disk_none,32)) {
        devices_.card_control=proof->base;devices_.card_stride=0x108;
        devices_.card_disk_none=proof->disk_none;devices_.card_disk_sda=proof->disk_sda;
      }
  }
  if(kind==SimpleHleKind::CardSetGlobalDiskId) {
    if(const auto* code=memory_.Resolve(address,56))
      if(const auto base=GekkoAOTSdk::ProveCardDiskSetter({code,56});
         base && memory_.Resolve(*base,2*0x110+32)) {
        devices_.card_control=*base;devices_.card_stride=0x110;
        devices_.card_disk_none=*base+2*0x110;
      }
  }
  if (kind == SimpleHleKind::CardGetDiskId) {
    if (const auto* code = memory_.Resolve(address, 24))
      if (const auto base = GekkoAOTSdk::ProveCardControl({code,24});
          base && memory_.Resolve(*base, 2 * 0x110)) devices_.card_control = *base;
  }
  if(kind==SimpleHleKind::PadSetAnalogMode) {
    if(const auto* code=memory_.Resolve(address,512))
      devices_.pad_analog_sda=GekkoAOTSdk::ProvePadAnalogModeCell({code,512});
  }
  if (kind == SimpleHleKind::SiSetXy) {
    if(const auto* code=memory_.Resolve(address,108)) {
      auto cell=GekkoAOTSdk::ProveSiPolicy({code,108});
      if(!cell)cell=GekkoAOTSdk::ProveSiPolicy({code,96});
      if(cell&&memory_.Resolve(*cell,4))devices_.input_policy=*cell;
    }
  }
  if(kind==SimpleHleKind::OsSyncSram) {
    const auto* code=memory_.Resolve(address,16);
    if(code) {
      const auto word=[](const std::uint8_t* p){return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];};
      const auto hi=word(code),lo=word(code+4);
      if((hi&0xffff0000u)==0x3c600000u && (lo&0xffff0000u)==0x38630000u && word(code+8)==0x8063004cu && word(code+12)==0x4e800020u) {
        const std::uint32_t base=(hi<<16)+static_cast<std::int16_t>(lo);
        if(memory_.Resolve(base,0x54)) sram_control_address_=base;
      }
    }
  }
  simple_hle_hooks_[address] = kind;
  if(kind==SimpleHleKind::AngularCameraSetFov || kind==SimpleHleKind::TangentCameraFrustum ||
     (kind>=SimpleHleKind::GxSetAlphaCompare && kind<=SimpleHleKind::GxSetAlphaUpdate))
  { native_hook_code_proofs_.erase(address); // Full resident-body proof at execution.
    native_hook_proof_last_ = nullptr; native_hook_proof_last_address_ = 0u; }
  else RegisterNativeHookCodeProof(address);
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
  Capture::Mark(Capture::Kind::PeGuestDelivery, reg, value);
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

} // namespace GekkoAOT::Native
