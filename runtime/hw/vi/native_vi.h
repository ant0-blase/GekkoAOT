#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdint>

namespace GekkoAOT::HW::VI {

// Standalone GameCube Video Interface (VI) register/timing block.
//
// The renderer itself remains NativeGX -> Aurora. NativeVI owns the retail
// Flipper MMIO state at CC002000, beam timing and VI interrupt generation.
class NativeVI final {
public:
  static constexpr std::uint32_t BasePhysical = 0x0c002000u;
  static constexpr std::uint32_t WindowSize = 0x1000u;
  static constexpr std::uint32_t CpuClockHz = 486000000u;

  NativeVI() { Reset(); }

  void Reset(bool ntsc = true, bool component_cable = false);
  bool Handles(std::uint32_t address) const;
  bool Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value) const;
  bool Write(std::uint32_t address, std::uint64_t value, std::uint8_t size);
  void AdvanceCycles(std::uint64_t cycles);

  bool InterruptPending() const;
  bool ConsumeInterruptUpdate();
  bool ConsumeOddFieldBoundary();
  bool ConsumeEvenFieldBoundary();
  bool ConsumeFrameBoundary();

  std::uint16_t Clock() const { return regs_[kClock / 2u]; }
  std::uint16_t DtvStatus() const { return regs_[kDtvStatus / 2u]; }
  std::uint32_t HalfLineCount() const { return half_line_count_; }
  std::uint32_t TicksPerHalfLine() const;
  std::uint32_t HalfLinesPerFrame() const;
  std::uint32_t XfbAddressTop() const;
  std::uint32_t XfbAddressBottom() const;
  // Geometry encoded by the retail VI picture configuration. WPL is the
  // visible width in 16-pixel units, STD is the field stride in 16-pixel
  // units and ACV is the number of active lines in one field.
  std::uint32_t XfbWidthPixels() const;
  std::uint32_t XfbStrideBytes() const;
  std::uint32_t XfbFieldHeight() const;

private:
  static constexpr std::uint32_t ToPhysical(std::uint32_t address) {
    return address & 0x3fffffffu;
  }

  // VI register offsets.
  static constexpr std::uint32_t kVerticalTiming = 0x00u;
  static constexpr std::uint32_t kControl = 0x02u;
  static constexpr std::uint32_t kHorizontalTiming0Hi = 0x04u;
  static constexpr std::uint32_t kHorizontalTiming0Lo = 0x06u;
  static constexpr std::uint32_t kHorizontalTiming1Hi = 0x08u;
  static constexpr std::uint32_t kHorizontalTiming1Lo = 0x0au;
  static constexpr std::uint32_t kVblankOddHi = 0x0cu;
  static constexpr std::uint32_t kVblankOddLo = 0x0eu;
  static constexpr std::uint32_t kVblankEvenHi = 0x10u;
  static constexpr std::uint32_t kVblankEvenLo = 0x12u;
  static constexpr std::uint32_t kBurstOddHi = 0x14u;
  static constexpr std::uint32_t kBurstOddLo = 0x16u;
  static constexpr std::uint32_t kBurstEvenHi = 0x18u;
  static constexpr std::uint32_t kBurstEvenLo = 0x1au;
  static constexpr std::uint32_t kFbLeftTopHi = 0x1cu;
  static constexpr std::uint32_t kFbRightTopHi = 0x20u;
  static constexpr std::uint32_t kFbLeftBottomHi = 0x24u;
  static constexpr std::uint32_t kFbRightBottomHi = 0x28u;
  static constexpr std::uint32_t kVerticalBeam = 0x2cu;
  static constexpr std::uint32_t kHorizontalBeam = 0x2eu;
  static constexpr std::uint32_t kPreRetraceHi = 0x30u;
  static constexpr std::uint32_t kPostRetraceHi = 0x34u;
  static constexpr std::uint32_t kDisplayInterrupt2Hi = 0x38u;
  static constexpr std::uint32_t kDisplayInterrupt3Hi = 0x3cu;
  static constexpr std::uint32_t kDisplayLatch0Hi = 0x40u;
  static constexpr std::uint32_t kDisplayLatch1Hi = 0x44u;
  static constexpr std::uint32_t kHScaleW = 0x48u;
  static constexpr std::uint32_t kHScaleR = 0x4au;
  static constexpr std::uint32_t kFilter0Hi = 0x4cu;
  static constexpr std::uint32_t kUnknownAaHi = 0x68u;
  static constexpr std::uint32_t kUnknownAaLo = 0x6au;
  static constexpr std::uint32_t kClock = 0x6cu;
  static constexpr std::uint32_t kDtvStatus = 0x6eu;
  static constexpr std::uint32_t kFbWidth = 0x70u;
  static constexpr std::uint32_t kBorderBlankEnd = 0x72u;
  static constexpr std::uint32_t kBorderBlankStart = 0x74u;
  static constexpr std::uint32_t kLastRegister = kBorderBlankStart;

  static constexpr std::uint16_t kControlEnable = 1u << 0;
  static constexpr std::uint16_t kControlReset = 1u << 1;
  static constexpr std::uint16_t kControlWritableMask =
      (1u << 0) | (1u << 2) | (1u << 3) | (3u << 4) | (3u << 6) | (3u << 8);

  static constexpr std::uint32_t kInterruptMaskBit = 1u << 28;
  static constexpr std::uint32_t kInterruptStatusBit = 1u << 31;

  bool Read16(std::uint32_t offset, std::uint16_t* value) const;
  bool Write16(std::uint32_t offset, std::uint16_t value);
  std::uint32_t Pair32(std::uint32_t hi_offset) const;
  void SetPair32(std::uint32_t hi_offset, std::uint32_t value);
  void UpdateInterruptTargets(std::uint64_t phase_begin, std::uint64_t phase_end,
                              bool include_begin);
  std::uint32_t HalfLinesPerOddField() const;
  std::uint32_t HalfLinesPerEvenField() const;
  std::uint32_t HorizontalLineWidth() const;
  std::uint16_t VerticalBeamValue() const;
  std::uint16_t HorizontalBeamValue() const;
  void RecomputeTimingCache();
  void ClampTimingPhase();

  std::array<std::uint16_t, (kLastRegister / 2u) + 1u> regs_{};
  std::uint64_t cycles_into_half_line_ = 0;
  std::uint32_t half_line_count_ = 0;
  bool half_line_start_checked_ = false;
  // VI timing registers change rarely, but AdvanceCycles is called for every
  // compiled guest slice. Keep the derived values hot instead of rebuilding
  // them (including a 64-bit division) in the runtime's hottest loop.
  std::uint32_t cached_horizontal_line_width_ = 1;
  std::uint32_t cached_ticks_per_half_line_ = 1;
  std::uint32_t cached_half_lines_per_frame_ = 1;
  bool interrupt_update_pending_ = false;
  bool odd_field_boundary_seen_ = false;
  bool even_field_boundary_seen_ = false;
  bool frame_boundary_seen_ = false;
};

} // namespace GekkoAOT::HW::VI
