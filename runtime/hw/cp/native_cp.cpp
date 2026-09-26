// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/cp/native_cp.h"

#include <algorithm>

namespace GekkoAOT::HW::CP {
namespace {
constexpr std::uint32_t kStatus = 0x00u;
constexpr std::uint32_t kControl = 0x02u;
constexpr std::uint32_t kClear = 0x04u;
constexpr std::uint32_t kPerfSelect = 0x06u;
constexpr std::uint32_t kUnknown0A = 0x0au;
constexpr std::uint32_t kFifoBaseLo = 0x20u;
constexpr std::uint32_t kFifoBaseHi = 0x22u;
constexpr std::uint32_t kFifoEndLo = 0x24u;
constexpr std::uint32_t kFifoEndHi = 0x26u;
constexpr std::uint32_t kFifoHiWatermarkLo = 0x28u;
constexpr std::uint32_t kFifoHiWatermarkHi = 0x2au;
constexpr std::uint32_t kFifoLoWatermarkLo = 0x2cu;
constexpr std::uint32_t kFifoLoWatermarkHi = 0x2eu;
constexpr std::uint32_t kFifoRwDistanceLo = 0x30u;
constexpr std::uint32_t kFifoRwDistanceHi = 0x32u;
constexpr std::uint32_t kFifoWritePointerLo = 0x34u;
constexpr std::uint32_t kFifoWritePointerHi = 0x36u;
constexpr std::uint32_t kFifoReadPointerLo = 0x38u;
constexpr std::uint32_t kFifoReadPointerHi = 0x3au;
constexpr std::uint32_t kFifoBreakpointLo = 0x3cu;
constexpr std::uint32_t kFifoBreakpointHi = 0x3eu;
constexpr std::uint32_t kXfRasBusyLo = 0x40u;
constexpr std::uint32_t kXfRasBusyHi = 0x42u;
constexpr std::uint32_t kXfClksLo = 0x44u;
constexpr std::uint32_t kXfClksHi = 0x46u;
constexpr std::uint32_t kXfWaitInLo = 0x48u;
constexpr std::uint32_t kXfWaitInHi = 0x4au;
constexpr std::uint32_t kXfWaitOutLo = 0x4cu;
constexpr std::uint32_t kXfWaitOutHi = 0x4eu;
constexpr std::uint32_t kVcacheCheckLo = 0x50u;
constexpr std::uint32_t kVcacheCheckHi = 0x52u;
constexpr std::uint32_t kVcacheMissLo = 0x54u;
constexpr std::uint32_t kVcacheMissHi = 0x56u;
constexpr std::uint32_t kVcacheStallLo = 0x58u;
constexpr std::uint32_t kVcacheStallHi = 0x5au;
constexpr std::uint32_t kClksPerVtxInLo = 0x60u;
constexpr std::uint32_t kClksPerVtxInHi = 0x62u;
constexpr std::uint32_t kClksPerVtxOut = 0x64u;
}

void NativeCP::Reset() {
  control_ = 0;
  perf_select_ = 0;
  unknown_0a_ = 0;
  fifo_base_ = 0;
  fifo_end_ = 0;
  fifo_hi_watermark_ = 0;
  fifo_lo_watermark_ = 0;
  fifo_rw_distance_ = 0;
  fifo_write_pointer_ = 0;
  fifo_read_pointer_ = 0;
  fifo_breakpoint_ = 0;
  gather_bytes_ = 0;
}

bool NativeCP::Handles(std::uint32_t address) const {
  const std::uint32_t physical = ToPhysical(address);
  return physical >= BasePhysical && physical < BasePhysical + WindowSize;
}

void NativeCP::WriteLow(std::uint32_t* reg, std::uint16_t value) {
  *reg = (*reg & 0xffff0000u) | (static_cast<std::uint32_t>(value & kLowMask));
  *reg &= kAddressMask;
}

void NativeCP::WriteHigh(std::uint32_t* reg, std::uint16_t value) {
  *reg = (*reg & 0x0000ffffu) | (static_cast<std::uint32_t>(value & kHighMask) << 16u);
  *reg &= kAddressMask;
}

bool NativeCP::HiWatermarkActive() const {
  return fifo_rw_distance_ > fifo_hi_watermark_;
}

bool NativeCP::LoWatermarkActive() const {
  return fifo_rw_distance_ < fifo_lo_watermark_;
}

bool NativeCP::BreakpointActive() const {
  return (control_ & kCtrlBpEnable) != 0u && fifo_read_pointer_ == fifo_breakpoint_;
}

std::uint16_t NativeCP::ComputeStatus() const {
  std::uint16_t status = 0;
  if (HiWatermarkActive()) status |= 1u << 0;
  if (LoWatermarkActive()) status |= 1u << 1;
  const bool read_idle = fifo_rw_distance_ == 0u || fifo_read_pointer_ == fifo_write_pointer_;
  const bool command_idle = fifo_rw_distance_ == 0u || !GPReadEnabled() || BreakpointActive();
  if (read_idle) status |= 1u << 2;
  if (command_idle) status |= 1u << 3;
  if (BreakpointActive()) status |= 1u << 4;
  return status;
}

bool NativeCP::InterruptPending() const {
  if (!GPReadEnabled()) return false;
  const bool bp = BreakpointActive() && (control_ & kCtrlBpInt) != 0u;
  const bool overflow = HiWatermarkActive() && (control_ & kCtrlOverflowIntEnable) != 0u;
  const bool underflow = LoWatermarkActive() && (control_ & kCtrlUnderflowIntEnable) != 0u;
  return bp || overflow || underflow;
}

bool NativeCP::Read16(std::uint32_t offset, std::uint16_t* value) const {
  if (!value) return false;
  switch (offset) {
  case kStatus: *value = ComputeStatus(); return true;
  case kControl: *value = control_; return true;
  case kClear: *value = 0; return true;
  case kPerfSelect: *value = perf_select_; return true;
  case kUnknown0A: *value = unknown_0a_; return true;
#define CP_READ_PAIR(lo, hi, reg) \
  case lo: *value = static_cast<std::uint16_t>((reg) & 0xffffu); return true; \
  case hi: *value = static_cast<std::uint16_t>(((reg) >> 16u) & 0xffffu); return true
  CP_READ_PAIR(kFifoBaseLo, kFifoBaseHi, fifo_base_);
  CP_READ_PAIR(kFifoEndLo, kFifoEndHi, fifo_end_);
  CP_READ_PAIR(kFifoHiWatermarkLo, kFifoHiWatermarkHi, fifo_hi_watermark_);
  CP_READ_PAIR(kFifoLoWatermarkLo, kFifoLoWatermarkHi, fifo_lo_watermark_);
  CP_READ_PAIR(kFifoRwDistanceLo, kFifoRwDistanceHi, fifo_rw_distance_);
  CP_READ_PAIR(kFifoWritePointerLo, kFifoWritePointerHi, fifo_write_pointer_);
  CP_READ_PAIR(kFifoReadPointerLo, kFifoReadPointerHi, fifo_read_pointer_);
  CP_READ_PAIR(kFifoBreakpointLo, kFifoBreakpointHi, fifo_breakpoint_);
#undef CP_READ_PAIR
  case kXfRasBusyLo:
  case kXfRasBusyHi:
  case kXfClksLo:
  case kXfClksHi:
  case kXfWaitInLo:
  case kXfWaitInHi:
  case kXfWaitOutLo:
  case kXfWaitOutHi:
  case kVcacheCheckLo:
  case kVcacheCheckHi:
  case kVcacheMissLo:
  case kVcacheMissHi:
  case kVcacheStallLo:
  case kVcacheStallHi:
  case kClksPerVtxInLo:
  case kClksPerVtxInHi:
    *value = 0;
    return true;
  case kClksPerVtxOut:
    *value = 4;
    return true;
  default:
    return false;
  }
}

bool NativeCP::Write16(std::uint32_t offset, std::uint16_t value) {
  switch (offset) {
  case kControl:
    control_ = value & 0x003fu;
    if (GPReadEnabled()) DrainSynchronousGpu();
    return true;
  case kClear:
    // Overflow/underflow are level conditions derived from the current FIFO
    // distance. Accept the SDK clear pulse; the condition naturally reasserts
    // if the FIFO is still outside its programmed watermarks.
    return true;
  case kPerfSelect:
    perf_select_ = value & 0x0007u;
    return true;
  case kUnknown0A:
    unknown_0a_ = value & 0x00ffu;
    return true;
#define CP_WRITE_PAIR(lo, hi, reg) \
  case lo: WriteLow(&(reg), value); return true; \
  case hi: WriteHigh(&(reg), value); return true
  CP_WRITE_PAIR(kFifoBaseLo, kFifoBaseHi, fifo_base_);
  CP_WRITE_PAIR(kFifoEndLo, kFifoEndHi, fifo_end_);
  CP_WRITE_PAIR(kFifoHiWatermarkLo, kFifoHiWatermarkHi, fifo_hi_watermark_);
  CP_WRITE_PAIR(kFifoLoWatermarkLo, kFifoLoWatermarkHi, fifo_lo_watermark_);
  CP_WRITE_PAIR(kFifoRwDistanceLo, kFifoRwDistanceHi, fifo_rw_distance_);
  CP_WRITE_PAIR(kFifoWritePointerLo, kFifoWritePointerHi, fifo_write_pointer_);
  CP_WRITE_PAIR(kFifoReadPointerLo, kFifoReadPointerHi, fifo_read_pointer_);
  CP_WRITE_PAIR(kFifoBreakpointLo, kFifoBreakpointHi, fifo_breakpoint_);
#undef CP_WRITE_PAIR
  default:
    return false;
  }
}

bool NativeCP::Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value) const {
  if (!value || !Handles(address)) return false;
  const std::uint32_t offset = ToPhysical(address) - BasePhysical;
  if (size == 2u && (offset & 1u) == 0u) {
    std::uint16_t result = 0;
    if (!Read16(offset, &result)) return false;
    *value = result;
    return true;
  }
  if (size == 4u && (offset & 3u) == 0u) {
    std::uint16_t high = 0, low = 0;
    if (!Read16(offset, &high) || !Read16(offset + 2u, &low)) return false;
    *value = (static_cast<std::uint32_t>(high) << 16u) | low;
    return true;
  }
  return false;
}

bool NativeCP::Write(std::uint32_t address, std::uint64_t value, std::uint8_t size) {
  if (!Handles(address)) return false;
  const std::uint32_t offset = ToPhysical(address) - BasePhysical;
  if (size == 2u && (offset & 1u) == 0u)
    return Write16(offset, static_cast<std::uint16_t>(value));
  if (size == 4u && (offset & 3u) == 0u) {
    const std::uint16_t high = static_cast<std::uint16_t>((value >> 16u) & 0xffffu);
    const std::uint16_t low = static_cast<std::uint16_t>(value & 0xffffu);
    return Write16(offset, high) && Write16(offset + 2u, low);
  }
  return false;
}

void NativeCP::AdvancePointer(std::uint32_t* pointer) {
  if (!pointer) return;
  if (fifo_base_ == 0u && fifo_end_ == 0u) return;

  // SDK GXFifo::top is base + size - 4, while CP read/write pointers are
  // 32-byte aligned. The final legal pointer is therefore top - 28. Testing
  // only pointer == top skips the wrap point by one gather burst.
  const std::uint64_t next = static_cast<std::uint64_t>(*pointer) + 32u;
  if (*pointer < fifo_base_ || *pointer > fifo_end_ || next > fifo_end_)
    *pointer = fifo_base_;
  else
    *pointer = static_cast<std::uint32_t>(next) & kAddressMask;
}

void NativeCP::DrainSynchronousGpu() {
  // NativeGX consumes the raw gather stream synchronously, but guest-visible
  // CP state must still advance exactly as the Flipper FIFO does. In
  // particular GXEnableBreakPt relies on the GP read pointer *stopping* at the
  // programmed address and raising the CP interrupt before later FIFO bytes
  // are consumed. Teleporting read := write skips every breakpoint between
  // the two pointers and leaves games spinning forever waiting for their
  // GXBreakPtCallback.
  if (!GPReadEnabled()) return;

  while (fifo_rw_distance_ != 0u) {
    if (BreakpointActive()) break;
    if (fifo_rw_distance_ < 32u) break;
    if (read_burst_ && !read_burst_(read_burst_user_, fifo_read_pointer_)) break;

    const std::uint32_t before = fifo_read_pointer_;
    AdvancePointer(&fifo_read_pointer_);
    if (fifo_rw_distance_ >= 32u)
      fifo_rw_distance_ -= 32u;
    else
      fifo_rw_distance_ = 0u;

    if (BreakpointActive()) break;
    if (fifo_read_pointer_ == fifo_write_pointer_) {
      fifo_rw_distance_ = 0u;
      break;
    }
    if (fifo_read_pointer_ == before) break;
  }
}

bool NativeCP::StepPastHandledBreakpointOnce() {
  // Do not weaken ordinary GX breakpoint semantics. This hook is only legal
  // after the guest CP interrupt handler has disabled BPInt while leaving the
  // breakpoint itself armed, and only when the runtime has separately observed
  // a multi-frame all-thread idle deadlock.
  if (!GPReadEnabled() || !BreakpointActive() || BreakpointInterruptEnabled() ||
      fifo_rw_distance_ < 32u)
    return false;

  if (read_burst_ && !read_burst_(read_burst_user_, fifo_read_pointer_))
    return false;

  const std::uint32_t before = fifo_read_pointer_;
  AdvancePointer(&fifo_read_pointer_);
  fifo_rw_distance_ -= 32u;
  if (fifo_read_pointer_ == fifo_write_pointer_)
    fifo_rw_distance_ = 0u;

  return fifo_read_pointer_ != before;
}

bool NativeCP::NotifyGatherWrite(std::uint32_t bytes) {
  gather_bytes_ += bytes;
  bool changed = false;
  while (gather_bytes_ >= 32u) {
    gather_bytes_ -= 32u;
    if (!GPLinkEnabled()) continue;
    changed = true;
    AdvancePointer(&fifo_write_pointer_);
    // SDK top is base + size - 4, but WriteLow/WriteHigh store the CP
    // register with its low five bits cleared. fifo_end_ therefore names
    // the final 32-byte burst, not the last word of that burst.
    const std::uint32_t fifo_span =
        fifo_end_ >= fifo_base_ ? (fifo_end_ - fifo_base_ + 32u) : 0u;
    if (fifo_span != 0u)
      fifo_rw_distance_ = std::min(fifo_rw_distance_ + 32u, fifo_span);
    else
      fifo_rw_distance_ += 32u;
    if (GPReadEnabled()) DrainSynchronousGpu();
  }
  return changed;
}

} // namespace GekkoAOT::HW::CP
