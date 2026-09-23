#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>

namespace GekkoAOT::HW::CP {

// Native model of Flipper's Command Processor register block.  The standalone
// renderer consumes the write-gather stream synchronously, but the guest still
// owns and observes the real CP FIFO register state used by the Dolphin SDK.
class NativeCP final {
public:
  static constexpr std::uint32_t BasePhysical = 0x0c000000u;
  static constexpr std::uint32_t WindowSize = 0x1000u;

  NativeCP() { Reset(); }

  void Reset();
  bool Handles(std::uint32_t address) const;
  bool Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value) const;
  bool Write(std::uint32_t address, std::uint64_t value, std::uint8_t size);

  // Notify the CP that bytes were written to the 32-byte Gekko write-gather
  // pipe. An enabled GP drains complete bursts synchronously through the read
  // callback, respecting breakpoints before exposing bytes to NativeGX.
  // Returns true only when one or more complete 32-byte gather bursts changed
  // guest-visible CP FIFO accounting.
  bool NotifyGatherWrite(std::uint32_t bytes);
  using ReadBurst = bool (*)(void* user, std::uint32_t address);
  void SetReadBurstCallback(ReadBurst callback, void* user) {
    read_burst_ = callback;
    read_burst_user_ = user;
  }

  bool InterruptPending() const;
  bool GPReadEnabled() const { return (control_ & kCtrlGpReadEnable) != 0; }
  bool GPLinkEnabled() const { return (control_ & kCtrlGpLinkEnable) != 0; }

  std::uint16_t Control() const { return control_; }
  std::uint16_t Status() const { return ComputeStatus(); }
  std::uint32_t FifoBase() const { return fifo_base_; }
  std::uint32_t FifoEnd() const { return fifo_end_; }
  std::uint32_t FifoHiWatermark() const { return fifo_hi_watermark_; }
  std::uint32_t FifoLoWatermark() const { return fifo_lo_watermark_; }
  std::uint32_t FifoReadWriteDistance() const { return fifo_rw_distance_; }
  std::uint32_t FifoWritePointer() const { return fifo_write_pointer_; }
  std::uint32_t FifoReadPointer() const { return fifo_read_pointer_; }
  std::uint32_t FifoBreakpoint() const { return fifo_breakpoint_; }

private:
  static constexpr std::uint32_t ToPhysical(std::uint32_t address) {
    return address & 0x3fffffffu;
  }

  static constexpr std::uint16_t kCtrlGpReadEnable = 1u << 0;
  static constexpr std::uint16_t kCtrlBpEnable = 1u << 1;
  static constexpr std::uint16_t kCtrlOverflowIntEnable = 1u << 2;
  static constexpr std::uint16_t kCtrlUnderflowIntEnable = 1u << 3;
  static constexpr std::uint16_t kCtrlGpLinkEnable = 1u << 4;
  static constexpr std::uint16_t kCtrlBpInt = 1u << 5;

  static constexpr std::uint32_t kAddressMask = 0x03ffffe0u;
  static constexpr std::uint16_t kLowMask = 0xffe0u;
  static constexpr std::uint16_t kHighMask = 0x03ffu;

  bool Read16(std::uint32_t offset, std::uint16_t* value) const;
  bool Write16(std::uint32_t offset, std::uint16_t value);
  static void WriteLow(std::uint32_t* reg, std::uint16_t value);
  static void WriteHigh(std::uint32_t* reg, std::uint16_t value);
  std::uint16_t ComputeStatus() const;
  bool HiWatermarkActive() const;
  bool LoWatermarkActive() const;
  bool BreakpointActive() const;
  void AdvancePointer(std::uint32_t* pointer);
  void DrainSynchronousGpu();

  std::uint16_t control_ = 0;
  std::uint16_t perf_select_ = 0;
  std::uint16_t unknown_0a_ = 0;
  std::uint32_t fifo_base_ = 0;
  std::uint32_t fifo_end_ = 0;
  std::uint32_t fifo_hi_watermark_ = 0;
  std::uint32_t fifo_lo_watermark_ = 0;
  std::uint32_t fifo_rw_distance_ = 0;
  std::uint32_t fifo_write_pointer_ = 0;
  std::uint32_t fifo_read_pointer_ = 0;
  std::uint32_t fifo_breakpoint_ = 0;
  std::uint32_t gather_bytes_ = 0;
  ReadBurst read_burst_ = nullptr;
  void* read_burst_user_ = nullptr;
};

} // namespace GekkoAOT::HW::CP
