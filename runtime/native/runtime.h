#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/cpu_state.h"
#include "dsp/native/native_dsp.h"
#include "gx/host/host_bridge.h"
#include "hw/ai/native_ai.h"
#include "hw/cp/native_cp.h"
#include "hw/di/native_di.h"
#include "hw/exi/native_exi.h"
#include "hw/lc/native_lc.h"
#include "hw/mi/native_mi.h"
#include "hw/pi/native_pi.h"
#include "hw/pe/native_pe.h"
#include "hw/si/native_si.h"
#include "hw/vi/native_vi.h"
#include "native/address_space.h"
#include "native/page_table.h"
#include "native/boot.h"
#include "native/dol_loader.h"
#include "native/module_loader.h"
#include "platform/input.h"
#include "os/native_os.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace GekkoAOT::Native {

enum class RunStatus {
  Completed = 0,
  DispatchLimitReached,
  GuestHalted,
  UncompiledAddress,
  InstructionFallback,
  UnhandledMmioRead,
  UnhandledMmioWrite,
  UnhandledSprRead,
  UnhandledSprWrite,
  NativeHleFailure,
  HostRequestedExit,
  ModuleNotLoaded,
  InvalidGuestMemoryRead,
  InvalidGuestMemoryWrite,
};

const char* RunStatusName(RunStatus status);

struct RunResult {
  RunStatus status = RunStatus::ModuleNotLoaded;
  std::uint32_t program_counter = 0;
  std::uint32_t fault_address = 0;
  std::uint32_t fault_value = 0;
  std::uint64_t dispatches = 0;
  std::uint64_t hle_calls = 0;
  std::uint64_t cycles = 0;
  std::uint64_t secondary_dispatches = 0;
  std::uint64_t rel_mapping_refreshes = 0;
  std::uint64_t rel_address_translations = 0;
  std::uint64_t rel_address_misses = 0;
  std::uint32_t rel_last_canonical = 0;
  std::uint32_t rel_last_runtime = 0;
  std::uint32_t link_register = 0;
  std::uint32_t msr = 0;
  std::uint32_t srr0 = 0;
  std::uint32_t srr1 = 0;
  std::uint32_t exception = 0;
  std::uint32_t pi_cause = 0;
  std::uint32_t pi_mask = 0;
  std::uint64_t timebase = 0;
  std::uint64_t di_reads = 0;
  std::uint64_t di_read_bytes = 0;
  std::uint64_t di_read_failures = 0;
  std::uint32_t di_last_opcode = 0;
  std::uint32_t di_command0 = 0;
  std::uint32_t di_status = 0;
  std::uint32_t di_control = 0;
  std::uint32_t di_last_error = 0;
  std::uint32_t decrementer = 0;
  bool decrementer_pending = false;
  std::uint32_t last_mmio_read = 0;
  std::uint64_t mmio_reads = 0;
  std::uint64_t same_mmio_read_streak = 0;
  std::uint16_t dsp_control = 0;
  std::uint32_t dsp_mailbox = 0;
  bool dsp_mail_pending = false;
  bool dsp_bootstrap_loaded = false;
  bool dsp_bootstrap_running = false;
  bool dsp_bootstrap_completed = false;
  std::uint8_t dsp_task_loader_stage = 0;
  bool dsp_task_init_pending = false;
  std::uint64_t dsp_task_init_count = 0;
  std::uint32_t dsp_cpu_mail = 0;
  std::uint64_t dsp_cpu_mails = 0;
  bool dsp_native_core = false;
  bool dsp_rom_loaded = false;
  std::uint16_t dsp_native_pc = 0;
  std::uint16_t dsp_native_last_opcode = 0;
  std::uint64_t dsp_native_instructions = 0;
  std::uint64_t dsp_audio_frames = 0;
  std::uint32_t stack_pointer = 0;
  std::uint32_t halt_stack_word_08 = 0;
  std::uint32_t halt_stack_word_0c = 0;
  std::uint32_t halt_stack_word_10 = 0;
  std::uint32_t halt_stack_word_94 = 0;
  std::string halt_text_08;
  std::string halt_text_10;
};

enum class SimpleHleKind : std::uint8_t {
  OsGetTime,
  OsGetTick,
  Memcpy,
  Memmove,
  Memset,
  Memcmp,
  Bzero,
};

class HostRuntime final {
public:
  explicit HostRuntime(bool enable_mem2 = false);
  ~HostRuntime();

  ModuleStatus LoadModule(const std::filesystem::path& path,
                          const char* expected_game_id = nullptr);
  ModuleStatus LoadSecondaryModule(const std::filesystem::path& path,
                                   const char* expected_game_id = nullptr);
  std::size_t SecondaryModuleCount() const { return secondary_modules_.size(); }
  const std::string& SecondaryModuleLastError() const { return secondary_module_last_error_; }
  GameCubeBootResult InitializeGameCube(const std::filesystem::path& boot_bin,
                                         const GameCubeBootConfig& config = {});
  GameCubeBootResult InitializeGameCube(const std::vector<std::uint8_t>& boot_bin,
                                         const GameCubeBootConfig& config = {});
  GameCubeFstResult LoadGameCubeFst(const std::filesystem::path& fst_bin);
  GameCubeFstResult LoadGameCubeFst(const std::vector<std::uint8_t>& fst);
  GameCubeBi2Result LoadGameCubeBi2(const std::filesystem::path& bi2_bin);
  GameCubeBi2Result LoadGameCubeBi2(const std::vector<std::uint8_t>& bi2);
  DolLoadResult LoadDolImage(const std::filesystem::path& path);
  DolLoadResult LoadDolImage(const std::vector<std::uint8_t>& image);
  const std::vector<GekkoAOTRange>& LoadedDolCodeRanges() const { return loaded_dol_code_ranges_; }
  std::uint32_t LoadedDolEntryPoint() const { return loaded_dol_entry_point_; }
  void Reset();

  void RegisterOsHook(GekkoAOT::NativeOS::Kind kind, std::uint32_t address);
  void RegisterSimpleHle(SimpleHleKind kind, std::uint32_t address);

  bool InitializeNativeGX(const std::filesystem::path& bridge_path);
  void ShutdownNativeGX();
  bool AttachDiscImage(const std::filesystem::path& image_path);
  const std::string& DiscMediaBackend() const { return disc_media_backend_; }
  const std::string& DiscMediaFormat() const { return disc_media_format_; }
  bool NativeGXReady() const { return native_gx_.Ready(); }
  const std::string& NativeGXLastError() const { return native_gx_.LastError(); }

  RunResult Run(std::uint64_t dispatch_limit = 0);

  CPUState& Cpu() { return cpu_; }
  const CPUState& Cpu() const { return cpu_; }
  AddressSpace& Memory() { return memory_; }
  const AddressSpace& Memory() const { return memory_; }
  const GekkoAOTModuleDesc* Module() const { return module_.Descriptor(); }
  const ModuleLibrary& ModuleLibraryHandle() const { return module_; }
  const GekkoAOT::HW::CP::NativeCP& CommandProcessor() const { return cp_; }
  const GekkoAOT::HW::PI::NativePI& ProcessorInterface() const { return pi_; }
  const GekkoAOT::HW::PE::NativePE& PixelEngine() const { return pe_; }
  const GekkoAOT::HW::MI::NativeMI& MemoryInterface() const { return mi_; }
  const GekkoAOT::HW::LC::NativeLC& LockedCache() const { return lc_; }
  const GekkoAOT::HW::AI::NativeAI& AudioInterface() const { return ai_; }
  const GekkoAOT::HW::DI::NativeDI& DiscInterface() const { return di_; }
  const GekkoAOT::HW::EXI::NativeEXI& ExpansionInterface() const { return exi_; }
  const GekkoAOT::HW::SI::NativeSI& SerialInterface() const { return si_; }
  const GekkoAOT::HW::VI::NativeVI& VideoInterface() const { return vi_; }
  const GekkoAOT::DSP::NativeDSP& DspInterface() const { return dsp_; }

private:
  friend struct HardwareTestAccess;
  enum class FaultKind : std::uint8_t {
    None,
    InstructionFallback,
    MmioRead,
    MmioWrite,
    SprRead,
    SprWrite,
    HleFailure,
    InvalidMemoryRead,
    InvalidMemoryWrite,
  };

  struct Fault {
    FaultKind kind = FaultKind::None;
    std::uint32_t address = 0;
    std::uint32_t value = 0;
  };

  enum class CompatDispatchKind : std::uint8_t {
    Main,
    Secondary,
    LowMemory,
    NativeHle,
    Idle,
  };

  struct CompatBreadcrumb {
    std::uint64_t sequence = 0;
    std::uint32_t pc = 0;
    std::uint32_t lr = 0;
    std::uint32_t sp = 0;
    std::uint32_t instruction = 0;
    CompatDispatchKind kind = CompatDispatchKind::Main;
  };

  struct ActiveRelSection {
    const GekkoAOTModuleDesc* owner = nullptr;
    std::uint32_t module_id = 0;
    std::uint32_t section_index = 0;
    std::uint32_t linked_start = 0;
    std::uint32_t runtime_start = 0;
    std::uint32_t size = 0;
  };

  static HostRuntime* Self(CPUState* cpu);
  static std::uint64_t ExternalRead(CPUState* cpu, std::uint32_t address, std::uint8_t size);
  static void ExternalWrite(CPUState* cpu, std::uint32_t address, std::uint64_t value,
                            std::uint8_t size);
  static std::uint32_t ExternalRead32(CPUState* cpu, std::uint32_t address, std::uint8_t region);
  static void ExternalWrite32(CPUState* cpu, std::uint32_t address, std::uint32_t value,
                              std::uint8_t region);
  static void* ExternalPointer(CPUState* cpu, std::uint32_t address, std::uint32_t size);
  PageTranslation TranslateVmemAddress(std::uint32_t address, bool write);
  bool AccessPagedVmem(std::uint32_t address, std::uint64_t* value,
                       std::uint8_t size, bool write);
  static void InstructionFallback(CPUState* cpu, std::uint32_t instruction,
                                  std::uint32_t address);
  static bool HostCall(CPUState* cpu, std::uint32_t address);
  static GekkoAOT::HW::DI::NativeDI::CommandResponse DiscCommand(
      void* user, const GekkoAOT::HW::DI::NativeDI::CommandRequest& request);
  static bool NativePadPoll(void* user, std::uint32_t channel,
                            std::uint32_t* hi, std::uint32_t* lo);
  static int NativePadBuffer(void* user, std::uint32_t channel, std::uint8_t* buffer,
                             std::uint32_t request_length,
                             std::uint32_t expected_response_length);
  static void NativePadDirect(void* user, std::uint32_t channel,
                              std::uint32_t command, bool polling_enabled);
  static std::uint32_t SprRead(CPUState* cpu, std::uint16_t spr, std::uint32_t address);
  static void SprWrite(CPUState* cpu, std::uint16_t spr, std::uint32_t value,
                       std::uint32_t address);
  static void CacheControl(CPUState* cpu, std::uint8_t operation, std::uint32_t address,
                           std::uint32_t instruction_address);
  static void NativeGXPeEvent(void* user, std::uint32_t reg, std::uint32_t value);

  void WireCpuCallbacks();
  bool DispatchHostCall(std::uint32_t address);
  bool DispatchGlobalIndirect(std::uint32_t runtime_address);
  bool DispatchOsHle(GekkoAOT::NativeOS::Kind kind, std::uint32_t address);
  bool DispatchSimpleHle(SimpleHleKind kind, std::uint32_t address);
  void RebuildNativeInterceptIndex();
  void RefreshSecondaryRelMappings();
  bool ResolveSecondaryAddress(std::uint32_t runtime_address,
                               const GekkoAOTModuleDesc** descriptor,
                               std::uint32_t* linked_address);
  const GekkoAOTModuleDesc* SelectFixedExecutableModule(std::uint32_t runtime_address,
                                                        bool* overlap);
  bool DispatchFixedExecutableModule(const GekkoAOTModuleDesc* descriptor,
                                     std::uint32_t runtime_address, RunResult& result);
  std::uint32_t TranslateSecondaryAddress(const GekkoAOTModuleDesc* descriptor,
                                          std::uint32_t linked_address) const;
  bool TranslateCanonicalRelAddress(std::uint32_t canonical_address,
                                    std::uint32_t* runtime_address) const;
  bool DispatchSecondaryModule(std::uint32_t runtime_address, RunResult& result);
  void DetachDiscImage();
  bool ReadDiscImage(std::uint64_t offset, void* destination, std::uint32_t size);
  void SyncNativeGXInterrupts();
  void SyncNativeCPInterrupt();
  void SyncNativePEInterrupts();
  void SyncNativeDSPInterrupt();
  void SyncNativeAIInterrupt();
  void SyncNativeDIInterrupt();
  void SyncNativeEXIInterrupt();
  void SyncNativeSIInterrupt();
  void SyncNativeVIInterrupt();
  void RefreshNativeInput(bool poll_si);
  bool TryTakeExternalInterrupt();
  bool TryTakeDecrementerInterrupt();
  void AdvanceCpuTimers(std::uint64_t cpu_cycles);
  void AdvancePerformanceCounters(std::uint64_t cpu_cycles, std::uint64_t tb_ticks);
  void RebuildPpcHaltIndex();
  bool DetectSdkPpcHalt(std::uint32_t pc, std::uint32_t* entry) const;
  bool ExecuteLowMemoryInstruction(std::uint32_t address, std::uint64_t* charged_cycles);
  void AdvanceRuntimeCycles(std::uint64_t charged, RunResult& result, bool idle_wait = false);
  void PollLiveVideoConfig(std::uint64_t charged);
  std::uint64_t TakeRealtimeHardwareCycles(std::uint64_t charged, bool force_sample);
  void QueueHostPresent(std::uint32_t xfb_top, std::uint32_t xfb_bottom,
                        std::uint32_t width = 0u, std::uint32_t stride_bytes = 0u,
                        std::uint32_t field_height = 0u);
  void NoteProducedFrame();
  void ServiceHostFrameScheduler();
  void ServicePerformanceMetrics(std::uint64_t hardware_cycles);
  bool CpuFifoTargetsGpu() const;
  bool CaptureWriteGatherToCpuFifo(std::uint64_t value, std::uint8_t size);
  bool QueueGpuFifoGather(std::uint64_t value, std::uint8_t size, std::uint32_t guest_pc);
  static bool ConsumeGpuFifoBurst(void* user, std::uint32_t address);
  RunStatus FaultStatus() const;
  void SetFault(FaultKind kind, std::uint32_t address, std::uint32_t value = 0);
  void RecordCompatBreadcrumb(std::uint32_t pc, CompatDispatchKind kind);
  void ReportCompatFailure(const char* reason, std::uint32_t address,
                           std::uint32_t value = 0, bool break_into_debugger = true);
  void DumpCompatHistory(const char* reason, std::uint32_t address,
                         std::uint32_t value) const;
  static const char* CompatDispatchKindName(CompatDispatchKind kind);
  void CompatDebuggerBreak();

  AddressSpace memory_;
  std::vector<GekkoAOTRange> loaded_dol_code_ranges_;
  std::uint32_t loaded_dol_entry_point_ = 0;
  CPUState cpu_{};
  ModuleLibrary module_;
  std::vector<std::unique_ptr<ModuleLibrary>> secondary_modules_;
  std::vector<ActiveRelSection> active_rel_sections_;
  std::string secondary_module_last_error_;
  std::uint64_t secondary_dispatches_ = 0;
  struct FixedExecutableRouteCache {
    const GekkoAOTModuleDesc* descriptor = nullptr;
    std::uint32_t chunk_start = 0;
    std::uint32_t chunk_end = 0;
    bool valid = false;
  };
  FixedExecutableRouteCache fixed_exec_route_cache_{};
  const GekkoAOTModuleDesc* fixed_exec_active_descriptor_ = nullptr;
  std::uint64_t fixed_exec_route_switches_ = 0;
  std::uint64_t global_indirect_dispatches_ = 0;
  std::uint64_t icbi_route_invalidations_ = 0;
  std::uint64_t rel_mapping_refreshes_ = 0;
  std::uint64_t rel_address_translations_ = 0;
  std::uint64_t rel_address_misses_ = 0;
  std::uint32_t rel_last_canonical_ = 0;
  std::uint32_t rel_last_runtime_ = 0;
  Fault fault_{};
  bool compat_diagnostics_enabled_ = false;
  // v79: correctness-first host boundary mode. This reproduces the useful
  // execution-boundary side effect of the GDB compatibility path without
  // enabling debugger traps or diagnostics. Retail titles that hand work
  // between CPU/VI/GX/DI at tight boundaries must return to the host before a
  // large native super-chain hides those events.
  bool safe_host_boundaries_enabled_ = false;
  bool compat_break_on_fault_ = false;
  bool compat_break_fired_ = false;
  static constexpr std::size_t kCompatBreadcrumbCount = 64u;
  std::array<CompatBreadcrumb, kCompatBreadcrumbCount> compat_breadcrumbs_{};
  std::uint64_t compat_breadcrumb_sequence_ = 0;
  std::size_t compat_breadcrumb_next_ = 0;
  std::unordered_map<std::uint32_t, GekkoAOT::NativeOS::Kind> os_hooks_;
  std::unordered_map<std::uint32_t, SimpleHleKind> simple_hle_hooks_;
  enum class HleFastType : std::uint8_t { Empty, Os, Simple };
  struct HleFastEntry {
    std::uint32_t address = 0;
    HleFastType type = HleFastType::Empty;
    std::uint8_t kind = 0;
  };
  static constexpr std::size_t kHleFastCacheSize = 2048u;
  std::array<HleFastEntry, kHleFastCacheSize> hle_fast_cache_{};
  // Sorted exact entrypoint index shared by the runtime's HLE dispatcher and
  // DolRecomp's rare interception query. Static-intercept specialization removes
  // the query from ordinary chunks at compile time; ranges that can actually
  // contain an HLE entry use one lower_bound here instead of a runtime cache.
  std::vector<std::uint32_t> native_intercepts_;
  // Generated code probes the host at many ordinary guest function entries.
  // Almost all probes are misses. A small direct-mapped negative cache keeps
  // those misses out of unordered_map, while real HLE entries remain exact.
  static constexpr std::size_t kHostCallNegativeCacheSize = 4096u;
  std::array<std::uint32_t, kHostCallNegativeCacheSize> hostcall_negative_cache_{};
  struct PpcHaltPc { std::uint32_t pc = 0; std::uint32_t entry = 0; };
  std::vector<PpcHaltPc> ppc_halt_pcs_;
  std::array<std::uint32_t, 1024> spr_state_{};
  std::uint32_t timebase_cycle_remainder_ = 0;
  std::uint32_t decrementer_cycle_remainder_ = 0;
  bool decrementer_pending_ = false;
  // AI/DSP are the only standalone devices here whose IRQ state can change
  // solely because guest time advanced. Poll them in tiny cycle quanta rather
  // than after every compiled slice; MMIO-triggered IRQs are still synchronized
  // immediately in ExternalRead/ExternalWrite.
  std::uint64_t timed_irq_sync_cycles_ = 0;
  // VI remains the guest-visible timing/IRQ authority. Presentation may be
  // decoupled and driven by completed GX display copies, with VI retained as a
  // compatibility watchdog/fallback.
  bool gx_present_on_copy_ = false;
  bool gx_present_fallback_vi_ = true;
  bool gx_frame_ready_since_vi_ = false;
  // v46 PC-style FPS target. Only the VI wait/retrace gate is virtualized;
  // CPU timebase/decrementer, DSP and AI remain at the real GameCube rate.
  // v47: execution is uncapped AOT. Hardware-visible clocks advance from the
  // host monotonic clock instead of from how quickly the host executes PPC.
  // This preserves 1.0x VI/TB/DEC/DSP/AI time without sleeping the AOT thread.
  bool hardware_clock_started_ = false;
  std::chrono::steady_clock::time_point hardware_clock_last_{};
  std::uint64_t hardware_clock_remainder_ = 0;
  // v65: sampling steady_clock at every AOT/HLE boundary dominated profiles.
  // Accumulate guest work and sample wall time in small sub-millisecond quanta;
  // the sampled delta still spans the full elapsed host interval, so hardware
  // time does not drift or derive from guest execution speed.
  std::uint64_t hardware_clock_guest_since_sample_ = 0;

  // Presentation is a separate host domain. The slider caps completed GX
  // frames only; it never changes VI, CPU, DSP or AI timing.
  std::uint32_t host_fps_limit_hz_ = 120;
  bool frame_interpolation_enabled_ = true;
  bool produced_frame_seen_ = false;
  bool interpolation_pair_valid_ = false;
  std::chrono::steady_clock::time_point produced_frame_last_{};
  std::chrono::steady_clock::time_point interpolation_pair_start_{};
  std::chrono::nanoseconds produced_frame_period_{16666667};
  std::uint64_t interpolated_present_count_ = 0;
  std::uint64_t direct_present_count_ = 0;
  bool host_present_pending_ = false;
  std::uint32_t host_present_top_ = 0;
  std::uint32_t host_present_bottom_ = 0;
  std::uint32_t host_present_width_ = 0;
  std::uint32_t host_present_stride_bytes_ = 0;
  std::uint32_t host_present_field_height_ = 0;
  bool host_present_started_ = false;
  std::chrono::steady_clock::time_point host_present_next_{};
  std::uint64_t host_present_count_ = 0;
  std::uint64_t host_present_dropped_ = 0;

  // v49 observability: VI cadence, unique GX frames, host presents and the
  // hardware clock are separate metrics. This avoids confusing accelerated
  // simulation with actual renderer throughput.
  bool perf_metrics_enabled_ = true;
  bool perf_metrics_started_ = false;
  std::chrono::steady_clock::time_point perf_metrics_last_{};
  std::uint64_t perf_hardware_cycles_total_ = 0;
  std::uint64_t perf_vi_boundaries_total_ = 0;
  std::uint64_t perf_guest_frames_total_ = 0;
  std::uint64_t perf_last_hardware_cycles_ = 0;
  std::uint64_t perf_last_vi_boundaries_ = 0;
  std::uint64_t perf_last_guest_frames_ = 0;
  std::uint64_t perf_last_presents_ = 0;
  std::uint64_t perf_last_interpolated_ = 0;
  std::uint64_t perf_last_dropped_ = 0;
  std::uint64_t perf_metrics_poll_cycles_ = 0;

  std::filesystem::path live_video_config_path_;
  std::filesystem::file_time_type live_video_config_mtime_{};
  bool live_video_config_mtime_valid_ = false;
  std::uint64_t live_video_config_poll_cycles_ = 0;
  GekkoAOT::HW::LC::NativeLC lc_;
  GekkoAOT::HW::CP::NativeCP cp_;
  GekkoAOT::HW::PE::NativePE pe_;
  GekkoAOT::HW::PI::NativePI pi_;
  GekkoAOT::HW::MI::NativeMI mi_;
  GekkoAOT::HW::AI::NativeAI ai_;
  GekkoAOT::HW::DI::NativeDI di_;
  std::ifstream disc_image_;
  void* nod_disc_image_ = nullptr;
  bool disc_image_uses_nod_ = false;
  std::filesystem::path disc_image_path_;
  std::uint64_t disc_image_size_ = 0;
  std::string disc_media_backend_ = "none";
  std::string disc_media_format_ = "unknown";
  std::uint64_t di_reads_ = 0;
  std::uint64_t di_read_bytes_ = 0;
  std::uint64_t di_read_failures_ = 0;
  // v58: preserve DVDReadAsyncPrio ordering. Host media reads may finish
  // immediately, but the guest-visible DI command remains busy until a later
  // hardware-time slice raises TCINT. This matches the SDK's asynchronous
  // callback contract and avoids re-entering streaming state machines from the
  // same MMIO transaction that started the read.
  bool di_async_completion_pending_ = false;
  std::uint64_t di_async_cycles_remaining_ = 0;
  std::uint32_t di_async_bytes_ = 0;
  std::uint32_t di_async_dma_address_ = 0;
  std::uint64_t di_async_disc_offset_ = 0;
  std::uint64_t di_async_sequence_ = 0;
  GekkoAOT::HW::EXI::NativeEXI exi_;
  GekkoAOT::HW::SI::NativeSI si_;
  std::array<GekkoAOT::Input::Pad, 4> native_pads_{};
  std::array<std::uint8_t, 4> native_pad_mode_{{3u, 3u, 3u, 3u}};
  bool native_input_ready_ = false;
  GekkoAOT::HW::VI::NativeVI vi_;
  GekkoAOT::DSP::NativeDSP dsp_;
  std::uint64_t hle_calls_ = 0;
  std::uint32_t last_mmio_read_ = 0;
  std::uint64_t mmio_reads_ = 0;
  std::uint64_t same_mmio_read_streak_ = 0;
  std::array<std::uint8_t, 32> cpu_fifo_gather_{};
  std::uint32_t cpu_fifo_gather_size_ = 0;
  std::uint32_t cpu_fifo_gather_base_ = 0;
  std::uint32_t cpu_fifo_gather_end_ = 0;
  std::uint64_t cpu_fifo_capture_bursts_ = 0;
  bool cpu_fifo_capture_logged_ = false;
  std::array<std::uint8_t, 32> gpu_fifo_gather_{};
  std::uint32_t gpu_fifo_gather_size_ = 0;
  std::uint32_t gpu_fifo_first_pc_ = 0;
  std::uint64_t gpu_fifo_bursts_ = 0;
  bool gpu_fifo_burst_logged_ = false;
  GekkoAOT::GX::HostBridge native_gx_;
};

} // namespace GekkoAOT::Native
