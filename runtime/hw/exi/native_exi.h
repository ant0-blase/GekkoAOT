#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdint>

namespace GekkoAOT::HW::EXI {

// Standalone GameCube Expansion Interface (EXI) register block.
//
// EXI owns three channels at CC006800, each with five 32-bit registers. The
// register/interrupt machinery is kept separate from concrete devices so the
// native memory-card/IPL/RTC services can plug in without routing through
// Dolphin's CEXIChannel/IEXIDevice classes.
class NativeEXI final {
public:
  static constexpr std::uint32_t BasePhysical = 0x0c006800u;
  static constexpr std::uint32_t ChannelStride = 0x14u;
  static constexpr std::uint32_t ChannelCount = 3u;
  static constexpr std::uint32_t WindowSize = ChannelStride * ChannelCount;

  enum TransferDirection : std::uint8_t {
    Read = 0,
    Write = 1,
    ReadWrite = 2,
  };

  struct DeviceHooks {
    void* user = nullptr;
    bool (*present)(void* user, std::uint32_t channel, std::uint8_t chip_select) = nullptr;
    bool (*immediate)(void* user, std::uint32_t channel, std::uint8_t chip_select,
                      std::uint8_t direction, std::uint8_t length,
                      std::uint32_t* data) = nullptr;
    bool (*dma)(void* user, std::uint32_t channel, std::uint8_t chip_select,
                std::uint8_t direction, std::uint32_t guest_address,
                std::uint32_t length) = nullptr;
  };

  NativeEXI() { Reset(); }

  void Reset();
  bool Handles(std::uint32_t address) const;
  bool ReadRegister(std::uint32_t address, std::uint8_t size, std::uint64_t* value) const;
  bool WriteRegister(std::uint32_t address, std::uint64_t value, std::uint8_t size);

  void SetDeviceHooks(DeviceHooks hooks) { hooks_ = hooks; }
  bool InterruptPending() const;

  std::uint32_t Status(std::uint32_t channel) const;
  std::uint32_t DmaAddress(std::uint32_t channel) const;
  std::uint32_t DmaLength(std::uint32_t channel) const;
  std::uint32_t Control(std::uint32_t channel) const;
  std::uint32_t ImmediateData(std::uint32_t channel) const;

private:
  static constexpr std::uint32_t ToPhysical(std::uint32_t address) {
    return address & 0x3fffffffu;
  }

  struct Channel {
    std::uint32_t status = 0;
    std::uint32_t dma_address = 0;
    std::uint32_t dma_length = 0;
    std::uint32_t control = 0;
    std::uint32_t immediate_data = 0;
  };

  static constexpr std::uint32_t kExiIntMask = 1u << 0;
  static constexpr std::uint32_t kExiInt = 1u << 1;
  static constexpr std::uint32_t kTcIntMask = 1u << 2;
  static constexpr std::uint32_t kTcInt = 1u << 3;
  static constexpr std::uint32_t kClockMask = 0x7u << 4;
  static constexpr std::uint32_t kChipSelectMask = 0x7u << 7;
  static constexpr std::uint32_t kExtIntMask = 1u << 10;
  static constexpr std::uint32_t kExtInt = 1u << 11;
  static constexpr std::uint32_t kExternalPresent = 1u << 12;
  static constexpr std::uint32_t kRomDisable = 1u << 13;

  static constexpr std::uint32_t kTransferStart = 1u << 0;
  static constexpr std::uint32_t kDmaMode = 1u << 1;
  static constexpr std::uint32_t kDirectionMask = 0x3u << 2;
  static constexpr std::uint32_t kTransferLengthMask = 0x3u << 4;
  static constexpr std::uint32_t kControlMask =
      kTransferStart | kDmaMode | kDirectionMask | kTransferLengthMask;

  bool Decode(std::uint32_t address, std::uint32_t* channel, std::uint32_t* reg_offset) const;
  std::uint32_t Read32(std::uint32_t channel, std::uint32_t reg_offset) const;
  bool Write32(std::uint32_t channel, std::uint32_t reg_offset, std::uint32_t value);
  bool DevicePresent(std::uint32_t channel, std::uint8_t chip_select) const;
  void RunTransfer(std::uint32_t channel);

  std::array<Channel, ChannelCount> channels_{};
  DeviceHooks hooks_{};
};

} // namespace GekkoAOT::HW::EXI
