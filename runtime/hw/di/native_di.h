#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdint>

namespace GekkoAOT::HW::DI {

// Standalone GameCube DVD Interface (DI) register block.
//
// This owns only the Flipper-side DI registers and interrupt semantics. The
// actual disc/container reader is intentionally kept behind DeviceHooks so a
// raw ISO reader, cached-disc provider or future native RVZ backend can be
// attached without routing execution through Dolphin's DVDInterface/DVDThread.
class NativeDI final {
public:
  static constexpr std::uint32_t BasePhysical = 0x0c006000u;
  static constexpr std::uint32_t WindowSize = 0x28u;

  struct CommandRequest {
    std::array<std::uint32_t, 3> command{};
    std::uint32_t dma_address = 0;
    std::uint32_t dma_length = 0;
    std::uint32_t control = 0;
    std::uint32_t immediate = 0;
    // Identifies the transfer that the media provider must complete. A later
    // command can reuse the same registers after this one was cancelled.
    std::uint64_t sequence = 0;
  };

  struct CommandResponse {
    // >0 complete successfully, 0 pending/asynchronous, <0 device error.
    int result = -1;
    std::uint32_t immediate = 0;
    std::uint32_t bytes_transferred = 0;
    std::uint32_t error_code = 0x00052000u; // invalid command by default
  };

  struct DeviceHooks {
    void* user = nullptr;
    CommandResponse (*execute)(void* user, const CommandRequest& request) = nullptr;
  };

  NativeDI() { Reset(); }

  void Reset(bool disc_present = true);
  bool Handles(std::uint32_t address) const;
  bool Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value) const;
  bool Write(std::uint32_t address, std::uint64_t value, std::uint8_t size);

  void SetDeviceHooks(DeviceHooks hooks) { hooks_ = hooks; }
  void SetDiscPresent(bool present);
  void CompletePending(bool success, std::uint32_t immediate = 0,
                       std::uint32_t bytes_transferred = 0,
                       std::uint32_t error_code = 0x00031100u);
  bool CompletePendingFor(std::uint64_t sequence, bool success,
                          std::uint32_t immediate = 0,
                          std::uint32_t bytes_transferred = 0,
                          std::uint32_t error_code = 0x00031100u);

  bool InterruptPending() const;
  bool DiscPresent() const { return disc_present_; }
  std::uint32_t Status() const { return status_; }
  std::uint32_t Cover() const { return cover_; }
  std::uint32_t Config() const { return config_; }
  std::uint32_t DMAAddress() const { return dma_address_; }
  std::uint32_t DMALength() const { return dma_length_; }
  std::uint32_t DMAControl() const { return dma_control_; }
  std::uint32_t Command0() const { return command_[0]; }
  std::uint8_t LastOpcode() const { return last_opcode_; }
  std::uint32_t LastError() const { return last_error_; }
  std::uint64_t CommandSequence() const { return command_sequence_; }
  bool AudioEnabled() const { return audio_enabled_; }
  bool AudioStreaming() const { return audio_streaming_; }
  std::uint64_t AudioPosition() const { return audio_position_; }

private:
  static constexpr std::uint32_t ToPhysical(std::uint32_t address) {
    return address & 0x3fffffffu;
  }

  static constexpr std::uint32_t kStatus = 0x00u;
  static constexpr std::uint32_t kCover = 0x04u;
  static constexpr std::uint32_t kCommand0 = 0x08u;
  static constexpr std::uint32_t kCommand1 = 0x0cu;
  static constexpr std::uint32_t kCommand2 = 0x10u;
  static constexpr std::uint32_t kDmaAddress = 0x14u;
  static constexpr std::uint32_t kDmaLength = 0x18u;
  static constexpr std::uint32_t kDmaControl = 0x1cu;
  static constexpr std::uint32_t kImmediate = 0x20u;
  static constexpr std::uint32_t kConfig = 0x24u;

  static constexpr std::uint32_t kBreak = 1u << 0;
  static constexpr std::uint32_t kDeviceErrorMask = 1u << 1;
  static constexpr std::uint32_t kDeviceErrorInt = 1u << 2;
  static constexpr std::uint32_t kTransferCompleteMask = 1u << 3;
  static constexpr std::uint32_t kTransferCompleteInt = 1u << 4;
  static constexpr std::uint32_t kBreakMask = 1u << 5;
  static constexpr std::uint32_t kBreakInt = 1u << 6;

  static constexpr std::uint32_t kCoverOpen = 1u << 0;
  static constexpr std::uint32_t kCoverIntMask = 1u << 1;
  static constexpr std::uint32_t kCoverInt = 1u << 2;

  static constexpr std::uint32_t kControlTStart = 1u << 0;
  static constexpr std::uint32_t kControlDma = 1u << 1;
  static constexpr std::uint32_t kControlWrite = 1u << 2;

  static constexpr std::uint32_t kGcDmaAddressMask = 0x03ffffe0u;
  static constexpr std::uint32_t kDmaLengthMask = 0xffffffe0u;

  bool Read32(std::uint32_t offset, std::uint32_t* value) const;
  bool Write32(std::uint32_t offset, std::uint32_t value);
  void ExecuteCommand();
  void FinishCommand(bool success, std::uint32_t immediate,
                     std::uint32_t bytes_transferred, std::uint32_t error_code);

  std::uint32_t status_ = 0;
  std::uint32_t cover_ = 0;
  std::array<std::uint32_t, 3> command_{};
  std::uint32_t dma_address_ = 0;
  std::uint32_t dma_length_ = 0;
  std::uint32_t dma_control_ = 0;
  std::uint32_t immediate_ = 0;
  std::uint32_t config_ = 1;
  std::uint32_t last_error_ = 0;
  std::uint8_t last_opcode_ = 0;
  std::uint64_t command_sequence_ = 0;
  bool disc_present_ = true;

  // GameCube DTK/streaming-audio drive state. This is the DI command contract
  // only; decoding/mixing DTK sectors is intentionally a separate audio task.
  bool audio_enabled_ = false;
  bool audio_streaming_ = false;
  bool audio_stop_at_track_end_ = false;
  std::uint8_t audio_buffer_size_ = 0;
  std::uint64_t audio_current_start_ = 0;
  std::uint32_t audio_current_length_ = 0;
  std::uint64_t audio_position_ = 0;
  std::uint64_t audio_next_start_ = 0;
  std::uint32_t audio_next_length_ = 0;

  DeviceHooks hooks_{};
};

} // namespace GekkoAOT::HW::DI
