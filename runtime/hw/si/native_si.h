#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdint>

namespace GekkoAOT::HW::SI {

// Standalone GameCube Serial Interface (SI) register block.
//
// SI owns the four controller channels at CC006400, the polling/communication
// control registers and the 128-byte transfer RAM at CC006480. Concrete input
// devices are kept behind hooks so the SDL/native-pad layer can be attached
// without routing through Dolphin's SerialInterfaceManager/ISIDevice classes.
class NativeSI final {
public:
  static constexpr std::uint32_t BasePhysical = 0x0c006400u;
  static constexpr std::uint32_t WindowSize = 0x100u; // SI register + transfer-RAM aperture
  static constexpr std::uint32_t ChannelCount = 4u;
  static constexpr std::uint32_t BufferSize = 128u;

  struct DeviceHooks {
    void* user = nullptr;

    // Automatic SI polling. Return true when a device supplied fresh data.
    // hi/lo use the hardware SI channel input register layout.
    bool (*poll)(void* user, std::uint32_t channel,
                 std::uint32_t* hi, std::uint32_t* lo) = nullptr;

    // Communication-buffer transfer. Return >0 for response length, -1 for
    // no-response, or 0 when a device intentionally leaves the transfer pending.
    int (*buffer)(void* user, std::uint32_t channel, std::uint8_t* buffer,
                  std::uint32_t request_length,
                  std::uint32_t expected_response_length) = nullptr;

    // Direct command emitted by SISR.WR using the per-channel OUT register.
    void (*direct)(void* user, std::uint32_t channel,
                   std::uint32_t command, bool polling_enabled) = nullptr;
  };

  NativeSI() { Reset(); }

  void Reset();
  bool Handles(std::uint32_t address) const;
  bool Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value);
  bool Write(std::uint32_t address, std::uint64_t value, std::uint8_t size);

  void SetDeviceHooks(DeviceHooks hooks) { hooks_ = hooks; }
  void PollNow();
  bool InterruptPending() const;

  std::uint32_t PollRegister() const { return poll_; }
  std::uint32_t CommunicationCSR() const { return com_csr_; }
  std::uint32_t StatusRegister() const { return status_; }
  std::uint32_t EXIClockCount() const { return exi_clock_count_; }

private:
  static constexpr std::uint32_t ToPhysical(std::uint32_t address) {
    return address & 0x3fffffffu;
  }

  struct Channel {
    std::uint32_t out = 0;
    std::uint32_t in_hi = 0;
    std::uint32_t in_lo = 0;
  };

  static constexpr std::uint32_t kPoll = 0x30u;
  static constexpr std::uint32_t kComCsr = 0x34u;
  static constexpr std::uint32_t kStatus = 0x38u;
  static constexpr std::uint32_t kExiClockCount = 0x3cu;
  static constexpr std::uint32_t kIoBuffer = 0x80u;
  static constexpr std::uint32_t kIoBufferEnd = kIoBuffer + BufferSize;

  static constexpr std::uint32_t kComTStart = 1u << 0;
  static constexpr std::uint32_t kComChannelMask = 0x3u << 1;
  static constexpr std::uint32_t kComInLengthMask = 0x7fu << 8;
  static constexpr std::uint32_t kComOutLengthMask = 0x7fu << 16;
  static constexpr std::uint32_t kComRdstIntMask = 1u << 27;
  static constexpr std::uint32_t kComRdstInt = 1u << 28;
  static constexpr std::uint32_t kComError = 1u << 29;
  static constexpr std::uint32_t kComTcIntMask = 1u << 30;
  static constexpr std::uint32_t kComTcInt = 1u << 31;

  static constexpr std::uint32_t kStatusWrite = 1u << 31;

  static std::uint32_t ReadDataBit(std::uint32_t channel) {
    return 0x20000000u >> (channel * 8u);
  }
  static std::uint32_t WriteStatusBit(std::uint32_t channel) {
    return 0x10000000u >> (channel * 8u);
  }
  static std::uint32_t NoResponseBit(std::uint32_t channel) {
    return 0x08000000u >> (channel * 8u);
  }
  static std::uint32_t ErrorBits(std::uint32_t channel) {
    return 0x0f000000u >> (channel * 8u);
  }
  static std::uint32_t PollEnableBit(std::uint32_t channel) {
    return 1u << (7u - channel);
  }

  static std::uint32_t DecodeLength(std::uint32_t field) {
    field &= 0x7fu;
    return ((field - 1u) & 0x7fu) + 1u;
  }

  bool ReadBuffer(std::uint32_t offset, std::uint8_t size, std::uint64_t* value) const;
  bool WriteBuffer(std::uint32_t offset, std::uint64_t value, std::uint8_t size);
  bool ReadRegister32(std::uint32_t offset, std::uint32_t* value);
  bool WriteRegister32(std::uint32_t offset, std::uint32_t value);
  void UpdateInterruptState();
  void RunBufferTransfer();
  void SendDirectCommands();
  void SetNoResponse(std::uint32_t channel);

  std::array<Channel, ChannelCount> channels_{};
  std::uint32_t poll_ = 0;
  std::uint32_t com_csr_ = 0;
  std::uint32_t status_ = 0;
  std::uint32_t exi_clock_count_ = 0;
  std::array<std::uint8_t, BufferSize> buffer_{};
  DeviceHooks hooks_{};
};

} // namespace GekkoAOT::HW::SI
