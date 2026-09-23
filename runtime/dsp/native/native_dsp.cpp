// SPDX-License-Identifier: GPL-3.0-or-later
#include "dsp/native/native_dsp.h"

#include <algorithm>
#include <cstring>
#include <cstdio>

namespace GekkoAOT::DSP {
namespace {
constexpr std::uint32_t kMailToDspHi = 0x00u;
constexpr std::uint32_t kMailToDspLo = 0x02u;
constexpr std::uint32_t kMailFromDspHi = 0x04u;
constexpr std::uint32_t kMailFromDspLo = 0x06u;
constexpr std::uint32_t kControl = 0x0au;
constexpr std::uint32_t kInterruptControl = 0x10u;
constexpr std::uint32_t kArInfo = 0x12u;
constexpr std::uint32_t kArMode = 0x16u;
constexpr std::uint32_t kArRefresh = 0x1au;
constexpr std::uint32_t kArDmaMmAddrHi = 0x20u;
constexpr std::uint32_t kArDmaMmAddrLo = 0x22u;
constexpr std::uint32_t kArDmaArAddrHi = 0x24u;
constexpr std::uint32_t kArDmaArAddrLo = 0x26u;
constexpr std::uint32_t kArDmaCountHi = 0x28u;
constexpr std::uint32_t kArDmaCountLo = 0x2au;
constexpr std::uint32_t kAudioDmaStartHi = 0x30u;
constexpr std::uint32_t kAudioDmaStartLo = 0x32u;
constexpr std::uint32_t kAudioDmaBlocksLength = 0x34u;
constexpr std::uint32_t kAudioDmaControlLen = 0x36u;
constexpr std::uint32_t kAudioDmaBlocksLeft = 0x3au;

constexpr std::uint16_t kDspReset = 0x0001u;
constexpr std::uint16_t kDspAssertInt = 0x0002u;
constexpr std::uint16_t kDspHalt = 0x0004u;
constexpr std::uint16_t kAid = 0x0008u;
constexpr std::uint16_t kAidMask = 0x0010u;
constexpr std::uint16_t kAram = 0x0020u;
constexpr std::uint16_t kAramMask = 0x0040u;
constexpr std::uint16_t kDsp = 0x0080u;
constexpr std::uint16_t kDspMask = 0x0100u;
constexpr std::uint16_t kDmaState = 0x0200u;
constexpr std::uint16_t kDspInitCode = 0x0400u;
constexpr std::uint16_t kDspInit = 0x0800u;
constexpr std::uint16_t kInterruptCauses = kAid | kAram | kDsp;
constexpr std::uint16_t kControlWritableState = kDspAssertInt | kDspHalt | kAidMask |
                                                kAramMask | kDspMask | kDspInitCode |
                                                kDspInit;

constexpr std::uint32_t kAramAddressMask = 0x03ffffffu;
constexpr std::uint16_t kHighAddressMask = 0x03ffu;
constexpr std::uint16_t kAlignedLowMask = 0xffe0u;
constexpr std::uint16_t kArInfoMask = 0x007fu;
constexpr std::uint16_t kArRefreshMask = 0x07ffu;
constexpr std::uint16_t kAudioHighMaskGc = 0x03ffu;

// GC SDK DSP initialization uploads a small program from physical 0x01000000
// into DSP IRAM.  Public hardware documentation describes the program as
// probing IROM/COEF and clearing DRAM; the real SDK bootstrap sequence writes
// 0x0054 to DMBH then 0x4348 to DMBL, yielding the 31-bit payload 0x00544348.
// Keep this as a narrow bootstrap state machine until the standalone DSP
// instruction core is complete; ordinary game ucodes are *not* HLE'd here.
constexpr std::uint32_t kInitProgramMainAddress = 0x01000000u;
constexpr std::uint32_t kInitProgramBytes = 0x400u;
constexpr std::uint32_t kInitProgramWords = kInitProgramBytes / 2u;
constexpr std::uint32_t kSdkBootstrapMail = 0x00544348u;

// Approximate the work performed by the tiny SDK bootstrap before it posts its
// ready mail.  The DSP runs at one sixth of the 486 MHz Gekko clock; the
// bootstrap walks 0x1000 IROM words, 0x1000 DRAM words and 0x0800 coefficient
// words, with loop/control overhead.  Exact timing is not guest-visible beyond
// "not immediate", so use a conservative PPC-cycle budget rather than an
// impossible same-MMIO reply.
constexpr std::uint64_t kSdkBootstrapPpcCycles = 0x30000u;

// Standard Nintendo SDK DSP task-loader protocol.  __DSP_boot_task sends these
// command words to the tiny init program before the newly loaded task starts.
// The sequence is shared across multiple SDK generations in the debug corpus.
// We model only the hardware-visible loader handshake here; the actual game
// ucode remains a separate native-DSP responsibility.
// CMBH bit 15 is the mailbox-full flag and is not part of the 31-bit payload.
// The SDK builds the raw CPU-side words with `lis 0x80F4` + signed `addi`
// immediates (A001/C002/A002/B002/D001).  The signed low half borrows from
// the upper half, so the raw Gekko value is 0x80F3xxxx; stripping CMBH.M
// leaves the actual DSP-visible payload 0x00F3xxxx.
constexpr std::uint32_t kSdkTaskLoadCodeAddress = 0x00f3a001u;
constexpr std::uint32_t kSdkTaskLoadImemAddress = 0x00f3c002u;
constexpr std::uint32_t kSdkTaskLoadCodeLength   = 0x00f3a002u;
constexpr std::uint32_t kSdkTaskLoadDmemLength   = 0x00f3b002u;
constexpr std::uint32_t kSdkTaskStartAddress   = 0x00f3d001u;
constexpr std::uint32_t kSdkTaskInitMail       = 0x5cd10000u; // 31-bit DMB payload for 0xDCD10000.
// SDK task message 0xDCD10004 invokes STRUCT_DSP_TASK::callback (offset 0x34).
// JAudio installs syncDSP() there; the ucode then leaves the actual F355 work
// completion in DMB for that callback to consume.
constexpr std::uint32_t kSdkTaskCallbackMail   = 0x5cd10004u;
constexpr std::uint64_t kSdkTaskInitPpcCycles  = 0x4000u;

// JAudio's ROM-less work protocol. The PPC side sends a packet length followed
// by command words. DspStartWork stores the upper 16 bits of the first command
// as a completion token; the ucode later replies with JAS_DSP_PREFIX|token.
// 0x8100 is the setup-table command family used with a completion callback,
// while e.g. 0x8200 is a fire-and-forget sync-frame command.
constexpr std::uint32_t kJAudioDspPrefix = 0xf3550000u;
constexpr std::uint16_t kJAudioSetupWorkToken = 0x8100u;
constexpr std::uint64_t kJAudioRomlessCompletionPpcCycles = 0x2000u;
}

NativeDSP::NativeDSP(Native::AddressSpace* memory) : memory_(memory), core_(this), ax_hle_(this), jaudio_hle_(this), aram_(AramSize) {
  Reset();
}

void NativeDSP::Reset() {
  core_.Reset();
  ax_hle_.Reset();
  jaudio_hle_.Reset();
  audio_sink_.Reset();
  std::fill(aram_.begin(), aram_.end(), 0);
  iram_.fill(0);
  dram_.fill(0);
  cpu_mail_visible_ = 0;
  cpu_mail_pending_value_ = 0;
  cpu_mail_pending_ = false;
  dsp_mail_visible_ = 0;
  dsp_mail_pending_ = false;

  // Retail boot sees the DSP halted and in its initialization state.  The
  // actual native DSP core can later replace that state machine behind this
  // interface without changing the MMIO-visible contract.
  control_ = kDspHalt | kDspInit;
  interrupt_control_ = 0;
  ar_info_ = 0;
  ar_mode_ = 1;
  ar_refresh_ = 156;

  aram_dma_mmaddr_ = 0;
  aram_dma_araddr_ = 0;
  aram_dma_count_ = 0;
  audio_dma_source_ = 0;
  audio_dma_blocks_length_ = 0;
  audio_dma_control_ = 0;
  audio_dma_remaining_blocks_ = 0;
  audio_dma_current_source_ = 0;
  audio_sample_phase_ = 0;
  audio_frames_into_block_ = 0;
  audio_dma_pending_blocks_ = 0;
  audio_dma_event_mode_reported_ = false;
  audio_blocks_submitted_ = 0;
  init_code_clear_cycles_ = 0;
  aid_interrupt_cycles_ = 0;
  sdk_bootstrap_cycles_ = 0;
  sdk_bootstrap_loaded_ = false;
  sdk_bootstrap_running_ = false;
  sdk_bootstrap_completed_ = false;
  sdk_romless_task_ready_pending_ = false;
  sdk_romless_task_active_ = false;
  sdk_romless_commands_ready_ = false;
  sdk_romless_command_words_expected_ = 0;
  sdk_romless_command_words_seen_ = 0;
  sdk_romless_work_token_ = 0;
  sdk_romless_packet_words_.fill(0);
  sdk_romless_packet_count_prefixed_ = true;
  sdk_romless_completion_token_ = 0;
  sdk_romless_completion_callback_mode_ = true;
  sdk_romless_completion_cycles_ = 0;
  sdk_romless_completion_payload_pending_ = 0;
  sdk_task_loader_stage_ = 0;
  sdk_task_code_address_ = 0;
  sdk_task_code_size_ = 0;
  sdk_task_imem_address_ = 0;
  sdk_task_dmem_length_ = 0;
  sdk_task_entry_ = 0;
  sdk_task_init_cycles_ = 0;
  sdk_task_init_count_ = 0;
  last_cpu_mail_ = 0;
  cpu_mail_write_count_ = 0;
}

bool NativeDSP::Handles(std::uint32_t address) const {
  const std::uint32_t physical = ToPhysical(address);
  return physical >= BasePhysical && physical < BasePhysical + WindowSize;
}

bool NativeDSP::InterruptPending() const {
  // Each cause bit has its enable bit directly above it.
  return (((control_ >> 1u) & control_ & kInterruptCauses) != 0u);
}

void NativeDSP::GenerateInterrupt(std::uint16_t cause) {
  control_ |= static_cast<std::uint16_t>(cause & kInterruptCauses);
}

bool NativeDSP::ConsumeCpuMailbox(std::uint32_t* mail) {
  if (!mail || !cpu_mail_pending_) return false;
  *mail = cpu_mail_pending_value_;
  cpu_mail_pending_ = false;
  cpu_mail_visible_ &= 0x7fffffffu;
  return true;
}

void NativeDSP::PostDspMailbox(std::uint32_t mail, bool interrupt) {
  // Bit 31 is mailbox ownership/full state, not part of the 31-bit payload.
  dsp_mail_visible_ = mail & 0x7fffffffu;
  dsp_mail_pending_ = true;
  if (interrupt) GenerateInterrupt(kDsp);
}

void NativeDSP::LoadInitProgramFromMainMemory() {
  sdk_bootstrap_loaded_ = false;
  sdk_bootstrap_running_ = false;
  sdk_bootstrap_completed_ = false;
  sdk_bootstrap_cycles_ = 0;
  sdk_task_loader_stage_ = 0;
  sdk_task_code_address_ = 0;
  sdk_task_code_size_ = 0;
  sdk_task_entry_ = 0;
  sdk_task_init_cycles_ = 0;
  sdk_romless_task_ready_pending_ = false;
  sdk_romless_task_active_ = false;
  sdk_romless_commands_ready_ = false;
  sdk_romless_command_words_expected_ = 0;
  sdk_romless_command_words_seen_ = 0;
  sdk_romless_work_token_ = 0;
  sdk_romless_packet_words_.fill(0);
  sdk_romless_packet_count_prefixed_ = true;
  sdk_romless_completion_token_ = 0;
  sdk_romless_completion_callback_mode_ = true;
  sdk_romless_completion_cycles_ = 0;
  sdk_romless_completion_payload_pending_ = 0;

  if (!memory_) return;
  const auto* source = memory_->Resolve(kInitProgramMainAddress, kInitProgramBytes);
  if (!source) return;

  // DSP IMEM is 16-bit and the PPC image is stored big-endian in MEM1.
  for (std::uint32_t i = 0; i < kInitProgramWords; ++i) {
    iram_[i] = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(source[i * 2u]) << 8u) |
        static_cast<std::uint16_t>(source[i * 2u + 1u]));
  }

  sdk_bootstrap_loaded_ = LooksLikeSdkInitProgram();
}

bool NativeDSP::LooksLikeSdkInitProgram() const {
  // Nintendo SDK revisions use the same eight two-word JMP-vector shape but
  // not the same absolute target numbers.  Matching one SDK revision exactly
  // made older titles spin forever in __OSInitAudioSystem while polling DMBH:
  // the bootstrap was present, but sdk_bootstrap_loaded_ never became true.
  //
  // Keep the detector clean-room and structural: vector 0 jumps to the fixed
  // bootstrap entry, vectors 1..7 are JMPs to a compact consecutive handler
  // table.  The target window is deliberately narrow enough to reject normal
  // retail ucodes without baking a particular SDK's table into the runtime.
  if (iram_[0] != 0x029fu || iram_[1] != 0x0010u) return false;

  if (iram_[2] != 0x029fu) return false;
  const std::uint16_t handler_base = iram_[3];
  if (handler_base < 0x0020u || handler_base > 0x0080u) return false;

  for (std::uint32_t vector = 1; vector < 8; ++vector) {
    if (iram_[vector * 2u] != 0x029fu) return false;
    const auto expected = static_cast<std::uint16_t>(handler_base + vector - 1u);
    if (iram_[vector * 2u + 1u] != expected) return false;
  }
  return true;
}

void NativeDSP::StartSdkBootstrap() {
  if (!sdk_bootstrap_loaded_ || sdk_bootstrap_running_ || sdk_bootstrap_completed_)
    return;
  sdk_bootstrap_running_ = true;
  sdk_bootstrap_cycles_ = kSdkBootstrapPpcCycles;
}

void NativeDSP::FinishSdkBootstrap() {
  sdk_bootstrap_cycles_ = 0;
  sdk_bootstrap_running_ = false;
  sdk_bootstrap_completed_ = true;

  // The documented SDK init program clears DSP DRAM before announcing that it
  // has finished.  IROM/COEF probes do not leave architecturally visible state
  // used by the PPC, so no fabricated ROM contents are needed here.
  dram_.fill(0);
  PostDspMailbox(kSdkBootstrapMail, false);
}

void NativeDSP::HandleSdkTaskLoaderMail(std::uint32_t mail) {
  // CPU mailbox mail is 31-bit. CMBH bit 15 (bit 31 of the combined host
  // value) is ownership/full state, not payload. Guest SDK code commonly
  // writes that bit set as part of its raw high half, so normalize at the
  // hardware boundary before decoding loader commands.
  const std::uint32_t payload = mail & 0x7fffffffu;
  last_cpu_mail_ = payload;
  ++cpu_mail_write_count_;
  if (!sdk_bootstrap_completed_) return;
  mail = payload;

  // SDK revisions differ slightly in the data words interleaved with the
  // 0x00F3xxxx loader commands. Treat the command words as structural
  // resynchronization points instead of requiring one brittle byte-for-byte
  // transcript. This still requires the standard D001 "start task" command
  // followed by a 16-bit DSP entry before synthesizing DSP_INIT.
  if (mail == kSdkTaskLoadCodeAddress) {
    sdk_task_loader_stage_ = 1u;
    sdk_task_code_address_ = 0u;
    sdk_task_code_size_ = 0u;
    sdk_task_entry_ = 0u;
    sdk_task_init_cycles_ = 0u;
    return;
  }
  if (mail == kSdkTaskLoadImemAddress) {
    if (sdk_task_loader_stage_ < 1u) sdk_task_loader_stage_ = 1u;
    sdk_task_loader_stage_ = 3u;
    return;
  }
  if (mail == kSdkTaskLoadCodeLength) {
    if (sdk_task_loader_stage_ < 1u) sdk_task_loader_stage_ = 1u;
    sdk_task_loader_stage_ = 5u;
    return;
  }
  if (mail == kSdkTaskLoadDmemLength) {
    if (sdk_task_loader_stage_ < 1u) sdk_task_loader_stage_ = 1u;
    sdk_task_loader_stage_ = 7u;
    return;
  }
  if (mail == kSdkTaskStartAddress) {
    // D001 is sufficiently distinctive and is the final loader command in
    // Nintendo's SDK task bootstrap. Accept it as a resync point so older SDKs
    // whose intermediate payload differs still reach the generic handshake.
    sdk_task_loader_stage_ = 9u;
    return;
  }

  switch (sdk_task_loader_stage_) {
  case 1:
    // MEM1 effective/physical address of task code. Do not make protocol
    // progress depend on the host mapping check: some SDKs pass a physical
    // address or stage the image while the mailbox sequence is in flight.
    sdk_task_code_address_ = mail;
    sdk_task_loader_stage_ = 2u;
    break;
  case 3:
    sdk_task_imem_address_ = static_cast<std::uint16_t>(mail);
    sdk_task_loader_stage_ = 4u;
    break;
  case 5:
    sdk_task_code_size_ = mail;
    sdk_task_loader_stage_ = 6u;
    break;
  case 7:
    if ((mail & 0xffff0000u) == 0u) {
      sdk_task_dmem_length_ = static_cast<std::uint16_t>(mail);
      sdk_task_loader_stage_ = 8u;
    }
    break;
  case 9:
    if ((mail & 0xffff0000u) == 0u) {
      sdk_task_entry_ = static_cast<std::uint16_t>(mail);
      sdk_task_loader_stage_ = 10u;

      // v50: known GameCube AX tasks take the ROM-free HLE fast path.  The
      // detector looks at the uploaded DSP image/protocol, never at game id.
      // Unknown/custom tasks continue to the standalone LLE core unchanged.
      const bool ax_started = ax_hle_.ProbeTask(sdk_task_code_address_, sdk_task_code_size_);
      bool lle_started = false;
      if (!ax_started) {
        lle_started = core_.LoadTask(
            sdk_task_code_address_, sdk_task_imem_address_,
            static_cast<std::uint16_t>(std::min<std::uint32_t>(sdk_task_code_size_, 0xffffu)),
            sdk_task_dmem_length_, sdk_task_entry_);
      } else {
        core_.SetRunning(false);
      }
      if (ax_started || !lle_started) {
        sdk_task_init_cycles_ = kSdkTaskInitPpcCycles;
        // With neither AX HLE nor a runnable LLE core, the task cannot emit
        // its first task-owned mailbox. Still model the SDK-visible startup
        // handshake: after __DSPHandler consumes DCD10000, expose one
        // non-interrupting ready mail for init callbacks such as JAudio's
        // DspHandShake. This fallback is never used when a real task backend
        // exists.
        sdk_romless_task_ready_pending_ = !ax_started && !lle_started;
        sdk_romless_task_active_ = sdk_romless_task_ready_pending_;
        sdk_romless_commands_ready_ = false;
        sdk_romless_command_words_expected_ = 0;
        sdk_romless_command_words_seen_ = 0;
        sdk_romless_work_token_ = 0;
        sdk_romless_completion_token_ = 0;
        sdk_romless_completion_cycles_ = 0;
        if (sdk_romless_task_ready_pending_) {
          std::fprintf(stderr,
                       "GEKKOAOT_NATIVE_DSP_ROMLESS_TASK_V56=1 code=%08x size=%u "
                       "entry=%04x policy=dcd1+ready-mail\n",
                       sdk_task_code_address_, sdk_task_code_size_, sdk_task_entry_);
        }
      } else {
        sdk_task_init_cycles_ = 0u;
        sdk_romless_task_ready_pending_ = false;
        sdk_romless_task_active_ = false;
        sdk_romless_commands_ready_ = false;
        sdk_romless_completion_cycles_ = 0u;
      }
    }
    break;
  default:
    break;
  }
}

void NativeDSP::FinishSdkTaskLaunch() {
  sdk_task_init_cycles_ = 0u;
  ++sdk_task_init_count_;

  // SDK ucodes signal that their task entry has started with DSP_INIT
  // (0xDCD10000). DMB bit 31 is the mailbox-full flag, so PostDspMailbox stores
  // the 31-bit payload and exposes the full state through DMBH bit 15.
  PostDspMailbox(kSdkTaskInitMail, true);
}

std::uint8_t NativeDSP::ReadAram(std::uint32_t address) const {
  return aram_[address & AramMask];
}

void NativeDSP::WriteAram(std::uint32_t address, std::uint8_t value) {
  aram_[address & AramMask] = value;
}

bool NativeDSP::Read16(std::uint32_t offset, std::uint16_t* value) {
  if (!value || (offset & 1u)) return false;

  switch (offset) {
  case kMailToDspHi: *value = static_cast<std::uint16_t>(cpu_mail_visible_ >> 16); return true;
  case kMailToDspLo: *value = static_cast<std::uint16_t>(cpu_mail_visible_); return true;
  case kMailFromDspHi:
    *value = static_cast<std::uint16_t>((dsp_mail_visible_ >> 16) & 0x7fffu);
    if (dsp_mail_pending_) *value |= 0x8000u;
    return true;
  case kMailFromDspLo: {
    *value = static_cast<std::uint16_t>(dsp_mail_visible_);
    // Hardware/HLE convention: after the low half is consumed, the busy bit in
    // the high word drops while the previous payload remains readable.
    const std::uint32_t consumed_mail = dsp_mail_visible_ & 0x7fffffffu;
    dsp_mail_visible_ &= 0x7fffffffu;
    dsp_mail_pending_ = false;

    // Unknown/custom task fallback for ROM-less operation. The SDK interrupt
    // handler consumes DCD10000 and may call an init callback before returning.
    // A real ucode normally posts its own ready word at that point. Queue a
    // mailbox-full zero payload without asserting a second interrupt so the
    // callback observes the same ordering and can finish initialization.
    if (sdk_romless_task_ready_pending_ && consumed_mail == kSdkTaskInitMail) {
      sdk_romless_task_ready_pending_ = false;
      sdk_romless_commands_ready_ = true;
      PostDspMailbox(0u, false);
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_DSP_ROMLESS_READY_V56=1 after=dcd10000 mail=00000000\n");
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_DSP_JAUDIO_ROMLESS_V59=1 phase=ready protocol=count+commands prefix=f355\n");
    } else if (consumed_mail == kSdkTaskCallbackMail &&
               sdk_romless_completion_payload_pending_ != 0u) {
      // The SDK task-control handler consumes DCD10004 first and only then
      // invokes task->callback (JAudio installs syncDSP there). Real JAudio
      // makes the F355|token work completion visible after that control mail
      // has been acknowledged, so stage the payload at this exact boundary.
      const auto payload = sdk_romless_completion_payload_pending_;
      sdk_romless_completion_payload_pending_ = 0u;
      PostDspMailbox(payload, false);
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_DSP_JAUDIO_ROMLESS_V61=1 phase=payload-after-dcd10004 mail=%08x irq=0 consumer=syncDSP\n",
                   payload);
    }
    return true;
  }
  case kControl: *value = control_; return true;
  case kInterruptControl: *value = interrupt_control_; return true;
  case kArInfo: *value = ar_info_; return true;
  case kArMode: *value = ar_mode_; return true;
  case kArRefresh: *value = ar_refresh_; return true;
  case kArDmaMmAddrHi: *value = static_cast<std::uint16_t>(aram_dma_mmaddr_ >> 16); return true;
  case kArDmaMmAddrLo: *value = static_cast<std::uint16_t>(aram_dma_mmaddr_); return true;
  case kArDmaArAddrHi: *value = static_cast<std::uint16_t>(aram_dma_araddr_ >> 16); return true;
  case kArDmaArAddrLo: *value = static_cast<std::uint16_t>(aram_dma_araddr_); return true;
  case kArDmaCountHi: *value = static_cast<std::uint16_t>(aram_dma_count_ >> 16); return true;
  case kArDmaCountLo: *value = static_cast<std::uint16_t>(aram_dma_count_); return true;
  case kAudioDmaStartHi: *value = static_cast<std::uint16_t>(audio_dma_source_ >> 16); return true;
  case kAudioDmaStartLo: *value = static_cast<std::uint16_t>(audio_dma_source_); return true;
  case kAudioDmaBlocksLength: *value = audio_dma_blocks_length_; return true;
  case kAudioDmaControlLen: *value = audio_dma_control_; return true;
  case kAudioDmaBlocksLeft:
    *value = audio_dma_remaining_blocks_ > 0 ?
                 static_cast<std::uint16_t>(audio_dma_remaining_blocks_ - 1u) : 0u;
    return true;
  default: return false;
  }
}

void NativeDSP::WriteControl(std::uint16_t value) {
  const bool init_was_set = (control_ & kDspInit) != 0;
  const bool halt_was_set = (control_ & kDspHalt) != 0;

  // Cause bits are write-one-to-clear while their neighbouring mask bits are
  // ordinary writable state.
  control_ &= static_cast<std::uint16_t>(~(value & kInterruptCauses));
  control_ = static_cast<std::uint16_t>((control_ & ~kControlWritableState) |
                                        (value & kControlWritableState));

  if (value & kDspReset) {
    cpu_mail_visible_ = 0;
    cpu_mail_pending_value_ = 0;
    cpu_mail_pending_ = false;
    dsp_mail_visible_ = 0;
    dsp_mail_pending_ = false;
    audio_dma_control_ = 0;
    audio_dma_remaining_blocks_ = 0;
    aid_interrupt_cycles_ = 0;
    core_.Reset();
    // Reset self-clears and returns the native core to halted/init state.
    control_ &= static_cast<std::uint16_t>(~kDspReset);
    control_ |= kDspHalt | kDspInit;

    // With DSPInit asserted, reset performs the hardware bootstrap DMA from
    // physical 0x01000000 into DSP instruction RAM.
    if (value & kDspInit) LoadInitProgramFromMainMemory();
  }

  // DSPAssertInt/CR_EXTERNAL_INT is an edge from the Gekko into the DSP core.
  // Deliver it independently of the CPU mailbox; Zelda-family ucodes rely on
  // the external interrupt vector instead of polling CMB forever. Match LLE
  // ordering: RESET/control state is applied before the external interrupt.
  if ((value & kDspAssertInt) != 0u && core_.Available())
    core_.NotifyExternalInterrupt();

  // Licensed software transitions DSPInit 1->0 while loading the init ucode.
  // Keep the externally visible init-code pulse; the actual ucode execution is
  // intentionally left for the native DSP core, not Dolphin HLE.
  if (init_was_set && (control_ & kDspInit) == 0u) {
    control_ |= kDspInitCode;
    init_code_clear_cycles_ = 130u;
  }

  // Clearing HALT starts execution from the freshly loaded SDK bootstrap.
  // This is intentionally delayed; the PPC immediately polls DMBH and should
  // observe an empty mailbox for a while before the DSP finishes its work.
  if (halt_was_set && (control_ & kDspHalt) == 0u) {
    StartSdkBootstrap();
    if (sdk_task_loader_stage_ >= 10u && core_.Available()) core_.SetRunning(true);
  }
  if ((control_ & kDspHalt) != 0u) core_.SetRunning(false);
}

void NativeDSP::RunAramDma() {
  const std::uint32_t count = aram_dma_count_ & 0x7fffffffu;
  const bool aram_to_main = (aram_dma_count_ & 0x80000000u) != 0u;
  if (count == 0u) return;

  control_ |= kDmaState;

  std::uint32_t main_address = aram_dma_mmaddr_ & kAramAddressMask;
  std::uint32_t aram_address = aram_dma_araddr_ & kAramAddressMask;
  std::uint32_t remaining = count;

  while (remaining != 0u) {
    const std::uint32_t chunk = std::min<std::uint32_t>(remaining, 32u);
    const std::uint32_t wrapped = aram_address & AramMask;
    const std::uint32_t contiguous = std::min<std::uint32_t>(chunk, AramSize - wrapped);
    std::uint8_t* main = memory_ ? memory_->Resolve(main_address, contiguous) : nullptr;
    if (!main) break;

    if (aram_to_main)
      std::memcpy(main, aram_.data() + wrapped, contiguous);
    else
      std::memcpy(aram_.data() + wrapped, main, contiguous);

    main_address += contiguous;
    aram_address += contiguous;
    remaining -= contiguous;
  }

  aram_dma_mmaddr_ = main_address;
  aram_dma_araddr_ = aram_address;
  aram_dma_count_ = (aram_to_main ? 0x80000000u : 0u) | remaining;
  control_ &= static_cast<std::uint16_t>(~kDmaState);
  GenerateInterrupt(kAram);
}

void NativeDSP::StartAudioDma(std::uint16_t value) {
  const bool already_enabled = (audio_dma_control_ & 0x8000u) != 0u;
  audio_dma_control_ = value;
  if (!already_enabled && (audio_dma_control_ & 0x8000u) != 0u) {
    audio_dma_current_source_ = audio_dma_source_;
    audio_dma_remaining_blocks_ = static_cast<std::uint16_t>(audio_dma_control_ & 0x7fffu);
    audio_sample_phase_ = 0;
    audio_frames_into_block_ = 0;
    audio_dma_pending_blocks_ = 0;

    // A zero-length DMA has no useful PCM payload.  Feeding the same 32-byte
    // address every 8 samples turns a transient/stale word into a ~4 kHz
    // repeated "bop".  Keep the register/interrupt behavior visible to the
    // guest, but do not send stale memory to the host audio device until a
    // non-zero transfer length is programmed.
    if (audio_dma_remaining_blocks_ == 0u) {
      audio_sink_.Flush();
      std::fprintf(stderr,
                   "GEKKOAOT_AUDIO_DMA_V51_1=1 source=%08x blocks=0 action=mute-zero-length\n",
                   audio_dma_source_);
    }

    // Hardware raises AID shortly after the first transfer is latched.
    aid_interrupt_cycles_ = 200u;
  }
}

void NativeDSP::SubmitAudioBlock() {
  if ((audio_dma_control_ & 0x8000u) == 0u) return;
  std::array<std::int16_t, 16> pcm{};
  if (memory_) {
    if (const auto* src = memory_->Resolve(audio_dma_current_source_, 32u)) {
      // AID memory is big-endian and stores stereo as R,L pairs. SDL wants
      // native-endian L,R pairs, so endian-convert and swap the channels here.
      for (std::size_t frame = 0; frame < 8u; ++frame) {
        const auto load_be_s16 = [&](std::size_t byte_off) {
          const std::uint16_t be = static_cast<std::uint16_t>(
              (static_cast<std::uint16_t>(src[byte_off]) << 8u) | src[byte_off + 1u]);
          return static_cast<std::int16_t>(be);
        };
        const std::int16_t right = load_be_s16(frame * 4u + 0u);
        const std::int16_t left = load_be_s16(frame * 4u + 2u);
        pcm[frame * 2u + 0u] = left;
        pcm[frame * 2u + 1u] = right;
      }
    }
  }
  audio_sink_.PushStereoS16(pcm.data(), 8u, audio_sample_rate_hz_);
  ++audio_blocks_submitted_;

  if (audio_dma_remaining_blocks_ != 0u) {
    --audio_dma_remaining_blocks_;
    audio_dma_current_source_ += 32u;
  }
  if (audio_dma_remaining_blocks_ == 0u) {
    audio_dma_current_source_ = audio_dma_source_;
    audio_dma_remaining_blocks_ = static_cast<std::uint16_t>(audio_dma_control_ & 0x7fffu);
    GenerateInterrupt(kAid);
  }
}

void NativeDSP::AdvanceAudioDma(std::uint64_t cycles) {
  if ((audio_dma_control_ & 0x8000u) == 0u || (audio_dma_control_ & 0x7fffu) == 0u ||
      cycles == 0u || audio_sample_rate_hz_ == 0u)
    return;

  // Convert elapsed GameCube hardware time to the 8-frame granularity of the
  // AID FIFO, but keep the resulting blocks as debt first.  AdvanceRuntimeCycles
  // is driven by host monotonic time and can hand us >5 ms at once.  The old
  // loop immediately consumed every elapsed block, potentially wrapping the
  // programmed DMA buffer multiple times before the PPC got a chance to service
  // INT_AID.  Since INT_AID is a level bit, those completions collapsed into
  // one interrupt: the host consumed audio faster than AX command lists were
  // produced and replayed stale 5 ms buffers (harsh/robotic sound).
  constexpr std::uint64_t kCpuClockHz = 486000000ull;
  const std::uint64_t whole = cycles / kCpuClockHz;
  const std::uint64_t rem = cycles % kCpuClockHz;
  std::uint64_t frames = whole * audio_sample_rate_hz_;
  const std::uint64_t phase = audio_sample_phase_ + rem * audio_sample_rate_hz_;
  frames += phase / kCpuClockHz;
  audio_sample_phase_ = phase % kCpuClockHz;
  frames += audio_frames_into_block_;

  audio_dma_pending_blocks_ += frames / 8u;
  audio_frames_into_block_ = static_cast<std::uint32_t>(frames % 8u);

  // The SDK requests an initial AID interrupt shortly after enabling DMA.  Do
  // not run past that scheduled guest-visible event just because the enclosing
  // realtime step was much larger than 200 PPC cycles.
  if (aid_interrupt_cycles_ != 0u) return;

  // When a masked-in AID cause is already asserted, let the PPC acknowledge it
  // before crossing the next DMA-wrap boundary.  This serializes guest-visible
  // completions without changing the underlying 32.028 kHz clock; elapsed time
  // remains queued in audio_dma_pending_blocks_ and catches up afterwards.
  const bool aid_irq_asserted = (control_ & kAid) != 0u && (control_ & kAidMask) != 0u;
  if (aid_irq_asserted) return;

  while (audio_dma_pending_blocks_ != 0u) {
    SubmitAudioBlock();
    --audio_dma_pending_blocks_;

    // SubmitAudioBlock raises INT_AID exactly when the current NumBlocks
    // transfer wraps.  Stop at that event boundary so two or more callbacks
    // can never collapse into the same level interrupt during one host step.
    if ((control_ & kAid) != 0u && (control_ & kAidMask) != 0u) break;
  }

  if (!audio_dma_event_mode_reported_) {
    audio_dma_event_mode_reported_ = true;
    std::fprintf(stderr,
                 "GEKKOAOT_AUDIO_DMA_V51_7=1 mode=irq-serialized rate_hz=%u "
                 "block_frames=8 pending_blocks=%llu\n",
                 audio_sample_rate_hz_,
                 static_cast<unsigned long long>(audio_dma_pending_blocks_));
  }
}

bool NativeDSP::CoreReadMain(std::uint32_t address, std::uint8_t* dst, std::uint32_t size) const {
  if (!memory_ || !dst || size == 0u) return false;
  const auto* p = memory_->Resolve(address, size);
  if (!p) return false;
  std::memcpy(dst, p, size);
  return true;
}

bool NativeDSP::CoreWriteMain(std::uint32_t address, const std::uint8_t* src, std::uint32_t size) {
  if (!memory_ || !src || size == 0u) return false;
  auto* p = memory_->Resolve(address, size);
  if (!p) return false;
  std::memcpy(p, src, size);
  return true;
}

void NativeDSP::RequestCoreInterrupt() { GenerateInterrupt(kDsp); }

bool NativeDSP::Write16(std::uint32_t offset, std::uint16_t value) {
  if (offset & 1u) return false;

  switch (offset) {
  case kMailToDspHi:
    cpu_mail_visible_ = (cpu_mail_visible_ & 0x0000ffffu) |
                        (static_cast<std::uint32_t>(value) << 16);
    return true;
  case kMailToDspLo: {
    cpu_mail_visible_ = (cpu_mail_visible_ & 0xffff0000u) | value;
    cpu_mail_visible_ |= 0x80000000u;
    cpu_mail_pending_value_ = cpu_mail_visible_;
    cpu_mail_pending_ = true;
    HandleSdkTaskLoaderMail(cpu_mail_pending_value_);

    // AX HLE consumes the same raw mailbox value the real DSP sees. Keep bit
    // 31 here: for 0xBABE/0xCDD1 messages that bit is both part of the visible
    // high word and the mailbox-full state on the MMIO side.
    const bool ax_consumed = ax_hle_.HandleCpuMail(cpu_mail_pending_value_);
    if (ax_consumed) cpu_mail_pending_ = false;

    if (sdk_romless_task_active_ && sdk_romless_commands_ready_ &&
        sdk_task_loader_stage_ >= 10u) {
      const std::uint32_t raw_mail = cpu_mail_pending_value_;
      const std::uint16_t high = static_cast<std::uint16_t>(raw_mail >> 16u);
      const std::uint16_t low = static_cast<std::uint16_t>(raw_mail);

      auto fixed_packet_words = [](std::uint16_t token) -> std::uint8_t {
        switch (token & 0xff00u) {
        case 0x8100u: return 5u; // setup: voices, VPB, SRC table, AFC table, FX
        case 0x8200u: return 3u; // sync frame: descriptor, left, right
        case 0x8000u: return 1u; // wait frame
        case 0x8b00u: return 2u; // IPL secondary command
        case 0x8c00u: return 2u; // AGB secondary command
        default: return 0u;
        }
      };

      if (sdk_romless_command_words_expected_ == 0u) {
        // Newer task wrappers prefix the packet with a count. Older JAudio
        // builds stream the fixed-size command directly. Support both forms;
        // command recognition is protocol-based and contains no title check.
        if (high == 0x8000u && low > 0u && low <= sdk_romless_packet_words_.size()) {
          sdk_romless_command_words_expected_ = static_cast<std::uint8_t>(low);
          sdk_romless_command_words_seen_ = 0u;
          sdk_romless_work_token_ = 0u;
          sdk_romless_packet_count_prefixed_ = true;
        } else if (const std::uint8_t words = fixed_packet_words(high); words != 0u) {
          sdk_romless_command_words_expected_ = words;
          sdk_romless_command_words_seen_ = 1u;
          sdk_romless_work_token_ = high;
          sdk_romless_packet_count_prefixed_ = false;
          sdk_romless_packet_words_[0] = raw_mail;
        }
      } else {
        const std::size_t index = sdk_romless_command_words_seen_;
        if (index < sdk_romless_packet_words_.size()) {
          // CMBH bit 15 is mailbox-full. On payload words this strips cached
          // PPC pointers to the physical address seen by the DSP. The first
          // command word keeps bit 31 because 0x8xxx is the command family.
          sdk_romless_packet_words_[index] = index == 0u ? raw_mail : (raw_mail & 0x7fffffffu);
        }
        if (sdk_romless_command_words_seen_ == 0u)
          sdk_romless_work_token_ = high;
        ++sdk_romless_command_words_seen_;
      }

      if (sdk_romless_command_words_expected_ != 0u &&
          sdk_romless_command_words_seen_ >= sdk_romless_command_words_expected_) {
        const auto token = sdk_romless_work_token_;
        const bool handled = jaudio_hle_.HandlePacket(
            sdk_romless_packet_words_.data(), sdk_romless_command_words_expected_);
        if (handled) cpu_mail_pending_ = false;

        // Count-prefixed JAudio uses the SDK task callback for setup work;
        // sync-frame is asynchronous there. The older direct protocol polls a
        // completion mailbox after each command, so expose F355|token directly.
        const bool needs_completion =
            sdk_romless_packet_count_prefixed_ ? token == kJAudioSetupWorkToken : handled;
        if (needs_completion && sdk_romless_completion_cycles_ == 0u) {
          sdk_romless_completion_token_ = token;
          sdk_romless_completion_callback_mode_ = sdk_romless_packet_count_prefixed_;
          sdk_romless_completion_cycles_ = kJAudioRomlessCompletionPpcCycles;
          std::fprintf(stderr,
                       "GEKKOAOT_NATIVE_DSP_JAUDIO_V69=1 phase=schedule token=%04x mode=%s delay=%llu handled=%u\n",
                       token, sdk_romless_completion_callback_mode_ ? "task-callback" : "direct",
                       static_cast<unsigned long long>(sdk_romless_completion_cycles_), handled ? 1u : 0u);
        }
        sdk_romless_command_words_expected_ = 0u;
        sdk_romless_command_words_seen_ = 0u;
        sdk_romless_work_token_ = 0u;
        sdk_romless_packet_words_.fill(0u);
      }
    }

    // During the structural ROM-loader handshake or HLE execution GekkoAOT
    // consumes the mail itself, so acknowledge immediately. LLE keeps mailbox
    // ownership asserted until its DSP core reads CMBL.
    if (sdk_task_loader_stage_ < 10u || ax_consumed || ax_hle_.Active() || jaudio_hle_.Active() || !core_.Available())
      cpu_mail_visible_ &= 0x7fffffffu;
    return true;
  }
  case kMailFromDspHi:
  case kMailFromDspLo:
  case kArMode:
  case kAudioDmaBlocksLeft:
    return false;
  case kControl: WriteControl(value); return true;
  case kInterruptControl: interrupt_control_ = value; return true;
  case kArInfo: ar_info_ = value & kArInfoMask; return true;
  case kArRefresh: ar_refresh_ = value & kArRefreshMask; return true;
  case kArDmaMmAddrHi:
    aram_dma_mmaddr_ = (aram_dma_mmaddr_ & 0x0000ffffu) |
                       (static_cast<std::uint32_t>(value & kHighAddressMask) << 16);
    return true;
  case kArDmaMmAddrLo:
    aram_dma_mmaddr_ = (aram_dma_mmaddr_ & 0xffff0000u) | (value & kAlignedLowMask);
    return true;
  case kArDmaArAddrHi:
    aram_dma_araddr_ = (aram_dma_araddr_ & 0x0000ffffu) |
                       (static_cast<std::uint32_t>(value & kHighAddressMask) << 16);
    return true;
  case kArDmaArAddrLo:
    aram_dma_araddr_ = (aram_dma_araddr_ & 0xffff0000u) | (value & kAlignedLowMask);
    return true;
  case kArDmaCountHi:
    aram_dma_count_ = (aram_dma_count_ & 0x0000ffffu) |
                      (static_cast<std::uint32_t>(value & 0x83ffu) << 16);
    return true;
  case kArDmaCountLo:
    aram_dma_count_ = (aram_dma_count_ & 0xffff0000u) | (value & kAlignedLowMask);
    RunAramDma();
    return true;
  case kAudioDmaStartHi:
    audio_dma_source_ = (audio_dma_source_ & 0x0000ffffu) |
                        (static_cast<std::uint32_t>(value & kAudioHighMaskGc) << 16);
    return true;
  case kAudioDmaStartLo:
    audio_dma_source_ = (audio_dma_source_ & 0xffff0000u) | (value & kAlignedLowMask);
    return true;
  case kAudioDmaBlocksLength:
    audio_dma_blocks_length_ = value;
    return true;
  case kAudioDmaControlLen:
    StartAudioDma(value);
    return true;
  default: return false;
  }
}

bool NativeDSP::Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value) {
  if (!value || !Handles(address)) return false;
  const std::uint32_t offset = ToPhysical(address) - BasePhysical;

  if (size == 2 && (offset & 1u) == 0u) {
    std::uint16_t result = 0;
    if (!Read16(offset, &result)) return false;
    *value = result;
    return true;
  }

  if (size == 4 && (offset & 3u) == 0u) {
    std::uint16_t hi = 0, lo = 0;
    if (!Read16(offset, &hi) || !Read16(offset + 2u, &lo)) return false;
    *value = (static_cast<std::uint32_t>(hi) << 16) | lo;
    return true;
  }

  return false;
}

bool NativeDSP::Write(std::uint32_t address, std::uint64_t value, std::uint8_t size) {
  if (!Handles(address)) return false;
  const std::uint32_t offset = ToPhysical(address) - BasePhysical;

  if (size == 2 && (offset & 1u) == 0u)
    return Write16(offset, static_cast<std::uint16_t>(value));

  if (size == 4 && (offset & 3u) == 0u) {
    const auto hi = static_cast<std::uint16_t>((value >> 16) & 0xffffu);
    const auto lo = static_cast<std::uint16_t>(value & 0xffffu);
    // DSP registers are 16-bit. Keep hardware write ordering: high-address
    // halfword first, then low-address halfword.
    return Write16(offset, hi) && Write16(offset + 2u, lo);
  }

  return false;
}

void NativeDSP::AdvanceCycles(std::uint64_t cycles) {
  if (cycles == 0u) return;

  core_.AdvancePpcCycles(cycles);
  ax_hle_.AdvancePpcCycles(cycles);
  AdvanceAudioDma(cycles);

  if (init_code_clear_cycles_ != 0u) {
    if (cycles >= init_code_clear_cycles_) {
      init_code_clear_cycles_ = 0;
      control_ &= static_cast<std::uint16_t>(~kDspInitCode);
    } else {
      init_code_clear_cycles_ -= cycles;
    }
  }

  if (aid_interrupt_cycles_ != 0u) {
    if (cycles >= aid_interrupt_cycles_) {
      aid_interrupt_cycles_ = 0;
      GenerateInterrupt(kAid);
    } else {
      aid_interrupt_cycles_ -= cycles;
    }
  }

  if (sdk_bootstrap_cycles_ != 0u) {
    if (cycles >= sdk_bootstrap_cycles_)
      FinishSdkBootstrap();
    else
      sdk_bootstrap_cycles_ -= cycles;
  }

  if (sdk_task_init_cycles_ != 0u) {
    if (cycles >= sdk_task_init_cycles_)
      FinishSdkTaskLaunch();
    else
      sdk_task_init_cycles_ -= cycles;
  }

  if (sdk_romless_completion_cycles_ != 0u) {
    if (cycles >= sdk_romless_completion_cycles_) {
      // Do not overwrite a guest-visible DSP mail. Retry on the next hardware
      // advance boundary if another mailbox is still pending.
      if (!dsp_mail_pending_) {
        const auto token = sdk_romless_completion_token_;
        sdk_romless_completion_cycles_ = 0u;
        sdk_romless_completion_token_ = 0u;
        const auto payload = kJAudioDspPrefix | token;
        if (sdk_romless_completion_callback_mode_) {
          // Count-prefixed task protocol: task callback first, then the F355
          // work payload becomes visible when DCD10004 is consumed.
          sdk_romless_completion_payload_pending_ = payload;
          PostDspMailbox(kSdkTaskCallbackMail, true);
          std::fprintf(stderr,
                       "GEKKOAOT_NATIVE_DSP_JAUDIO_V69=1 phase=callback-irq token=%04x control=dcd10004 payload=%08x\n",
                       token, payload);
        } else {
          // Older direct JAudio waits synchronously for one task-owned reply.
          PostDspMailbox(payload, false);
          std::fprintf(stderr,
                       "GEKKOAOT_NATIVE_DSP_JAUDIO_V69=1 phase=direct-complete token=%04x payload=%08x\n",
                       token, payload);
        }
      } else {
        sdk_romless_completion_cycles_ = 1u;
      }
    } else {
      sdk_romless_completion_cycles_ -= cycles;
    }
  }
}

} // namespace GekkoAOT::DSP
