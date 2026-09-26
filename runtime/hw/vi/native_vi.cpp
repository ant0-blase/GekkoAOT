// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/vi/native_vi.h"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace GekkoAOT::HW::VI {
namespace {
constexpr std::uint32_t kClockFrequencies[2] = {27000000u, 54000000u};

constexpr std::uint32_t InterruptOffsets[4] = {
    0x30u, 0x34u, 0x38u, 0x3cu,
};

std::uint64_t SliceBigEndian16(std::uint16_t word, std::uint32_t byte_offset,
                               std::uint8_t size) {
  if (size == 2u) return word;
  return byte_offset == 0u ? (word >> 8) : (word & 0xffu);
}
}

void NativeVI::Reset(bool ntsc, bool component_cable) {
  regs_.fill(0);

  // Retail-compatible VI preset. These are the same hardware-facing values
  // the SDK/boot path expects before a title installs its final video mode.
  regs_[kVerticalTiming / 2u] = 0x0006u; // EQU=6, ACV=0
  regs_[kControl / 2u] = static_cast<std::uint16_t>(0x0001u | (ntsc ? 0u : 0x0100u));
  SetPair32(kHorizontalTiming0Hi, 0x476901adu); // HLW=429, HCE=105, HCS=71
  SetPair32(kHorizontalTiming1Hi, 0x02ea5140u); // HSY=64, HBE=162, HBS=373
  SetPair32(kVblankOddHi, 0x000501f6u);         // PRB=502, PSB=5
  SetPair32(kVblankEvenHi, 0x000401f7u);        // PRB=503, PSB=4
  SetPair32(kBurstOddHi, 0x410c410cu);
  SetPair32(kBurstEvenHi, 0x40ed40edu);

  SetPair32(kPreRetraceHi, 0x110701aeu);  // HCT=430, VCT=263, mask=1
  SetPair32(kPostRetraceHi, 0x10010001u); // HCT=1, VCT=1, mask=1

  regs_[kHScaleW / 2u] = 0x2828u; // STD=40, WPL=40
  regs_[kHScaleR / 2u] = 0u;
  regs_[kClock / 2u] = ntsc ? 1u : 0u; // 54MHz for NTSC, 27MHz otherwise
  regs_[kDtvStatus / 2u] = component_cable ? 1u : 0u;
  regs_[kFbWidth / 2u] = 0u;

  RecomputeTimingCache();
  cycles_into_half_line_ = 0;
  half_line_count_ = 0;
  half_line_start_checked_ = false;
  interrupt_update_pending_ = false;
  odd_field_boundary_seen_ = false;
  even_field_boundary_seen_ = false;
  frame_boundary_seen_ = false;
}

bool NativeVI::Handles(std::uint32_t address) const {
  const std::uint32_t physical = ToPhysical(address);
  return physical >= BasePhysical && physical < BasePhysical + WindowSize;
}

std::uint32_t NativeVI::Pair32(std::uint32_t hi_offset) const {
  const std::uint32_t hi = regs_[hi_offset / 2u];
  const std::uint32_t lo = regs_[(hi_offset + 2u) / 2u];
  return (hi << 16) | lo;
}

void NativeVI::SetPair32(std::uint32_t hi_offset, std::uint32_t value) {
  regs_[hi_offset / 2u] = static_cast<std::uint16_t>(value >> 16);
  regs_[(hi_offset + 2u) / 2u] = static_cast<std::uint16_t>(value);
}

std::uint32_t NativeVI::HorizontalLineWidth() const {
  return cached_horizontal_line_width_;
}

std::uint32_t NativeVI::HalfLinesPerOddField() const {
  const std::uint16_t vertical = regs_[kVerticalTiming / 2u];
  const std::uint32_t equ = vertical & 0x0fu;
  const std::uint32_t acv = (vertical >> 4) & 0x03ffu;
  const std::uint32_t vblank = Pair32(kVblankOddHi);
  const std::uint32_t prb = vblank & 0x03ffu;
  const std::uint32_t psb = (vblank >> 16) & 0x03ffu;
  const std::uint32_t count = 3u * equ + prb + 2u * acv + psb;
  return count ? count : 1u;
}

std::uint32_t NativeVI::HalfLinesPerEvenField() const {
  const std::uint16_t vertical = regs_[kVerticalTiming / 2u];
  const std::uint32_t equ = vertical & 0x0fu;
  const std::uint32_t acv = (vertical >> 4) & 0x03ffu;
  const std::uint32_t vblank = Pair32(kVblankEvenHi);
  const std::uint32_t prb = vblank & 0x03ffu;
  const std::uint32_t psb = (vblank >> 16) & 0x03ffu;
  const std::uint32_t count = 3u * equ + prb + 2u * acv + psb;
  return count ? count : 1u;
}

std::uint32_t NativeVI::HalfLinesPerFrame() const {
  return cached_half_lines_per_frame_;
}

std::uint32_t NativeVI::TicksPerHalfLine() const {
  return cached_ticks_per_half_line_;
}

void NativeVI::RecomputeTimingCache() {
  const std::uint32_t hlw = Pair32(kHorizontalTiming0Hi) & 0x3ffu;
  cached_horizontal_line_width_ = hlw ? hlw : 1u;

  const std::uint32_t clock_index = regs_[kClock / 2u] & 1u;
  const std::uint32_t sample_ticks =
      static_cast<std::uint32_t>((2ull * CpuClockHz) / kClockFrequencies[clock_index]);
  const std::uint64_t ticks =
      static_cast<std::uint64_t>(sample_ticks) * cached_horizontal_line_width_;
  cached_ticks_per_half_line_ =
      static_cast<std::uint32_t>(std::max<std::uint64_t>(ticks, 1u));

  const std::uint64_t frame =
      static_cast<std::uint64_t>(HalfLinesPerOddField()) +
      static_cast<std::uint64_t>(HalfLinesPerEvenField());
  cached_half_lines_per_frame_ =
      static_cast<std::uint32_t>(std::max<std::uint64_t>(
          1u, std::min<std::uint64_t>(frame, 0xffffffffull)));
}

std::uint16_t NativeVI::VerticalBeamValue() const {
  return static_cast<std::uint16_t>(1u + half_line_count_ / 2u);
}

std::uint16_t NativeVI::HorizontalBeamValue() const {
  const std::uint32_t hlw = HorizontalLineWidth();
  const std::uint32_t ticks = TicksPerHalfLine();
  const std::uint64_t cycles_into_line =
      (half_line_count_ & 1u ? static_cast<std::uint64_t>(ticks) : 0ull) +
      cycles_into_half_line_;
  const std::uint64_t raw = 1ull + static_cast<std::uint64_t>(hlw) * cycles_into_line / ticks;
  return static_cast<std::uint16_t>(std::clamp<std::uint64_t>(raw, 1ull, 2ull * hlw));
}

bool NativeVI::Read16(std::uint32_t offset, std::uint16_t* value) const {
  if (!value || (offset & 1u) != 0u || offset > kLastRegister) return false;
  if (offset == kVerticalBeam) {
    *value = VerticalBeamValue();
    return true;
  }
  if (offset == kHorizontalBeam) {
    *value = HorizontalBeamValue();
    return true;
  }
  *value = regs_[offset / 2u];
  return true;
}

bool NativeVI::Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value) const {
  if (!value || !Handles(address) || (size != 1u && size != 2u && size != 4u)) return false;
  const std::uint32_t offset = ToPhysical(address) - BasePhysical;
  if (offset > kLastRegister + 1u) return false;

  if (size == 4u) {
    if ((offset & 3u) != 0u || offset + 2u > kLastRegister) return false;
    std::uint16_t hi = 0, lo = 0;
    if (!Read16(offset, &hi) || !Read16(offset + 2u, &lo)) return false;
    *value = (static_cast<std::uint32_t>(hi) << 16) | lo;
    return true;
  }

  if (size == 2u) {
    if ((offset & 1u) != 0u || offset > kLastRegister) return false;
    std::uint16_t word = 0;
    if (!Read16(offset, &word)) return false;
    *value = word;
    return true;
  }

  const std::uint32_t aligned = offset & ~1u;
  if (aligned > kLastRegister) return false;
  std::uint16_t word = 0;
  if (!Read16(aligned, &word)) return false;
  *value = SliceBigEndian16(word, offset & 1u, 1u);
  return true;
}

void NativeVI::ClampTimingPhase() {
  const std::uint32_t per_halfline = TicksPerHalfLine();
  if (cycles_into_half_line_ >= per_halfline)
    cycles_into_half_line_ %= per_halfline;
  const std::uint32_t frame_halflines = HalfLinesPerFrame();
  if (half_line_count_ >= frame_halflines)
    half_line_count_ %= frame_halflines;
}

bool NativeVI::Write16(std::uint32_t offset, std::uint16_t value) {
  if ((offset & 1u) != 0u || offset > kLastRegister) return false;
  if (offset == kVerticalBeam || offset == kHorizontalBeam) {
    // Beam-position writes are undocumented on retail hardware. Keep the
    // generated timing state authoritative instead of silently corrupting it.
    return true;
  }

  if (offset == kControl) {
    const bool reset = (value & kControlReset) != 0u;
    regs_[kControl / 2u] = value & kControlWritableMask;
    if (reset) {
      for (std::uint32_t irq : InterruptOffsets) SetPair32(irq, 0u);
      half_line_count_ = 0;
      cycles_into_half_line_ = 0;
      half_line_start_checked_ = false;
      odd_field_boundary_seen_ = false;
      even_field_boundary_seen_ = false;
      frame_boundary_seen_ = false;
      interrupt_update_pending_ = true;
    }
    return true;
  }

  // The four interrupt HI halves contain VCT, mask and status. Writes are
  // direct hardware state, matching retail VI MMIO semantics used by the SDK.
  for (std::uint32_t index = 0; index < 4u; ++index) {
    const std::uint32_t irq = InterruptOffsets[index];
    if (offset == irq) {
      const std::uint32_t before = Pair32(irq);
      regs_[offset / 2u] = value;
      const std::uint32_t after = Pair32(irq);
      interrupt_update_pending_ = true;

      // v81: trace SDK VI configuration/acknowledgement without changing it.
      static unsigned write_logs = 0u;
      if (write_logs < 128u &&
          ((before ^ after) & (kInterruptMaskBit | kInterruptStatusBit))) {
        ++write_logs;
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_VI_IRQ_TRACE_V81=1 phase=write idx=%u offset=%02x before=%08x after=%08x mask=%u status=%u\n",
                     index, irq, before, after,
                     (after & kInterruptMaskBit) ? 1u : 0u,
                     (after & kInterruptStatusBit) ? 1u : 0u);
      }
      return true;
    }
  }

  // XFB high words have a clear-POFF command in bit 31. The POFF state itself
  // is bit 28, both located in this high half.
  if (offset == kFbLeftTopHi || offset == kFbRightTopHi ||
      offset == kFbLeftBottomHi || offset == kFbRightBottomHi) {
    std::uint16_t next = value;
    if (value & 0xe000u) next &= ~0x1000u;
    regs_[offset / 2u] = next;
    return true;
  }

  regs_[offset / 2u] = value;
  if (offset == kVerticalTiming || offset == kHorizontalTiming0Hi ||
      offset == kHorizontalTiming0Lo || offset == kVblankOddHi ||
      offset == kVblankOddLo || offset == kVblankEvenHi ||
      offset == kVblankEvenLo || offset == kClock) {
    RecomputeTimingCache();
    ClampTimingPhase();
  }
  return true;
}

bool NativeVI::Write(std::uint32_t address, std::uint64_t value, std::uint8_t size) {
  if (!Handles(address) || (size != 2u && size != 4u)) return false;
  const std::uint32_t offset = ToPhysical(address) - BasePhysical;

  if (size == 2u) {
    if ((offset & 1u) != 0u || offset > kLastRegister) return false;
    return Write16(offset, static_cast<std::uint16_t>(value));
  }

  if ((offset & 3u) != 0u || offset + 2u > kLastRegister) return false;
  const std::uint32_t word = static_cast<std::uint32_t>(value);
  return Write16(offset, static_cast<std::uint16_t>(word >> 16)) &&
         Write16(offset + 2u, static_cast<std::uint16_t>(word));
}

void NativeVI::UpdateInterruptTargets(std::uint64_t phase_begin,
                                      std::uint64_t phase_end,
                                      bool include_begin) {
  const std::uint32_t hlw = HorizontalLineWidth();
  const std::uint32_t halfline_first_sample =
      1u + (half_line_count_ & 1u) * hlw;
  const std::uint32_t current_line = VerticalBeamValue();
  for (std::uint32_t irq : InterruptOffsets) {
    std::uint32_t reg = Pair32(irq);
    const std::uint32_t hct = reg & 0x07ffu;
    const std::uint32_t vct = (reg >> 16) & 0x07ffu;
    if (vct != current_line || hct < halfline_first_sample ||
        hct >= halfline_first_sample + hlw) continue;

    // The beam counter rises when this sample begins. Match the same integer
    // clock conversion used by HorizontalBeamValue(), including non-integral
    // host-cycle/sample ratios after guest timing-register changes.
    const std::uint64_t sample_offset = hct - halfline_first_sample;
    const std::uint64_t target_cycle =
        (sample_offset * TicksPerHalfLine() + hlw - 1u) / hlw;
    if (target_cycle < phase_begin || target_cycle > phase_end ||
        (target_cycle == phase_begin && !include_begin)) continue;

    const bool newly_asserted = (reg & kInterruptStatusBit) == 0u;
    reg |= kInterruptStatusBit;
    SetPair32(irq, reg);
    if (newly_asserted) {
      interrupt_update_pending_ = true;
      static unsigned assert_logs = 0u;
      if (assert_logs++ < 128u) {
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_VI_IRQ_TRACE_V81=1 phase=latch idx=%u hct=%u vct=%u halfline=%u hlw=%u reg=%08x mask=%u\n",
                     static_cast<unsigned>((irq - InterruptOffsets[0]) / 4u),
                     hct, vct, half_line_count_, hlw, reg,
                     (reg & kInterruptMaskBit) ? 1u : 0u);
      }
    }
  }
}

void NativeVI::AdvanceCycles(std::uint64_t cycles) {
  if ((regs_[kControl / 2u] & kControlEnable) == 0u) return;

  while (cycles != 0u) {
    const std::uint32_t per_halfline = TicksPerHalfLine();
    const std::uint64_t remaining = per_halfline - cycles_into_half_line_;
    const std::uint64_t advance = std::min(cycles, remaining);
    UpdateInterruptTargets(cycles_into_half_line_, cycles_into_half_line_ + advance,
                           cycles_into_half_line_ == 0u && !half_line_start_checked_);
    if (cycles_into_half_line_ == 0u) half_line_start_checked_ = true;
    if (cycles < remaining) {
      cycles_into_half_line_ += cycles;
      return;
    }

    cycles -= remaining;
    cycles_into_half_line_ = 0;
    ++half_line_count_;

    // VI scans two fields per frame.  Keep a distinct boundary for the odd
    // (top-XFB) and even (bottom-XFB) fields instead of exposing only the
    // full-frame wrap.  This is important for titles that update/use the two
    // VI framebuffer origins independently; presenting only TOP once per
    // full frame is effectively an Immediate-XFB-like shortcut.
    const std::uint32_t odd_halflines = HalfLinesPerOddField();
    const std::uint32_t frame_halflines = HalfLinesPerFrame();
    if (half_line_count_ == odd_halflines)
      odd_field_boundary_seen_ = true;
    if (half_line_count_ >= frame_halflines) {
      even_field_boundary_seen_ = true;
      half_line_count_ = 0;
      frame_boundary_seen_ = true;
    }
    // The first sample of the new half-line is already current at the exact
    // boundary. Latch HCT 1 / HCT (HLW + 1) once before returning to the CPU.
    half_line_start_checked_ = false;
    UpdateInterruptTargets(0u, 0u, true);
    half_line_start_checked_ = true;
    // Keep the power-on PI VI cause intact until VI reaches a half-line
    // boundary, where the interrupt line is sampled even without a new IRQ.
    interrupt_update_pending_ = true;
  }
}

bool NativeVI::InterruptPending() const {
  for (std::uint32_t irq : InterruptOffsets) {
    const std::uint32_t reg = Pair32(irq);
    if ((reg & kInterruptMaskBit) && (reg & kInterruptStatusBit)) return true;
  }
  return false;
}

bool NativeVI::ConsumeInterruptUpdate() {
  const bool pending = interrupt_update_pending_;
  interrupt_update_pending_ = false;
  return pending;
}

bool NativeVI::ConsumeOddFieldBoundary() {
  const bool seen = odd_field_boundary_seen_;
  odd_field_boundary_seen_ = false;
  return seen;
}

bool NativeVI::ConsumeEvenFieldBoundary() {
  const bool seen = even_field_boundary_seen_;
  even_field_boundary_seen_ = false;
  return seen;
}

bool NativeVI::ConsumeFrameBoundary() {
  const bool seen = frame_boundary_seen_;
  frame_boundary_seen_ = false;
  return seen;
}

std::uint32_t NativeVI::XfbAddressTop() const {
  const std::uint32_t reg = Pair32(kFbLeftTopHi);
  const std::uint32_t base = reg & 0x00ffffffu;
  return (reg & (1u << 28)) ? (base << 5) : base;
}

std::uint32_t NativeVI::XfbAddressBottom() const {
  const std::uint32_t reg = Pair32(kFbLeftBottomHi);
  const std::uint32_t base = reg & 0x00ffffffu;
  // POFF for bottom is physically tied to the top register.
  const bool poff = (Pair32(kFbLeftTopHi) & (1u << 28)) != 0u;
  return poff ? (base << 5) : base;
}

std::uint32_t NativeVI::XfbWidthPixels() const {
  // VI picture configuration: low byte = STD, bits 8..14 = WPL. Retail
  // scanout width is WPL * 16 pixels.
  const std::uint16_t picture = regs_[kHScaleW / 2u];
  return static_cast<std::uint32_t>((picture >> 8) & 0x7fu) * 16u;
}

std::uint32_t NativeVI::XfbStrideBytes() const {
  // STD is measured in 16-pixel units. XFB is YUVY 4:2:2 at two bytes per
  // pixel, therefore one field-row stride is STD * 16 * 2 bytes.
  const std::uint16_t picture = regs_[kHScaleW / 2u];
  return static_cast<std::uint32_t>(picture & 0xffu) * 32u;
}

std::uint32_t NativeVI::XfbFieldHeight() const {
  const std::uint16_t vertical = regs_[kVerticalTiming / 2u];
  return static_cast<std::uint32_t>((vertical >> 4) & 0x03ffu);
}

} // namespace GekkoAOT::HW::VI
