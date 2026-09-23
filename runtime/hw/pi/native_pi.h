#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>

namespace GekkoAOT::HW::PI {

class NativePI final {
public:
  static constexpr std::uint32_t BasePhysical = 0x0c003000u;
  static constexpr std::uint32_t WindowSize = 0x1000u;

  enum InterruptCause : std::uint32_t {
    Pi = 0x0001u,
    ResetSwitch = 0x0002u,
    Dvd = 0x0004u,
    Serial = 0x0008u,
    Exi = 0x0010u,
    Audio = 0x0020u,
    Dsp = 0x0040u,
    Memory = 0x0080u,
    Video = 0x0100u,
    PeToken = 0x0200u,
    PeFinish = 0x0400u,
    CommandProcessor = 0x0800u,
    Debug = 0x1000u,
    HighSpeedPort = 0x2000u,
    WiiIpc = 0x4000u,
    ResetButtonState = 0x10000u,
  };

  NativePI() { Reset(); }

  void Reset();
  bool Handles(std::uint32_t address) const;
  bool Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value) const;
  bool Write(std::uint32_t address, std::uint64_t value, std::uint8_t size);

  void SetInterrupt(std::uint32_t cause, bool set = true);
  bool InterruptPending() const { return (interrupt_cause_ & interrupt_mask_) != 0; }

  std::uint32_t InterruptCauseValue() const { return interrupt_cause_; }
  std::uint32_t InterruptMaskValue() const { return interrupt_mask_; }
  std::uint32_t FifoBase() const { return fifo_cpu_base_; }
  std::uint32_t FifoEnd() const { return fifo_cpu_end_; }
  std::uint32_t FifoWritePointer() const { return fifo_cpu_write_pointer_; }
  void SetFifoWritePointerFromCP(std::uint32_t value);
  void AdvanceCpuFifoWritePointer();
  bool ConsumeFifoResetRequested();

private:
  static constexpr std::uint32_t ToPhysical(std::uint32_t address) {
    return address & 0x3fffffffu;
  }

  bool Read32(std::uint32_t offset, std::uint32_t* value) const;
  bool Write32(std::uint32_t offset, std::uint32_t value);

  std::uint32_t interrupt_cause_ = 0;
  std::uint32_t interrupt_mask_ = 0;
  std::uint32_t fifo_cpu_base_ = 0;
  std::uint32_t fifo_cpu_end_ = 0;
  std::uint32_t fifo_cpu_write_pointer_ = 0;
  std::uint32_t error_cause_ = 0;
  std::uint32_t error_address_ = 0;
  std::uint32_t reset_code_ = 0;
  std::uint32_t unknown_ = 0x000001ffu;
  std::uint32_t flipper_bus_strength_ = 0x02492492u;
  bool fifo_reset_requested_ = false;
};

} // namespace GekkoAOT::HW::PI
