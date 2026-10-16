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
#include "native/guest_write_tracker.h"
#include "native/page_table.h"
#include "native/boot.h"
#include "native/dol_loader.h"
#include "native/module_loader.h"
#include "native/low_vector_native.h"
#include "native/native_read_poll.h"
#include "native/native_counter_poll.h"
#include "native/native_spin_probe.h"
#include "native/native_vfs.h"
#include "NativeHW/native_hw.h"
#include "NativeHW/EXI/native_exi.h"
#include "platform/input.h"
#include "os/native_os.h"

#include <array>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
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
  std::uint64_t idle_event_waits = 0;
  std::uint64_t idle_event_requested_cycles = 0;
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
  std::array<std::uint32_t, 8> halt_frame_sp{};
  std::array<std::uint32_t, 8> halt_frame_caller_lr{};
  std::uint8_t halt_frame_count = 0;
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
  Strlen,
  Strcmp,
  Strncmp,
  Strcpy,
  Strncpy,
  Strcat,
  Strchr,
  Strrchr,
  Memchr,
  Strstr,
  Strncat,
  Strnlen,
  Strcspn,
  Strpbrk,
  DcFlushRange,
  DcFlushRangeNoSync,
  DcStoreRange,
  DcStoreRangeNoSync,
  DcInvalidateRange,
  DcZeroRange,
  DcTouchRange,
  IcInvalidateRange,
  GxLoadPosMtxImm,
  GxLoadTexMtxImm,
  // v300 native PAD service (appended: existing static tokens keep their value).
  PadRead,
  DvdFastOpen,
  GxLoadNrmMtxImm,
  GxLoadNrmMtxImm3x3,
  GxSetArray,
  Count,
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
  static const GekkoAOTNativeHardware& NativeHardwareServices();
  static const GekkoAOTNativeWgpipeTry& NativeWgpipeTryServices();
  static const GekkoAOTNativeStableMmio& NativeStableMmioServices();
  static const GekkoAOTNativeAddressWindow& NativeAddressWindowServices(bool published = false);
  static std::uint8_t* NativeAddressBase(CPUState*);
  static std::uint8_t* PublishedNativeAddressBase(CPUState*);
  static const GekkoAOTNativeFlatVmem& NativeFlatVmemServices();
  static const GekkoAOTNativeLockedCache& NativeLockedCacheServices();
  static std::uint8_t* NativeLockedCacheBase(CPUState*);
  static const GekkoAOTFlatVmemView* NativeFlatVmemView(CPUState*);
  static std::uint64_t NativeTryStableMmioRead(CPUState*, std::uint32_t, std::uint8_t);
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
  static void ExternalWriteSlow(CPUState* cpu, std::uint32_t address, std::uint64_t value,
                                std::uint8_t size);
  static std::uint64_t NativeCPRead16(CPUState*, std::uint32_t, std::uint8_t);
  static std::uint64_t NativeVIRead16(CPUState*, std::uint32_t, std::uint8_t);
  static void NativePIWrite32(CPUState*, std::uint32_t, std::uint64_t, std::uint8_t);
  static void NativeWgpipeWrite(CPUState*, std::uint32_t, std::uint64_t, std::uint8_t);
  bool TryGatherPartial(std::uint64_t value, std::uint8_t size, std::uint32_t pc);
  static std::uint32_t NativeTryWgpipeWrite(CPUState*, std::uint32_t, std::uint64_t, std::uint8_t, std::uint32_t);
  template<bool ProvenWgpipe>
  static void WriteGatherOrExternal(CPUState*, std::uint32_t, std::uint64_t, std::uint8_t);
  void ServiceTimedMmioPoll(std::uint32_t address,bool counter=false);
  void RecordMmioRead(std::uint32_t address);
  static void ExternalWrite32(CPUState* cpu, std::uint32_t address, std::uint32_t value,
                              std::uint8_t region);
  static void* ExternalPointer(CPUState* cpu, std::uint32_t address, std::uint32_t size);
  // Scalar accesses only need status + physical address. PTE identity stays in
  // the DTLB/walker; a small result can travel in host registers instead of a
  // caller-provided 20-byte temporary on each VM access.
  struct DataAddressTranslation {
    PageTranslation::Status status = PageTranslation::Status::Missing;
    std::uint32_t physical = 0;
  };
  struct DataTlbEntry;
  DataAddressTranslation TranslateVmemAddress(std::uint32_t address, bool write);
  DataAddressTranslation TranslatePagedDataAddress(std::uint32_t address, bool write);
  DataAddressTranslation WalkPagedDataAddress(std::uint32_t address, bool write,
                                             DataTlbEntry* changed_entry = nullptr);
  void InvalidateDataTlb(std::uint32_t address);
  void ClearDataTlb();
  void RefreshVmemBatModes();
  bool AccessPagedVmem(std::uint32_t address, std::uint64_t* value,
                       std::uint8_t size, bool write);
  static void InstructionFallback(CPUState* cpu, std::uint32_t instruction,
                                  std::uint32_t address);
  static bool HostCall(CPUState* cpu, std::uint32_t address);
  static GekkoAOT::HW::DI::NativeDI::CommandResponse DiscCommand(
      void* user, const GekkoAOT::HW::DI::NativeDI::CommandRequest& request);
  static bool NativeExiSelect(void* user, std::uint32_t channel, std::uint8_t chip_select);
  static bool NativeExiPresent(void* user, std::uint32_t channel, std::uint8_t chip_select);
  static bool NativeExiImmediate(void* user, std::uint32_t channel,
                                 std::uint8_t chip_select, std::uint8_t direction,
                                 std::uint8_t length, std::uint32_t* data);
  static bool NativeExiDma(void* user, std::uint32_t channel,
                           std::uint8_t chip_select, std::uint8_t direction,
                           std::uint32_t guest_address, std::uint32_t length);
  static bool DiscAudioRead(void* user, std::uint64_t offset, void* destination,
                            std::uint32_t size);
  static void MixDiscAudio(void* user, std::int16_t* pcm, std::size_t frames,
                           std::uint32_t output_hz);
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
  void ServiceDiscCompletion(std::uint64_t cycles);
  bool DispatchHostCall(std::uint32_t address);
  bool DispatchHostCallImpl(std::uint32_t address);
  bool DispatchNativeRegionProbe();
  bool DispatchGlobalIndirect(std::uint32_t runtime_address);
  bool DispatchOsHle(GekkoAOT::NativeOS::Kind kind, std::uint32_t address);
  bool DispatchSimpleHle(SimpleHleKind kind, std::uint32_t address);
  // v300: host spans of a guest byte range for native helpers. MEM1/MEM2 and
  // other host-backed regions are contiguous views; the paged VM aperture is
  // split per 4 KiB page through the emulated DTLB.
  // v301: NotResident means a VM page is unmapped or protected. Nothing was
  // raised or modified: the service declines and the original guest code runs
  // this call, faulting page by page with resumable state. (Raising the DSI
  // from the service and retrying it needs every page resident at once, which
  // livelocks once the range exceeds the pager's working set.)
  enum class GuestRangeStatus : std::uint8_t { Ok, NotResident, Invalid };
  struct GuestSpan {
    std::uint8_t* host = nullptr;
    std::uint32_t length = 0u;
  };
  GuestRangeStatus TranslateGuestRange(std::uint32_t address, std::uint32_t size, bool write,
                                       std::vector<GuestSpan>& spans);
  std::array<std::vector<GuestSpan>, 4> hle_spans_{};
  std::vector<std::uint8_t> hle_scratch_{};
  bool DispatchCopiedCacheStub(std::uint32_t address, std::uint64_t* cycles);
  void InvalidateInstructionRange(std::uint32_t address, std::uint32_t size);
  void RebuildNativeInterceptIndex();
  void RefreshSecondaryRelMappings();
  bool ResolveSecondaryAddress(std::uint32_t runtime_address,
                               const GekkoAOTModuleDesc** descriptor,
                               std::uint32_t* linked_address);
  const GekkoAOTModuleDesc* SelectFixedExecutableModule(std::uint32_t runtime_address,
                                                        bool* overlap);
  const GekkoAOTModuleDesc* SelectFixedExecutableModuleSlow(std::uint32_t runtime_address,
                                                            bool* overlap);
  bool DispatchFixedExecutableModule(const GekkoAOTModuleDesc* descriptor,
                                     std::uint32_t runtime_address, RunResult& result);
  std::uint32_t TranslateSecondaryAddress(const GekkoAOTModuleDesc* descriptor,
                                          std::uint32_t linked_address) const;
  void PublishSecondaryControlState(const GekkoAOTModuleDesc* descriptor);
  bool TranslateCanonicalRelAddress(std::uint32_t canonical_address,
                                    std::uint32_t* runtime_address) const;
  bool DispatchSecondaryModule(std::uint32_t runtime_address, RunResult& result);
  std::uint32_t SecondaryModuleCapabilities(const GekkoAOTModuleDesc* descriptor);
  void DetachDiscImage();
  bool ReadDiscImage(std::uint64_t offset, void* destination, std::uint32_t size);
  void QuiesceDiscIo();
  void SyncNativeGXInterrupts(bool polling_token = false);
  void SyncNativeCPInterrupt();
  void SyncNativePEInterrupts();
  void SyncNativeDSPInterrupt();
  void SyncNativeAIInterrupt();
  void SyncNativeDIInterrupt();
  void SyncNativeEXIInterrupt();
  void SyncNativeSIInterrupt();
  void SyncNativeVIInterrupt();
  void RefreshNativeInput(bool poll_si);
  void ServiceIdleCpBreakpointRecovery(bool frame_boundary);
  bool TryTakeExternalInterrupt();
  bool TryTakeDecrementerInterrupt();
  void AdvanceCpuTimers(std::uint64_t cpu_cycles);
  void AdvancePerformanceCounters(std::uint64_t cpu_cycles, std::uint64_t tb_ticks);
  void RebuildPpcHaltIndex();
  bool DetectSdkPpcHalt(std::uint32_t pc, std::uint32_t* entry) const;
  bool FetchLowMemoryInstruction(std::uint32_t address, std::uint32_t* instruction);
  void InvalidateLowInstructionCacheLine(std::uint32_t address);
  bool IcbiLineTouchesFixedImages(std::uint32_t address);
  std::uint32_t TryExecuteLeafTail(std::uint32_t pc, bool execute);
  bool EeEnableNeedsBoundary();
  void ObserveIdleLoop(RunResult& result);
  void RestartIdleLoop();
  void FailIdleLoopProof();
  void CaptureIdleRegs();
  bool IdleRegsMatch() const;
  void SnapshotIdlePages();
  void InvalidateDispatchProbesForRange(std::uint32_t address, std::uint32_t size);
  void InvalidateLowInstructionCache();
  bool ExecuteLowMemoryInstruction(std::uint32_t address, std::uint64_t* charged_cycles);
  bool ExecuteNativeLowMemoryVector(std::uint32_t address, std::uint64_t* charged_cycles);
  void AdvanceRuntimeCycles(std::uint64_t charged, RunResult& result, bool idle_wait = false);
  // v276: per-boundary cycle accounting. Between two host-clock samples
  // (TakeRealtimeHardwareCycles samples once per kGuestPollQuantum charged
  // cycles) AdvanceRuntimeCycles only accumulates: no device, timer, frame or
  // metrics work can run. Do that accumulation inline at the AOT boundary and
  // call the full scheduler only when a sample is due or the async GX worker
  // has a PE event to commit -- exactly the conditions under which the full
  // path does anything beyond the same two additions.
  void AdvanceRuntimeCyclesFast(std::uint64_t charged, RunResult& result) {
    constexpr std::uint64_t kGuestPollQuantum =
        (GekkoAOT::HW::VI::NativeVI::CpuClockHz / 8000u) + 1u;
    if (hardware_clock_started_ && !runtime_profile_enabled_ &&
        charged < kGuestPollQuantum &&
        hardware_clock_guest_since_sample_ + charged < kGuestPollQuantum &&
        result.cycles <= ~std::uint64_t{0} - charged &&
        !(runtime_parallel_workers_ && native_gx_.PeEventPending())) {
      result.cycles += charged;
      hardware_clock_guest_since_sample_ += charged;
      hardware_clock_sampled_last_call_ = false;
      return;
    }
    AdvanceRuntimeCycles(charged, result);
  }
  std::uint64_t IdleWakeDeadlineCycles(bool include_decrementer=true) const;
  void WaitForIdleEvent(RunResult& result,std::uint64_t maximum_cycles=~std::uint64_t{0},bool masked_timer=false,
                        bool hold_decrementer=false);
  void ConsumeAotPollPark(RunResult& result);
  void ParkSdkIdlePoll(RunResult& result);
  void ParkNativeReadPoll(RunResult& result);
  void ParkNativeCounterPoll(RunResult& result);
  void ParkClockSpin(RunResult& result);
  void ParkYieldSpin();
  void ParkFixedPointPoll(RunResult& result);
  void NoteMmioPoll(std::uint32_t address, std::uint64_t value);
  // v333: optional guest execution rate cap (see ThrottleGuestRate). Guest
  // time is wall time, so this is a slower host CPU from the guest's view.
  void ThrottleGuestRate(RunResult& result);
  std::uint64_t irq_external_taken_ = 0, irq_decrementer_taken_ = 0;
  std::uint64_t irq_report_external_ = 0, irq_report_decrementer_ = 0;
  std::uint32_t irq_external_causes_ = 0;
  std::array<std::uint64_t, 16> irq_cause_counts_{};
  std::uint32_t guest_rate_ceiling_pct_ = 0u;
  bool rate_started_ = false;
  double rate_tokens_ = 0.0;
  std::uint64_t rate_charged_ = 0, rate_mark_charged_ = 0;
  std::uint64_t rate_throttles_ = 0, rate_throttle_ns_ = 0;
  std::uint64_t rate_report_throttles_ = 0, rate_report_ns_ = 0;
  std::chrono::steady_clock::time_point rate_mark_time_{};
  void NoteClockCall(bool wide) {
    const auto lr = cpu_.lr;
    const auto tb = cpu_.timebase;
    // Clock reads microseconds apart, from one or several sites of a loop.
    clock_spin_streak_ = tb - clock_spin_last_tb_ < 400u ? clock_spin_streak_ + 1u : 0u;
    clock_spin_last_lr_ = lr; clock_spin_last_tb_ = tb;
    time_poll_entry_ = cpu_.pc; time_poll_return_ = lr; time_poll_wide_ = wide;
  }
  void PollLiveVideoConfig(std::uint64_t charged);
  void ServiceWidescreenFrustumAssist(bool frame_boundary);
  std::uint64_t TakeRealtimeHardwareCycles(std::uint64_t charged, bool force_sample);
  void QueueHostPresent(std::uint32_t xfb_top, std::uint32_t xfb_bottom,
                        std::uint32_t width = 0u, std::uint32_t stride_bytes = 0u,
                        std::uint32_t field_height = 0u);
  void NoteProducedFrame();
  void ServiceHostFrameScheduler(std::chrono::steady_clock::time_point now);
  void ServicePerformanceMetrics(std::uint64_t hardware_cycles);
  bool CpuFifoTargetsGpu() const;
  bool CaptureWriteGatherToCpuFifo(std::uint64_t value, std::uint8_t size);
  bool QueueGpuFifoGather(std::uint64_t value, std::uint8_t size, std::uint32_t guest_pc);
  bool QueueGpuFifoBytes(const std::uint8_t* bytes,std::size_t size,std::uint32_t guest_pc);
  bool QueueSdkFifoPacket(const std::uint8_t* bytes,std::size_t size);
  static bool ConsumeGpuFifoBurst(void* user, std::uint32_t address);
  static bool ConsumeGpuFifoBurstBatch(void* user, std::uint32_t address,
                                       std::uint32_t burst_count);
  RunStatus FaultStatus() const;
  void SetFault(FaultKind kind, std::uint32_t address, std::uint32_t value = 0);
  void RecordCompatBreadcrumb(std::uint32_t pc, CompatDispatchKind kind);
  void ReportCompatFailure(const char* reason, std::uint32_t address,
                           std::uint32_t value = 0, bool break_into_debugger = true);
  void DumpCompatHistory(const char* reason, std::uint32_t address,
                         std::uint32_t value) const;
  static const char* CompatDispatchKindName(CompatDispatchKind kind);
  void CompatDebuggerBreak();

  // v298: publish the immutable address-window decision beside the runtime
  // owner, avoiding a remote policy load/branch at every native body entry.
  // Reset republishes it; no CPU/module ABI or guest memory rules change.
  std::uint8_t* published_native_address_base_ = nullptr;
  bool native_flat_vmem_enabled_ = true;
  bool native_locked_cache_enabled_ = true;
  AddressSpace memory_;
  std::vector<GekkoAOTRange> loaded_dol_code_ranges_;
  std::uint32_t loaded_dol_entry_point_ = 0;
  CPUState cpu_{};
  ModuleLibrary module_;
  std::vector<std::unique_ptr<ModuleLibrary>> secondary_modules_;
  std::unordered_map<const GekkoAOTModuleDesc*, std::uint32_t> secondary_module_caps_;
  const GekkoAOTModuleDesc* cached_secondary_caps_owner_ = nullptr;
  std::uint32_t cached_secondary_caps_ = 0;
  bool has_fixed_secondary_modules_ = false;
  std::vector<ActiveRelSection> active_rel_sections_;
  struct RelCodeRoute {
    const GekkoAOTModuleDesc* owner = nullptr;
    std::uint32_t runtime_start = 0;
    std::uint32_t runtime_end = 0;
    std::uint32_t linked_start = 0;
    bool exclusive_from_main = false;
  };
  RelCodeRoute cached_rel_route_{};
  // v277: secondary dispatch scratch is CPU-instance state. Keeping these in
  // HostRuntime removes ELF TLS lookup (__tls_get_addr) from every fixed/REL
  // dispatch while retaining exact nested save/restore semantics.
  bool native_region_probe_capture_ = false;
  std::uint32_t native_region_last_blocked_start_ = 0u;
  std::uint32_t native_region_retry_start_ = 0u;
  std::uint64_t hle_guest_declines_ = 0u;
  std::uint32_t hle_decline_entry_ = 0u;  // v301: one-shot guest-body admission
  // v321: last paged-VM DSI raise, for stale-latch / non-recoverable reports.
  std::uint32_t last_dsi_pc_ = 0u, last_dsi_lr_ = 0u, last_dsi_msr_ = 0u;
  std::uint32_t nonrecoverable_dsi_reports_ = 0u;
  bool stale_dsi_reported_ = false;
  std::uint32_t dsi_trace_tag_[16]{}, dsi_trace_pc_[16]{}, dsi_trace_exc_[16]{};
  std::uint32_t dsi_trace_next_ = 0u;
  void NoteDsiTrace(std::uint32_t tag, std::uint32_t pc, std::uint32_t exc) {
    const auto i = dsi_trace_next_++ & 15u;
    dsi_trace_tag_[i] = tag; dsi_trace_pc_[i] = pc; dsi_trace_exc_[i] = exc;
  }
  const GekkoAOTModuleDesc* native_region_secondary_owner_ = nullptr;
  // REL discovery is an expensive full-MEM1 operation.  Treat the guest's
  // instruction-cache publication boundary as the invalidation epoch: OSLink
  // must publish relocated code before it can execute, so one scan per epoch
  // is sufficient and avoids rescanning 24 MiB on every secondary dispatch.
  bool rel_mapping_dirty_ = true;
  std::string secondary_module_last_error_;
  std::uint64_t secondary_dispatches_ = 0;
  struct FixedExecutableRouteCache {
    const GekkoAOTModuleDesc* descriptor = nullptr;
    std::uint32_t chunk_start = 0;
    std::uint32_t chunk_end = 0;
    bool valid = false;
  };
  FixedExecutableRouteCache fixed_exec_route_cache_{};
  // NativeHW v3 / router v175: keep several verified chunk routes at once.
  // MOH Frontline bounces between main + fixed ELF chunks every frame; the old
  // one-entry range cache repeatedly fell back into SelectFixedExecutableModule.
  struct FixedExecutablePageRoute {
    const GekkoAOTModuleDesc* descriptor = nullptr;
    std::uint32_t page_tag = 0;
    std::uint32_t chunk_start = 0;
    std::uint32_t chunk_end = 0;
    std::uint64_t epoch = 0;
  };
  static constexpr std::size_t kFixedExecPageRouteCacheSize = 8192u;
  static_assert((kFixedExecPageRouteCacheSize &
                 (kFixedExecPageRouteCacheSize - 1u)) == 0u);
  std::array<FixedExecutablePageRoute, kFixedExecPageRouteCacheSize>
      fixed_exec_page_routes_{};
  std::uint64_t fixed_exec_route_epoch_ = 1u;
  // v274: bumped only when chunk coverage or image admissibility can change
  // (module load, paging migration, reset); icbi bumps only the route epoch.
  std::uint64_t fixed_exec_structure_epoch_ = 1u;
  // v275: MEM1 pages (0x80000000-0x81ffffff) covered by a valid chunk of any
  // fixed secondary executable, rebuilt per structural epoch. A PC on an
  // uncovered page has at most one candidate image, so the router answers
  // "no overlap" without probing the PC-keyed caches or the resolver.
  // v276: MEM1 pages entirely covered by chunks (with build hashes) of the
  // verified active fixed executable. For such a PC the resolver's first step
  // (active image owns the chunk) is the answer, without FindModuleChunk or the
  // out-of-line resolver. Rebuilt whenever the active image or the structural
  // epoch changes (module load, paging migration, ICBI drop of the active image).
  const GekkoAOTModuleDesc* fixed_exec_active_pages_owner_ = nullptr;
  std::uint64_t fixed_exec_active_pages_epoch_ = 0u;
  std::array<std::uint64_t, 8192u / 64u> fixed_exec_active_pages_{};
  void RebuildFixedExecActivePages();
  std::uint64_t fixed_exec_secondary_pages_epoch_ = 0u;
  std::array<std::uint64_t, 8192u / 64u> fixed_exec_secondary_pages_{};
  void RebuildFixedExecSecondaryPages();
  std::uint64_t fixed_exec_page_cache_hits_ = 0u;
  std::uint64_t fixed_exec_page_cache_fills_ = 0u;
  const GekkoAOTModuleDesc* fixed_exec_active_descriptor_ = nullptr;
  // v163: exact-PC negative cache for fixed-image overlap decisions that could
  // not be disambiguated from guest bytes. It is epoch-local and is cleared by
  // ICBI, so it only memoizes the resolver's existing legacy fallback.
  static constexpr std::size_t kFixedExecNegativeCacheSize = 1024u;
  static_assert((kFixedExecNegativeCacheSize & (kFixedExecNegativeCacheSize - 1u)) == 0u);
  std::array<std::uint32_t, kFixedExecNegativeCacheSize> fixed_exec_negative_pcs_{};
  std::uint64_t fixed_exec_negative_cache_hits_ = 0;
  // v274: exact-PC memo of the router results that never read guest memory:
  // "no fixed-image overlap" (nullptr) and "route to the active image" (the
  // v161 and v155 paths). With a fixed side executable loaded (apploader
  // reboot.dol, extra ELFs) recomputing them cost a 512-byte candidate init
  // plus one binary search per module on every dispatch, and the one-range
  // page cache thrashes when several chunks share a 4 KiB page. These results
  // depend only on static descriptor coverage, the epoch (ICBI, module load,
  // paging change, reset) and the active image, so an entry is valid only
  // while all three are unchanged. Hash-verified decisions are never memoized.
  struct FixedExecRouteMemo {
    std::uint64_t epoch = 0u;
    const GekkoAOTModuleDesc* active = nullptr;
    const GekkoAOTModuleDesc* route = nullptr;  // nullptr: no overlap
    std::uint32_t pc = 0u;
    std::uint32_t chunk_start = 0u;
    std::uint32_t chunk_end = 0u;
  };
  static constexpr std::size_t kFixedExecRouteMemoSize = 2048u;
  static_assert((kFixedExecRouteMemoSize & (kFixedExecRouteMemoSize - 1u)) == 0u);
  std::array<FixedExecRouteMemo, kFixedExecRouteMemoSize> fixed_exec_route_memo_{};
  std::uint64_t fixed_exec_route_switches_ = 0;
  std::uint64_t global_indirect_dispatches_ = 0;
  std::uint64_t icbi_route_invalidations_ = 0;
  std::uint64_t icbi_targeted_invalidations_ = 0;
  bool leaf_tail_enabled_ = true;
  std::uint64_t leaf_tail_executions_ = 0;
  // v274: proven idle-loop park. Every host service reachable from guest code
  // (MMIO, HLE, SPR, cache ops, WGPIPE, fallbacks) and every taken interrupt
  // bumps host_effects_, so an iteration without a bump only changed guest
  // registers and guest RAM. When consecutive iterations return to the same
  // head with identical registers and GuestWriteTracker proves that the
  // iteration left every page it wrote byte-identical, the next iteration is
  // an exact replay: the host sleeps until the next hardware event instead.
  static constexpr std::size_t kIdleRegBytes =
      offsetof(CPUState, timebase) +
      (offsetof(CPUState, program_exception) + sizeof(std::uint32_t) - offsetof(CPUState, sr)) +
      sizeof(std::uint32_t) + sizeof(bool);
  static constexpr std::uint32_t kIdleLoopMaxBoundaries = 64u;
  static constexpr std::uint32_t kIdleLoopStableIterations = 8u;
  static constexpr std::size_t kIdleLoopMaxPages = 16u;
  struct IdleLoopState {
    std::uint32_t head_pc = 0u;
    std::uint32_t boundaries = 0u;
    std::uint32_t prev_boundaries = ~0u;
    std::uint64_t sequence = 0u;
    std::uint64_t prev_sequence = 0u;
    std::uint64_t effects = 0u;
    std::uint32_t stable = 0u;
    std::uint32_t cooldown = 0u;
    std::uint32_t backoff = 0u;
    std::uint8_t phase = 0u;  // 0 search, 1 learn write set, 2 verify+park
    bool have_regs = false;
    std::array<std::uint8_t, kIdleRegBytes> regs{};
  };
  bool idle_loop_proof_enabled_ = true;
  // Diagnostics are opt-in (GEKKOAOT_RUNTIME_PROFILE / GEKKOAOT_BOUNDARY_PROFILE):
  // with both the prover and profiling off, no hot path touches these counters.
  bool runtime_profile_enabled_ = false;
  bool track_host_effects_ = false;
  void CountHostEffect() { if (track_host_effects_) ++host_effects_; }
  std::uint64_t host_effects_ = 0;
  std::uint64_t idle_loop_parks_ = 0;
  std::uint64_t boundaries_total_ = 0;
  std::uint64_t guest_cycles_total_ = 0;  // cycles charged by AOT code (v274)
  bool ee_pending_query_enabled_ = true;
  bool ee_pending_barrier_enabled_ = true;
  bool native_address_base_enabled_ = true;
  bool native_address_publication_enabled_ = false;
  std::uint64_t ee_enable_queries_ = 0;
  std::uint64_t perf_last_ee_queries_ = 0;
  std::uint64_t perf_last_guest_cycles_ = 0;
  bool boundary_profile_enabled_ = false;
  std::unordered_map<std::uint64_t, std::uint64_t> boundary_profile_;
  std::uint64_t perf_last_boundaries_ = 0;
  std::uint64_t perf_last_leaf_tails_ = 0;
  std::uint64_t perf_last_hle_calls_ = 0;
  std::uint64_t perf_last_host_effects_ = 0;
  std::uint64_t perf_last_idle_parks_ = 0;
  IdleLoopState idle_loop_;
  GuestWriteTracker guest_write_tracker_;
  std::vector<std::uintptr_t> idle_pages_;
  std::vector<std::uintptr_t> idle_written_;
  std::vector<std::uint8_t> idle_page_data_;
  // v143: small architectural I-cache model for RAM-resident exception
  // vectors. 32-byte lines match Gekko cache-control granularity. The
  // AOT body remains immutable; this only covers low-memory code which
  // is generated/copied by IPL/SDK at runtime.
  static constexpr std::uint32_t kLowInstructionCacheSpan = 0x2000u;
  static constexpr std::uint32_t kLowInstructionCacheLineBytes = 32u;
  static constexpr std::size_t kLowInstructionCacheLines =
      kLowInstructionCacheSpan / kLowInstructionCacheLineBytes;
  static constexpr std::size_t kLowInstructionCacheWords =
      kLowInstructionCacheSpan / sizeof(std::uint32_t);
  std::array<std::uint32_t, kLowInstructionCacheWords> low_instruction_cache_{};
  std::array<bool, kLowInstructionCacheLines> low_instruction_cache_valid_{};
  // v294 proofs follow the instruction-visible low-memory cache. Data writes
  // alone do not invalidate a verified vector while HID0[ICE] is enabled.
  NativeLowVectorCache native_low_vector_cache_{};
  bool native_low_vector_enabled_ = false;
  std::uint64_t native_low_vector_executions_ = 0u;
  std::uint64_t native_low_vector_proofs_ = 0u;
  std::uint64_t rel_mapping_refreshes_ = 0;
  std::uint64_t rel_address_translations_ = 0;
  std::uint64_t rel_address_misses_ = 0;
  std::uint32_t rel_last_canonical_ = 0;
  std::uint32_t rel_last_runtime_ = 0;
  Fault fault_{};
  bool compat_diagnostics_enabled_ = false;
  bool run_banner_reported_ = false;
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
  struct NativeHookCodeProof {
    std::uint32_t start = 0u, end = 0u;
    std::uint64_t hash = 0u, epoch = 0u;
    bool active = false;
  };
  std::unordered_map<std::uint32_t, NativeHookCodeProof> native_hook_code_proofs_;
  void RegisterNativeHookCodeProof(std::uint32_t address);
  bool NativeHookCodeMatches(std::uint32_t address);
  struct NativeRegionProbeCacheEntry {
    std::uint32_t start = 0u;
    std::uint32_t end = 0u;
    bool blocked = false;
    bool valid = false;
    std::uint64_t epoch = 0u;
  };
  static constexpr std::size_t kNativeRegionProbeCacheSize = 1024u;
  static_assert((kNativeRegionProbeCacheSize &
                 (kNativeRegionProbeCacheSize - 1u)) == 0u);
  std::array<NativeRegionProbeCacheEntry, kNativeRegionProbeCacheSize>
      native_region_probe_cache_{};
  // Generated code probes the host at many ordinary guest function entries.
  // Almost all probes are misses. A small direct-mapped negative cache keeps
  // those misses out of unordered_map, while real HLE entries remain exact.
  static constexpr std::size_t kHostCallNegativeCacheSize = 4096u;
  std::array<std::uint32_t, kHostCallNegativeCacheSize> hostcall_negative_cache_{};
  struct PpcHaltPc { std::uint32_t pc = 0; std::uint32_t entry = 0; };
  std::vector<PpcHaltPc> ppc_halt_pcs_;
  std::array<std::uint32_t, 1024> spr_state_{};
  // v212: Gekko has a 128-entry, 2-way DTLB (64 sets).  CoD Finest Hour's
  // software pager rewrites HPT entries and relies on tlbie as the visibility
  // boundary; a direct HPT walk on every host access observes replacements too
  // early and can restore the wrong 4 KiB ARAM-backed page.
  struct DataTlbEntry {
    bool valid = false;
    std::uint32_t ea_page = 0;
    std::uint32_t vsid = 0;
    std::uint32_t physical_page = 0;
    std::uint32_t pte_address = 0;
    std::uint32_t pte_upper = 0;
    std::uint32_t pte_lower = 0;
    std::uint64_t stamp = 0;
  };
  std::array<std::array<DataTlbEntry, 2>, 64> data_tlb_{};
  // v322: generated-code mirror of resident VM-aperture DTLB entries.
  void ClearPagedView();
  void DropPagedView(const DataTlbEntry& entry);
  void PublishPagedView(const DataTlbEntry& entry, unsigned way);
  bool native_paged_view_enabled_ = true;
  std::uint64_t data_tlb_stamp_ = 0;
  std::uint64_t data_tlb_hits_ = 0;
  std::uint64_t data_tlb_fills_ = 0;
  std::uint64_t data_tlb_invalidations_ = 0;
  // Supervisor/user modes with any DBAT that can intersect the VM aperture.
  // A zero mode proves the BAT search cannot override the hashed translation.
  std::uint8_t vmem_bat_modes_ = 0;
  std::uint32_t timebase_cycle_remainder_ = 0;
  std::uint32_t decrementer_cycle_remainder_ = 0;
  bool decrementer_pending_ = false;
  bool pe_notification_irq_active_ = false;
  // AI/DSP are the only standalone devices here whose IRQ state can change
  // solely because guest time advanced. Poll them in tiny cycle quanta rather
  // than after every compiled slice; MMIO-triggered IRQs are still synchronized
  // immediately in ExternalRead/ExternalWrite.
  std::uint64_t timed_irq_sync_cycles_ = 0;
  // NativeHW v3: avoid reprogramming the DSP mixer rate at every hardware
  // sample when the guest AI mode and native task backend did not change.
  std::uint32_t native_audio_effective_hz_ = 0u;

  // v160: conservative runtime-only escape hatch for a CP/video-flip deadlock.
  // Some titles deliberately park the main thread on a GX breakpoint until a
  // later VI callback observes a draw-sync token. If the guest has no current
  // thread for several full VI frames while CP is still stopped at the same
  // already-handled breakpoint, let exactly one pending FIFO burst cross that
  // fence. This is not used during normal execution and never fires while the
  // breakpoint interrupt is still pending.
  bool cp_idle_breakpoint_recovery_enabled_ = true;
  std::uint32_t cp_idle_breakpoint_frames_ = 0;
  std::uint32_t cp_idle_breakpoint_address_ = 0;
  std::uint64_t cp_idle_breakpoint_recoveries_ = 0;
  std::uint32_t sdk_idle_poll_pc_ = 0;
  // v274: PCs whose dispatch probe recognized the SDK idle poll. That probe
  // also reads the OSEnableInterrupts callee, so a targeted icbi revalidates
  // every recorded hit instead of only the probes next to the line.
  std::vector<std::uint32_t> sdk_idle_probe_pcs_;
  std::int16_t sdk_idle_poll_offset_ = 0;

  // v181: dispatch-boundary code probes are compatibility helpers, not guest
  // work. With safe host boundaries enabled they used to reread several guest
  // instructions after nearly every AOT function return.  Memoize the exact-PC
  // result in the architectural I-cache publication epoch so the common miss
  // path is just a couple of integer compares.  ICBI/fixed-image publication
  // already bumps fixed_exec_route_epoch_, so self-modifying/overlay code is
  // re-probed before a cached answer can be reused.
  struct DispatchCodeProbeCacheEntry {
    std::uint32_t pc = 0;
    std::int16_t idle_offset = 0;
    std::uint8_t idle_state = 0;  // 0 unknown, 1 miss, 2 SDK idle poll
    std::uint8_t time_kind = 0;   // 0 unknown, 1 miss, 2 OSGetTick, 3 OSGetTime,
                                  // 4 integer leaf tail (v274)
  };
  // v185: HPSS idle profiles still showed AddressSpace::Read32() beneath
  // HostRuntime::Run after v181. The cache is direct-mapped and shared by the
  // SDK-idle and OSGetTime/Tick recognizers, so 4K slots can thrash on larger
  // retail images even though each entry is tiny. A 16K table remains a small
  // fixed allocation while greatly reducing exact-PC collision misses.
  static constexpr std::size_t kDispatchCodeProbeCacheSize = 16384u;
  static_assert((kDispatchCodeProbeCacheSize &
                 (kDispatchCodeProbeCacheSize - 1u)) == 0u);
  std::array<DispatchCodeProbeCacheEntry, kDispatchCodeProbeCacheSize>
      dispatch_code_probe_cache_{};
  std::uint64_t dispatch_code_probe_epoch_ = 0;
  // v274: full probe flushes (module load, paging, ICFI, large or paged
  // invalidations). icbi of a MEM1 line drops only the probes it can affect.
  std::uint64_t dispatch_code_probe_generation_ = 1u;
  DispatchCodeProbeCacheEntry& ProbeDispatchCode(std::uint32_t pc);
  static_assert(sizeof(DispatchCodeProbeCacheEntry) == 8u);
  bool sdk_idle_park_enabled_ = false;
  bool native_read_poll_enabled_ = false;
  bool native_counter_poll_enabled_ = false;
  std::uint32_t native_counter_sample_divider_=0;
  std::uint64_t native_counter_poll_parks_=0,native_counter_poll_epoch_=0;
  struct NativeCounterPollEntry {std::uint32_t pc=0,lr=0;bool valid=false;NativeCounterPoll plan;};
  std::array<NativeCounterPollEntry,256> native_counter_polls_{};
  // v334: clock-wait loops (see native_spin_probe.h). The clock HLE records its
  // return site; the next boundary there may park until the first clock value
  // at which the loop stops waiting.
  bool clock_spin_enabled_ = false;
  bool time_poll_wide_ = false;
  std::uint32_t time_poll_return_ = 0, time_poll_entry_ = 0;
  std::uint32_t clock_spin_last_lr_ = 0, clock_spin_streak_ = 0;
  std::uint64_t clock_spin_last_tb_ = 0;
  std::uint64_t clock_spin_parks_ = 0, clock_spin_probes_ = 0, clock_spin_epoch_ = 0;
  struct ClockSpinEntry {std::uint32_t lr=0,entry=0;bool wide=false;std::uint32_t cooldown=0;std::uint64_t exit=0;};
  std::array<ClockSpinEntry,64> clock_spins_{};
  SpinProbe spin_probe_;
  // v344: no-op OSYieldThread loops (see ParkYieldSpin). DispatchOsHle counts
  // yields that returned to their caller without a thread switch.
  bool yield_spin_enabled_ = false;
  std::uint32_t yield_noop_lr_ = 0, yield_noop_count_ = 0;
  std::uint64_t yield_spin_parks_ = 0, yield_spin_declines_ = 0, yield_spin_park_ns_ = 0;
  std::chrono::steady_clock::time_point yield_spin_start_{};
  RunResult* active_result_ = nullptr;
  // v346: wait loops whose iteration is a fixed point (see ParkFixedPointPoll).
  bool fixed_point_park_enabled_ = false;
  struct FixedPointEntry {std::uint32_t pc=0,hits=0,cooldown=0,failures=0;bool proven=false;};
  std::array<FixedPointEntry,64> fixed_points_{};
  std::uint32_t fixed_point_last_pc_ = 0;
  std::uint64_t fixed_point_parks_ = 0, fixed_point_probes_ = 0, fixed_point_park_ns_ = 0;
  // v347: repeated device-status reads (see NoteMmioPoll).
  bool mmio_poll_park_enabled_ = false;
  // v355: idle decrementer coalescing (EA THREAD_yieldticks(10) storms).
  std::uint64_t idle_dec_quantum_ticks_ = 0, idle_dec_last_tb_ = 0;
  std::uint64_t idle_dec_coalesced_ = 0;
  std::uint32_t mmio_poll_address_ = 0, mmio_poll_count_ = 0;
  std::uint64_t mmio_poll_value_ = 0, mmio_poll_parks_ = 0, mmio_poll_park_ns_ = 0;
  std::chrono::steady_clock::time_point mmio_poll_start_{};
  std::uint64_t native_read_poll_generation_=1,native_read_poll_epoch_=0,native_read_poll_parks_=0;
  std::array<std::uint32_t,4096> native_read_poll_negatives_{};
  struct NativeReadPollEntry {std::uint32_t pc=0;NativeReadPoll plan;};
  std::array<NativeReadPollEntry,64> native_read_polls_{};
  bool aot_poll_park_enabled_ = true;
  bool idle_event_wait_enabled_ = true;
  std::uint32_t idle_event_max_wait_us_ = 2000u;
  std::uint64_t aot_poll_parks_ = 0u;
  std::uint64_t idle_event_waits_ = 0u;
  // v300 GEKKOAOT_RUNTIME_PROFILE: idle wall time split by GX worker state.
  std::uint64_t idle_ns_gx_busy_ = 0u;
  std::uint64_t idle_ns_gx_idle_ = 0u;
  std::uint64_t idle_gx_wakes_ = 0u;
  // v300 GEKKOAOT_RUNTIME_PROFILE: guest loads/stores that left the AOT fast
  // path, by address class: vm, wgpipe, flipper mmio, low physical/other.
  std::array<std::uint64_t, 4> external_reads_{};
  std::array<std::uint64_t, 4> external_writes_{};
  // v301 profile: 64 KiB regions of "other"-class external accesses.
  std::unordered_map<std::uint32_t, std::uint64_t> external_other_regions_{};
  void NoteExternalOther(std::uint32_t address, bool write) {
    if (ExternalClass(address) == 3u)
      ++external_other_regions_[(address & 0xffff0000u) | (write ? 1u : 0u)];
  }
  static unsigned ExternalClass(std::uint32_t address) {
    const std::uint32_t physical = address & 0x3fffffffu;
    if (address >= 0x7e000000u && address < 0x80000000u) return 0u;
    if ((address & 0x3fffffe0u) == 0x0c008000u) return 1u;
    if (physical >= 0x0c000000u && physical < 0x0c007000u) return 2u;
    return 3u;
  }
  std::uint64_t idle_event_requested_cycles_ = 0u;

  // VI remains the guest-visible timing/IRQ authority. Presentation may be
  // decoupled and driven by completed GX display copies, with VI retained as a
  // compatibility watchdog/fallback.
  bool gx_present_on_copy_ = false;
  bool gx_present_fallback_vi_ = true;
  bool gx_frame_ready_since_vi_ = false;
  std::uint64_t native_vi_last_queued_frame_ = 0;
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
  // v135: tells AdvanceRuntimeCycles whether TakeRealtimeHardwareCycles actually
  // sampled the host monotonic clock. Presentation reuses that sample instead
  // of issuing a second clock_gettime at every AOT/HLE boundary.
  bool hardware_clock_sampled_last_call_ = false;
  // v136: on x86 hosts exposing an invariant TSC with an exact CPUID.15H
  // crystal ratio, use TSC deltas for the realtime hardware clock. This removes
  // the remaining vDSO clock_gettime hotspot while retaining std::chrono as the
  // portable/VM fallback. hardware_clock_last_ remains a synthetic steady-clock
  // time_point so presentation and metrics share the same monotonic domain.
  bool hardware_tsc_checked_ = false;
  bool hardware_tsc_enabled_ = false;
  std::uint64_t hardware_tsc_hz_ = 0;
  std::uint64_t hardware_tsc_last_ = 0;
  std::uint64_t hardware_tsc_cycle_remainder_ = 0;
  std::uint64_t hardware_tsc_ns_remainder_ = 0;

  // Presentation is a separate host domain. The slider caps completed GX
  // frames only; it never changes VI, CPU, DSP or AI timing.
  std::uint32_t host_fps_limit_hz_ = 120;
  bool frame_interpolation_enabled_ = false;
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
  double perf_last_thread_seconds_ = -1.0;
  std::uint64_t perf_metrics_poll_cycles_ = 0;

  // v176: widescreen CPU-frustum assist. The renderer can widen the GX
  // projection universally, but retail games may cull objects on the CPU using
  // a separate 4:3 camera aspect. The safe runtime layer first uses known
  // projection observed by NativeGX, then optionally discovers a single
  // unambiguous adjacent [FOV, aspect] pair in writable MEM1. There are no game
  // IDs or fixed guest addresses in the automatic path.
  bool widescreen_frustum_assist_enabled_ = true;
  bool widescreen_frustum_manual_ = false;
  bool widescreen_frustum_ambiguous_logged_ = false;
  bool widescreen_frustum_selected_logged_ = false;
  std::uint32_t widescreen_frustum_aspect_address_ = 0u;
  std::uint32_t widescreen_frustum_fov_address_ = 0u;
  std::uint32_t widescreen_frustum_pending_aspect_address_ = 0u;
  std::uint32_t widescreen_frustum_pending_fov_address_ = 0u;
  std::uint32_t widescreen_frustum_pending_confirm_frames_ = 0u;
  std::uint32_t widescreen_frustum_scan_frames_ = 0u;
  std::uint32_t widescreen_frustum_scan_offset_ = 0u;
  std::uint32_t widescreen_frustum_scan_candidate_count_ = 0u;
  std::uint32_t widescreen_frustum_scan_candidate_aspect_ = 0u;
  std::uint32_t widescreen_frustum_scan_candidate_fov_ = 0u;
  std::uint32_t widescreen_frustum_next_scan_frame_ = 30u;
  float widescreen_frustum_last_target_aspect_ = 4.0f / 3.0f;
  float widescreen_frustum_original_aspect_ = 4.0f / 3.0f;
  float widescreen_frustum_observed_fov_ = 0.0f;
  float widescreen_frustum_observed_aspect_ = 4.0f / 3.0f;

  std::filesystem::path live_video_config_path_;
  GX::GraphicsSettings live_graphics_settings_ = GX::GraphicsSettingsFromEnvironment();
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
  GekkoAOT::NativeHW::Service native_hw_;
  GekkoAOT::NativeHW::EXI::HostService native_exi_host_;
  NativeVfs native_vfs_;
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
  std::uint64_t vfs_reads_ = 0;
  std::uint64_t vfs_read_bytes_ = 0;
  std::uint64_t vfs_fallback_reads_ = 0;
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
  std::vector<std::uint8_t> di_async_staging_{};
  struct DiscIoResult {
    bool ok = false;
    std::vector<std::uint8_t> bytes{};
  };
  std::future<DiscIoResult> di_io_future_{};
  bool di_io_inflight_ = false;
  bool runtime_parallel_workers_ = false;
  bool debug_fifo_overrun_ = false;
  bool fifo_irq_fence_enabled_ = true;
  bool atomic_fifo_service_ = false;
  std::uint64_t fifo_irq_fences_ = 0;
  bool TryTakeFifoWriteInterrupt(bool sdk_entry = false);
  // v172: backend-neutral runtime hot paths. Both optimizations preserve the
  // exact generic fallback and can be disabled independently at runtime for
  // compatibility bisects without rebuilding C or LLVM modules.
  bool fast_wgpipe_enabled_ = true;
  bool native_sdk_gx_packets_enabled_ = true;
  bool native_sdk_gx_arrays_enabled_ = true;
  std::uint64_t native_sdk_gx_packets_ = 0u;
  bool fast_time_probe_enabled_ = true;
  std::mutex disc_io_mutex_{};
  std::uint64_t di_drive_head_offset_ = 0;
  bool di_drive_head_valid_ = false;
  std::uint64_t di_read_buffer_start_ = 0;
  std::uint64_t di_read_buffer_end_ = 0;
  GekkoAOT::HW::EXI::NativeEXI exi_;
  GekkoAOT::HW::SI::NativeSI si_;
  std::array<GekkoAOT::Input::Pad, 4> native_pads_{};
  std::uint64_t native_pad_reads_ = 0u;
  std::array<std::uint8_t, 4> native_pad_mode_{{3u, 3u, 3u, 3u}};
  bool native_input_ready_ = false;
  // NativeHW v4 keeps SI sampling in the hardware-time domain. This must be
  // per-runtime state (not a function-static accumulator) so multiple runtime
  // instances/tests cannot leak cadence into each other.
  std::uint64_t native_input_hw_phase_ = 0;
  std::uint64_t native_input_samples_ = 0;
  std::uint64_t native_si_auto_polls_ = 0;
  std::uint64_t native_si_transfer_samples_ = 0;
  std::chrono::steady_clock::time_point native_si_last_transfer_sample_{};
  GekkoAOT::HW::VI::NativeVI vi_;
  GekkoAOT::DSP::NativeDSP dsp_;
  std::uint64_t hle_calls_ = 0;
  // v301 profile: native service calls by kind (simple SDK leaves / NativeOS).
  std::array<std::uint64_t, static_cast<std::size_t>(SimpleHleKind::Count)> simple_hle_kind_calls_{};
  std::array<std::uint64_t, 64> os_hle_kind_calls_{};
  std::uint32_t last_mmio_read_ = 0;
  std::uint64_t mmio_reads_ = 0;
  std::uint64_t same_mmio_read_streak_ = 0;
  bool cp_poll_drain_enabled_ = true;
  bool cp_poll_park_enabled_ = false;
  bool gx_matrix_chain_enabled_ = false;
  std::uint64_t cp_poll_drains_ = 0;
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
