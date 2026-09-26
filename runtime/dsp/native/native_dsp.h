#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include "native/address_space.h"
#include "dsp/native/native_dsp_core.h"
#include "dsp/native/native_ax_hle.h"
#include "dsp/native/native_jaudio_hle.h"
#include "dsp/native/native_audio_sink.h"

#include <array>
#include <cstdint>
#include <vector>

namespace GekkoAOT::DSP {

// Standalone GameCube DSP interface owned by GekkoAOT.
//
// This is intentionally not a DSP-HLE wrapper. It owns the hardware-visible
// MMIO register bank, PPC<->DSP mailboxes, ARAM, ARAM DMA and audio-DMA state.
// A future native DSP core/ucode executor plugs into ConsumeCpuMailbox(),
// PostDspMailbox() and the ARAM accessors without changing the host MMIO ABI.
class NativeDSP final {
public:
  static constexpr std::uint32_t BasePhysical = 0x0c005000u;
  static constexpr std::uint32_t WindowSize = 0x1000u;
  static constexpr std::uint32_t AramSize = 0x01000000u;
  static constexpr std::uint32_t AramMask = AramSize - 1u;
  static constexpr std::uint32_t IramWords = 0x1000u;
  static constexpr std::uint32_t DramWords = 0x1000u;

  explicit NativeDSP(Native::AddressSpace* memory = nullptr);

  void BindMemory(Native::AddressSpace* memory) { memory_ = memory; }
  void Reset();
  bool Handles(std::uint32_t address) const;
  bool Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value);
  bool Write(std::uint32_t address, std::uint64_t value, std::uint8_t size);
  void AdvanceCycles(std::uint64_t cycles);
  void SetAudioSampleRateHz(std::uint32_t hz) { audio_sample_rate_hz_ = hz ? hz : 32028u; }

  // PI sees one DSP interrupt source. The DSP_CONTROL cause bits and their
  // adjacent mask bits decide whether that source is asserted.
  bool InterruptPending() const;

  // Native-core boundary. The MMIO layer accepts CPU mail independently of
  // whatever native ucode/core implementation is attached later.
  bool ConsumeCpuMailbox(std::uint32_t* mail);
  void PostDspMailbox(std::uint32_t mail, bool interrupt = true);

  std::uint8_t ReadAram(std::uint32_t address) const;
  void WriteAram(std::uint32_t address, std::uint8_t value);
  const std::vector<std::uint8_t>& Aram() const { return aram_; }
  std::vector<std::uint8_t>& Aram() { return aram_; }

  std::uint16_t Control() const { return control_; }
  std::uint32_t CpuMailbox() const { return cpu_mail_visible_; }
  std::uint32_t DspMailbox() const { return dsp_mail_visible_; }
  bool DspMailboxPending() const { return dsp_mail_pending_; }
  std::uint32_t AramDmaMainAddress() const { return aram_dma_mmaddr_; }
  std::uint32_t AramDmaAramAddress() const { return aram_dma_araddr_; }
  std::uint32_t AudioDmaSource() const { return audio_dma_source_; }
  bool SdkBootstrapLoaded() const { return sdk_bootstrap_loaded_; }
  bool SdkBootstrapRunning() const { return sdk_bootstrap_running_; }
  bool SdkBootstrapCompleted() const { return sdk_bootstrap_completed_; }
  std::uint8_t SdkTaskLoaderStage() const { return sdk_task_loader_stage_; }
  bool SdkTaskInitPending() const { return sdk_task_init_cycles_ != 0u; }
  std::uint64_t SdkTaskInitCount() const { return sdk_task_init_count_; }
  std::uint32_t LastCpuMailbox() const { return last_cpu_mail_; }
  std::uint64_t CpuMailboxWriteCount() const { return cpu_mail_write_count_; }
  bool NativeCoreAvailable() const { return core_.Available(); }
  bool NativeAxHleActive() const { return ax_hle_.Active(); }
  bool NativeJAudioHleActive() const { return jaudio_hle_.Active(); }
  std::uint64_t NativeJAudioSyncFrames() const { return jaudio_hle_.SyncFrames(); }
  std::uint64_t NativeJAudioVoicesMixed() const { return jaudio_hle_.VoicesMixed(); }
  std::uint32_t NativeAxUcodeCrc32() const { return ax_hle_.UcodeCrc32(); }
  std::uint64_t NativeAxCommandLists() const { return ax_hle_.CommandLists(); }
  std::uint64_t NativeAxVoicesMixed() const { return ax_hle_.VoicesMixed(); }
  std::uint64_t NativeAxOutputFrames() const { return ax_hle_.OutputFrames(); }
  bool NativeCoreRomLoaded() const { return core_.RomLoaded(); }
  std::uint16_t NativeCorePc() const { return core_.Pc(); }
  std::uint16_t NativeCoreLastOpcode() const { return core_.LastOpcode(); }
  std::uint64_t NativeCoreInstructions() const { return core_.ExecutedInstructions(); }
  std::uint64_t AudioFramesSubmitted() const { return audio_sink_.FramesSubmitted(); }

  // Standalone LLE host bridge. These are deliberately narrow raw-memory and
  // interrupt operations, not Dolphin Core callbacks.
  bool CoreReadMain(std::uint32_t address, std::uint8_t* dst, std::uint32_t size) const;
  bool CoreWriteMain(std::uint32_t address, const std::uint8_t* src, std::uint32_t size);
  void RequestCoreInterrupt();
  void FlushAudioSink() { audio_sink_.Flush(); }

private:
  static constexpr std::uint32_t ToPhysical(std::uint32_t address) {
    return address & 0x3fffffffu;
  }

  bool Read16(std::uint32_t offset, std::uint16_t* value);
  bool Write16(std::uint32_t offset, std::uint16_t value);
  void WriteControl(std::uint16_t value);
  void GenerateInterrupt(std::uint16_t cause);
  void RunAramDma();
  void StartAudioDma(std::uint16_t value);
  void AdvanceAudioDma(std::uint64_t cycles);
  void SubmitAudioBlock();
  void LoadInitProgramFromMainMemory();
  bool LooksLikeSdkInitProgram() const;
  void StartSdkBootstrap();
  void FinishSdkBootstrap();
  void HandleSdkTaskLoaderMail(std::uint32_t mail);
  void FinishSdkTaskLaunch();

  Native::AddressSpace* memory_ = nullptr;
  NativeDSPCore core_;
  NativeAXHLE ax_hle_;
  NativeJAudioHLE jaudio_hle_;
  NativeAudioSink audio_sink_;
  std::vector<std::uint8_t> aram_;
  std::array<std::uint16_t, IramWords> iram_{};
  std::array<std::uint16_t, DramWords> dram_{};

  std::uint32_t cpu_mail_visible_ = 0;
  std::uint32_t cpu_mail_pending_value_ = 0;
  bool cpu_mail_pending_ = false;
  std::uint32_t dsp_mail_visible_ = 0;
  bool dsp_mail_pending_ = false;

  std::uint16_t control_ = 0;
  std::uint16_t interrupt_control_ = 0;
  std::uint16_t ar_info_ = 0;
  std::uint16_t ar_mode_ = 1;
  std::uint16_t ar_refresh_ = 156;

  std::uint32_t aram_dma_mmaddr_ = 0;
  std::uint32_t aram_dma_araddr_ = 0;
  std::uint32_t aram_dma_count_ = 0;

  std::uint32_t audio_dma_source_ = 0;
  std::uint16_t audio_dma_blocks_length_ = 0;
  std::uint16_t audio_dma_control_ = 0;
  std::uint16_t audio_dma_remaining_blocks_ = 0;
  std::uint32_t audio_dma_current_source_ = 0;
  std::uint32_t audio_sample_rate_hz_ = 32028u;
  std::uint64_t audio_sample_phase_ = 0;
  std::uint32_t audio_frames_into_block_ = 0;
  // Whole 8-frame AID blocks whose wall-clock time has elapsed but which
  // have not yet been presented to the guest DMA engine.  The standalone
  // runtime advances hardware from host realtime in comparatively large
  // chunks; preserving this debt lets us stop at each guest-visible AID IRQ
  // boundary instead of collapsing several 5 ms DMA completions into one
  // level interrupt.
  std::uint64_t audio_dma_pending_blocks_ = 0;
  bool audio_dma_event_mode_reported_ = false;
  std::uint64_t audio_blocks_submitted_ = 0;

  std::uint64_t init_code_clear_cycles_ = 0;
  std::uint64_t aid_interrupt_cycles_ = 0;
  std::uint64_t sdk_bootstrap_cycles_ = 0;
  bool sdk_bootstrap_loaded_ = false;
  bool sdk_bootstrap_running_ = false;
  bool sdk_bootstrap_completed_ = false;

  std::uint8_t sdk_task_loader_stage_ = 0;
  std::uint32_t sdk_task_code_address_ = 0;
  std::uint32_t sdk_task_code_size_ = 0;
  std::uint16_t sdk_task_imem_address_ = 0;
  std::uint16_t sdk_task_dmem_length_ = 0;
  std::uint16_t sdk_task_entry_ = 0;
  std::uint64_t sdk_task_init_cycles_ = 0;
  std::uint64_t sdk_task_init_count_ = 0;
  // When no executable backend exists for an otherwise valid SDK task, keep
  // the SDK task-start handshake moving. The first DCD1 mail is consumed by
  // __DSPHandler before the task's init callback runs; several JAudio-era
  // callbacks then synchronously poll for one task-owned ready mail.
  bool sdk_romless_task_ready_pending_ = false;
  // ROM-less JAudio tasks need more than the DCD1 startup handshake: the PPC
  // sends small command packets and waits for a DSP work-completion mailbox.
  // Keep a tiny protocol state machine so callback-driven setup can complete
  // without game/address-specific hooks.
  bool sdk_romless_task_active_ = false;
  bool sdk_romless_commands_ready_ = false;
  std::uint8_t sdk_romless_command_words_expected_ = 0;
  std::uint8_t sdk_romless_command_words_seen_ = 0;
  std::uint16_t sdk_romless_work_token_ = 0;
  std::array<std::uint32_t, 32> sdk_romless_packet_words_{};
  bool sdk_romless_packet_count_prefixed_ = true;
  std::uint16_t sdk_romless_completion_token_ = 0;
  bool sdk_romless_completion_callback_mode_ = true;
  std::uint64_t sdk_romless_completion_cycles_ = 0;
  // JAudio work completion is delivered in two stages: DCD10004 raises the
  // SDK task callback, then F355|token is exposed for syncDSP() to poll.
  std::uint32_t sdk_romless_completion_payload_pending_ = 0;
  std::uint32_t last_cpu_mail_ = 0;
  std::uint64_t cpu_mail_write_count_ = 0;
};

} // namespace GekkoAOT::DSP
