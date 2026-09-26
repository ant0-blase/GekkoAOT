// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/di/native_di.h"

#include <algorithm>
#include <cstdio>

namespace GekkoAOT::HW::DI {

namespace {
constexpr std::uint8_t kCmdSeek = 0xabu;
constexpr std::uint8_t kCmdRequestError = 0xe0u;
constexpr std::uint8_t kCmdAudioStream = 0xe1u;
constexpr std::uint8_t kCmdRequestAudioStatus = 0xe2u;
constexpr std::uint8_t kCmdStopMotor = 0xe3u;
constexpr std::uint8_t kCmdAudioBufferConfig = 0xe4u;
constexpr std::uint32_t kInvalidCommandError = 0x00052000u;
constexpr std::uint32_t kNoMediumError = 0x00023a00u;
}

void NativeDI::Reset(bool disc_present) {
  ++command_sequence_; // Retire any completion scheduled before the reset.
  status_ = 0;
  disc_present_ = disc_present;
  cover_ = disc_present ? 0u : kCoverOpen;
  command_ = {};
  dma_address_ = 0;
  dma_length_ = 0;
  dma_control_ = 0;
  immediate_ = 0;
  config_ = 1u; // GameCube: boot-ROM descrambler disabled after IPL/BS2 setup.
  last_error_ = 0;
  last_opcode_ = 0;
  audio_enabled_ = false;
  audio_streaming_ = false;
  audio_stop_at_track_end_ = false;
  audio_buffer_size_ = 0;
  audio_current_start_ = 0;
  audio_current_length_ = 0;
  audio_position_ = 0;
  audio_next_start_ = 0;
  audio_next_length_ = 0;
}

bool NativeDI::Handles(std::uint32_t address) const {
  const std::uint32_t physical = ToPhysical(address);
  return physical >= BasePhysical && physical < BasePhysical + WindowSize;
}

bool NativeDI::Read32(std::uint32_t offset, std::uint32_t* value) const {
  if (!value) return false;
  switch (offset) {
  case kStatus: *value = status_; return true;
  case kCover: *value = cover_; return true;
  case kCommand0: *value = command_[0]; return true;
  case kCommand1: *value = command_[1]; return true;
  case kCommand2: *value = command_[2]; return true;
  case kDmaAddress: *value = dma_address_; return true;
  case kDmaLength: *value = dma_length_; return true;
  case kDmaControl: *value = dma_control_; return true;
  case kImmediate: *value = immediate_; return true;
  case kConfig: *value = config_; return true;
  default: return false;
  }
}

bool NativeDI::Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value) const {
  if (!Handles(address) || !value || size != 4u) return false;
  const std::uint32_t offset = ToPhysical(address) - BasePhysical;
  if (offset & 3u) return false;
  std::uint32_t word = 0;
  if (!Read32(offset, &word)) return false;
  *value = word;
  return true;
}

bool NativeDI::InterruptPending() const {
  return ((status_ & kDeviceErrorInt) && (status_ & kDeviceErrorMask)) ||
         ((status_ & kTransferCompleteInt) && (status_ & kTransferCompleteMask)) ||
         ((status_ & kBreakInt) && (status_ & kBreakMask)) ||
         ((cover_ & kCoverInt) && (cover_ & kCoverIntMask));
}

void NativeDI::SetDiscPresent(bool present) {
  if (disc_present_ == present) return;
  disc_present_ = present;
  if (present)
    cover_ &= ~kCoverOpen;
  else
    cover_ |= kCoverOpen;
  cover_ |= kCoverInt;
}

void NativeDI::FinishCommand(bool success, std::uint32_t immediate,
                             std::uint32_t bytes_transferred,
                             std::uint32_t error_code) {
  immediate_ = immediate;
  dma_control_ &= ~kControlTStart;

  if (success) {
    if (dma_control_ & kControlDma) {
      const std::uint32_t transferred = std::min(bytes_transferred, dma_length_);
      dma_address_ = (dma_address_ + transferred) & kGcDmaAddressMask;
      dma_length_ -= transferred;
    }
    status_ |= kTransferCompleteInt;
  } else {
    last_error_ = error_code;
    status_ |= kDeviceErrorInt;
  }
}

void NativeDI::CompletePending(bool success, std::uint32_t immediate,
                               std::uint32_t bytes_transferred,
                               std::uint32_t error_code) {
  CompletePendingFor(command_sequence_, success, immediate, bytes_transferred, error_code);
}

bool NativeDI::CompletePendingFor(std::uint64_t sequence, bool success,
                                  std::uint32_t immediate,
                                  std::uint32_t bytes_transferred,
                                  std::uint32_t error_code) {
  if (sequence != command_sequence_ || !(dma_control_ & kControlTStart)) return false;
  FinishCommand(success, immediate, bytes_transferred, error_code);
  return true;
}

void NativeDI::ExecuteCommand() {
  const std::uint8_t opcode = static_cast<std::uint8_t>(command_[0] >> 24);
  last_opcode_ = opcode;

  // The disc reader is a separate provider. It may still have an image
  // mounted after the guest opens the drive cover, but that image is no longer
  // accessible through disc read or seek commands.
  if (!disc_present_ && (opcode == 0xa8u || opcode == kCmdSeek)) {
    FinishCommand(false, immediate_, 0u, kNoMediumError);
    return;
  }

  // Commands that are entirely register/state based stay owned by DI even when
  // a media provider is attached. This also keeps DeviceHooks focused on the
  // actual disc byte boundary instead of making every backend emulate drive
  // bookkeeping such as RequestError.
  if (opcode == kCmdRequestError) {
    const std::uint32_t error = last_error_;
    last_error_ = 0;
    FinishCommand(true, error, 0, 0);
    return;
  }
  if (opcode == kCmdSeek && disc_present_) {
    FinishCommand(true, immediate_, 0, 0);
    return;
  }
  if (opcode == kCmdAudioBufferConfig) {
    // E4: bit 16 enables the drive's DTK audio path; low nibble selects the
    // hardware audio-buffer size. Keep this register/drive state even though
    // actual ADPCM decoding is not part of NativeDI.
    audio_enabled_ = ((command_[0] >> 16u) & 1u) != 0u;
    audio_buffer_size_ = static_cast<std::uint8_t>(command_[0] & 0x0fu);
    if (!audio_enabled_) {
      audio_streaming_ = false;
      audio_stop_at_track_end_ = false;
    }
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_DI_DTK_V56=1 cmd=E4 enabled=%u buffer=%u\n",
                 audio_enabled_ ? 1u : 0u, static_cast<unsigned>(audio_buffer_size_));
    FinishCommand(true, 0u, 0u, 0u);
    return;
  }
  if (opcode == kCmdAudioStream) {
    // E1: subcommand 0 starts/queues a DTK stream, subcommand 1 stops it.
    // Offsets are encoded in 32-bit words just like DVDLowRead.
    if (!audio_enabled_ || !disc_present_) {
      FinishCommand(false, immediate_, 0u, 0x00052000u);
      return;
    }
    const std::uint8_t subcommand = static_cast<std::uint8_t>((command_[0] >> 16u) & 0xffu);
    if (subcommand == 0u) {
      const std::uint64_t offset = static_cast<std::uint64_t>(command_[1]) << 2u;
      const std::uint32_t length = command_[2];
      if (offset == 0u && length == 0u) {
        audio_stop_at_track_end_ = true;
      } else if (!audio_stop_at_track_end_) {
        audio_next_start_ = offset;
        audio_next_length_ = length;
        if (!audio_streaming_) {
          audio_current_start_ = audio_next_start_;
          audio_current_length_ = audio_next_length_;
          audio_position_ = audio_current_start_;
          audio_streaming_ = true;
        }
      }
    } else if (subcommand == 1u) {
      audio_stop_at_track_end_ = false;
      audio_streaming_ = false;
    } else {
      FinishCommand(false, immediate_, 0u, 0x00052000u);
      return;
    }
    std::fprintf(stderr,
                 "GEKKOAOT_NATIVE_DI_DTK_V56=1 cmd=E1 sub=%u streaming=%u start=%08llx length=%u\n",
                 static_cast<unsigned>(subcommand), audio_streaming_ ? 1u : 0u,
                 static_cast<unsigned long long>(audio_current_start_), audio_current_length_);
    FinishCommand(true, 0u, 0u, 0u);
    return;
  }
  if (opcode == kCmdRequestAudioStatus) {
    // E2 returns status through DI_IMMBUF. The position is intentionally kept
    // at the current track start until a DTK decoder owns stream timing; games
    // still receive correct enabled/running/start/length state and command
    // completion instead of DEINT/InvalidCommand.
    if (!audio_enabled_ || !disc_present_) {
      FinishCommand(false, immediate_, 0u, 0x00052000u);
      return;
    }
    const std::uint8_t subcommand = static_cast<std::uint8_t>((command_[0] >> 16u) & 0xffu);
    std::uint32_t result = 0u;
    switch (subcommand) {
    case 0u: result = audio_streaming_ ? 1u : 0u; break;
    case 1u: result = static_cast<std::uint32_t>((audio_position_ & ~0x7fffull) >> 2u); break;
    case 2u: result = static_cast<std::uint32_t>(audio_current_start_ >> 2u); break;
    case 3u: result = audio_current_length_; break;
    default:
      FinishCommand(false, immediate_, 0u, 0x00052000u);
      return;
    }
    static unsigned audio_status_logs = 0;
    if (audio_status_logs < 16u) {
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_DI_DTK_V56=1 cmd=E2 sub=%u result=%08x streaming=%u\n",
                   static_cast<unsigned>(subcommand), result, audio_streaming_ ? 1u : 0u);
      ++audio_status_logs;
    }
    FinishCommand(true, result, 0u, 0u);
    return;
  }
  if (opcode == kCmdStopMotor) {
    // DVDLowStopMotor is a drive-state command, not a media read. Retail GC
    // software expects it to complete normally through TCINT. Bits 17 and 20
    // request eject and kill respectively; only eject-without-kill opens the
    // cover. The common E3000000 command simply stops the motor.
    const bool eject = (command_[0] & (1u << 17)) != 0u;
    const bool kill = (command_[0] & (1u << 20)) != 0u;
    if (eject && !kill) SetDiscPresent(false);
    FinishCommand(true, immediate_, 0, 0);
    return;
  }

  // A backend owns commands that need real media bytes. Keeping this as a
  // narrow request/response boundary lets the standalone host use a raw or
  // cached logical disc without routing execution through Dolphin's DVD stack.
  if (hooks_.execute) {
    const CommandRequest request{command_, dma_address_, dma_length_, dma_control_,
                                 immediate_, command_sequence_};
    const CommandResponse response = hooks_.execute(hooks_.user, request);
    if (response.result == 0) return;
    FinishCommand(response.result > 0, response.immediate,
                  response.bytes_transferred, response.error_code);
    return;
  }

  FinishCommand(false, immediate_, 0,
                disc_present_ ? kInvalidCommandError : kNoMediumError);
}

bool NativeDI::Write32(std::uint32_t offset, std::uint32_t value) {
  switch (offset) {
  case kStatus: {
    constexpr std::uint32_t writable_state =
        kBreak | kDeviceErrorMask | kTransferCompleteMask | kBreakMask;
    status_ = (status_ & ~writable_state) | (value & writable_state);
    if (value & kDeviceErrorInt) status_ &= ~kDeviceErrorInt;
    if (value & kTransferCompleteInt) status_ &= ~kTransferCompleteInt;
    if (value & kBreakInt) status_ &= ~kBreakInt;
    if (value & kBreak) {
      dma_control_ &= ~kControlTStart;
      status_ |= kBreakInt;
    }
    return true;
  }

  case kCover:
    cover_ = (cover_ & ~(kCoverIntMask)) | (value & kCoverIntMask) |
             (disc_present_ ? 0u : kCoverOpen) | (cover_ & kCoverInt);
    if (value & kCoverInt) cover_ &= ~kCoverInt;
    return true;

  case kCommand0: command_[0] = value; return true;
  case kCommand1: command_[1] = value; return true;
  case kCommand2: command_[2] = value; return true;
  case kDmaAddress: dma_address_ = value & kGcDmaAddressMask; return true;
  case kDmaLength: dma_length_ = value & kDmaLengthMask; return true;

  case kDmaControl:
    dma_control_ = value & (kControlTStart | kControlDma | kControlWrite);
    if (dma_control_ & kControlTStart) {
      ++command_sequence_;
      ExecuteCommand();
    }
    return true;

  case kImmediate: immediate_ = value; return true;
  case kConfig:
    // DI_CONFIG is read-only on retail GameCube hardware.
    return false;
  default:
    return false;
  }
}

bool NativeDI::Write(std::uint32_t address, std::uint64_t value, std::uint8_t size) {
  if (!Handles(address) || size != 4u) return false;
  const std::uint32_t offset = ToPhysical(address) - BasePhysical;
  if (offset & 3u) return false;
  return Write32(offset, static_cast<std::uint32_t>(value));
}

} // namespace GekkoAOT::HW::DI
