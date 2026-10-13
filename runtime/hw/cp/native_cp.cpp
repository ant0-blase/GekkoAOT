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
  breakpoint_write_mask_ = 0;
  register_drain_pending_ = false;
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
  case kControl: {
    const auto previous = control_;
    control_ = value & 0x003fu;
    // Only defer a release of an armed breakpoint with queued work. Ordinary
    // GP enable/gather paths retain their synchronous fast-path contract.
    if (defer_register_drain_ && (previous & kCtrlBpEnable) != 0u &&
        (control_ & kCtrlBpEnable) == 0u && GPReadEnabled() &&
        fifo_rw_distance_ != 0u)
      register_drain_pending_ = true;
    if (GPReadEnabled()) RequestRegisterDrain();
    return true;
  }
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
  case kFifoBreakpointLo:
    WriteLow(&fifo_breakpoint_, value);
    breakpoint_write_mask_ |= 0x1u;
    // The SDK programs CPBreakpoint through two adjacent 16-bit MMIO writes.
    // Draining after the first half exposes a transient address to the
    // synchronous GPU.  In particular, moving 0x0035xxxx -> 0x0036xxxx with
    // the low half written first can make the reader run past the intended
    // stop before the high half arrives.  Commit GPU progress only after both
    // halves have been observed; either write order is accepted.
    if (breakpoint_write_mask_ == 0x3u) {
      breakpoint_write_mask_ = 0;
      RequestRegisterDrain();
    }
    return true;
  case kFifoBreakpointHi:
    WriteHigh(&fifo_breakpoint_, value);
    breakpoint_write_mask_ |= 0x2u;
    if (breakpoint_write_mask_ == 0x3u) {
      breakpoint_write_mask_ = 0;
      RequestRegisterDrain();
    }
    return true;
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

void NativeCP::RequestRegisterDrain() {
  if (register_drain_pending_) return;
  DrainSynchronousGpu();
}

void NativeCP::ServiceRegisterDrain() {
  if (!register_drain_pending_) return;
  register_drain_pending_ = false;
  DrainSynchronousGpu();
}

void NativeCP::DrainSynchronousGpu() {
  // NativeGX consumes the raw gather stream synchronously, but guest-visible
  // CP state must still advance exactly as the Flipper FIFO does. In
  // particular GXEnableBreakPt relies on the GP read pointer *stopping* at the
  // programmed address and raising the CP interrupt before later FIFO bytes
  // are consumed. Teleporting read := write skips every breakpoint between
  // the two pointers and leaves games spinning forever waiting for their
  // GXBreakPtCallback.
  if (!GPReadEnabled() || register_drain_pending_) return;

  // Synchronous GekkoAOT normally receives exactly one newly completed gather
  // line and drains it immediately. Handle that overwhelmingly common case
  // without entering the general backlog/breakpoint loop.
  if (fifo_rw_distance_ == 32u && !BreakpointActive()) {
    if (read_burst_ && !read_burst_(read_burst_user_, fifo_read_pointer_)) return;
    AdvancePointer(&fifo_read_pointer_);
    fifo_rw_distance_ = 0u;
    return;
  }

  // v161: a renderer call per 32-byte gather line becomes expensive whenever
  // the guest lets the CP FIFO build a backlog (MOH Frontline is a prominent
  // example).  With no breakpoint armed, process a contiguous run in one host
  // callback.  Stop at the ring end so HostRuntime can resolve one contiguous
  // MEM1 span.  Breakpoint traffic remains on the exact one-line path below.
  constexpr std::uint32_t kMaxBatchBursts = 128u;
  while (fifo_rw_distance_ >= 64u && !BreakpointActive() && read_burst_batch_) {
    const std::uint32_t available = fifo_rw_distance_ / 32u;
    const std::uint32_t bytes_to_end =
        fifo_read_pointer_ <= fifo_end_ ? fifo_end_ - fifo_read_pointer_ : 0u;
    const std::uint32_t contiguous = bytes_to_end / 32u + 1u;
    std::uint32_t batch = std::min({available, contiguous, kMaxBatchBursts});

    // A programmed CP breakpoint is a FIFO *boundary*, not merely a status
    // condition to sample before/after a host batch.  The v161 batch path used
    // BreakpointActive() as its only guard, which is false until the read
    // pointer is already equal to CPBreakpoint.  A multi-burst callback could
    // therefore advance from before the breakpoint to after it in one shot and
    // permanently miss the CP interrupt.  Star Fox Adventures relies on this
    // interrupt to wake its video-flip OSThreadQueue.
    //
    // Keep batching, but never let one callback cross an enabled breakpoint
    // that lies inside the currently queued FIFO distance.  Consuming exactly
    // up to the boundary is valid: after the pointer advance below,
    // BreakpointActive() becomes true and the normal CP status/IRQ path fires.
    if ((control_ & kCtrlBpEnable) != 0u && fifo_base_ <= fifo_breakpoint_ &&
        fifo_breakpoint_ <= fifo_end_ && fifo_base_ <= fifo_read_pointer_ &&
        fifo_read_pointer_ <= fifo_end_) {
      std::uint32_t bytes_to_breakpoint = 0u;
      if (fifo_breakpoint_ >= fifo_read_pointer_) {
        bytes_to_breakpoint = fifo_breakpoint_ - fifo_read_pointer_;
      } else {
        bytes_to_breakpoint = (fifo_end_ - fifo_read_pointer_ + 32u) +
                              (fifo_breakpoint_ - fifo_base_);
      }

      if (bytes_to_breakpoint != 0u && bytes_to_breakpoint <= fifo_rw_distance_)
        batch = std::min(batch, bytes_to_breakpoint / 32u);
    }

    // A one-burst approach to the breakpoint must use the exact path below so
    // every boundary retains the same callback/accounting semantics.
    if (batch < 2u || !read_burst_batch_(read_burst_batch_user_, fifo_read_pointer_, batch)) break;

    const std::uint32_t consumed = batch * 32u;
    // v241: this accepted batch is already capped to a contiguous ring span.
    // Advance its pointer once; the old loop had no callback/IRQ side effects
    // between bursts. Retain its exact fallback for malformed FIFO bounds.
    if ((fifo_base_ != 0u || fifo_end_ != 0u) &&
        fifo_base_ <= fifo_read_pointer_ && fifo_read_pointer_ <= fifo_end_) {
      const std::uint64_t next = static_cast<std::uint64_t>(fifo_read_pointer_) + consumed;
      fifo_read_pointer_ = next > fifo_end_ ? fifo_base_ :
                          static_cast<std::uint32_t>(next) & kAddressMask;
    } else {
      for (std::uint32_t i = 0; i < batch; ++i) AdvancePointer(&fifo_read_pointer_);
    }
    fifo_rw_distance_ = fifo_rw_distance_ > consumed ? fifo_rw_distance_ - consumed : 0u;
    if (fifo_read_pointer_ == fifo_write_pointer_) {
      fifo_rw_distance_ = 0u;
      return;
    }
  }

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

bool NativeCP::NotifyDirectGatherWrite(std::uint32_t bytes) {
  if (bytes != 32u || gather_bytes_ != 0u || register_drain_pending_ || !GPLinkEnabled() ||
      !GPReadEnabled() || BreakpointActive() || fifo_rw_distance_ != 0u ||
      fifo_read_pointer_ != fifo_write_pointer_)
    return false;

  // Publish the CPU write to CP state, then consume exactly that line. The
  // renderer already received these bytes directly from HostRuntime, so no
  // memory callback is required. Advancing onto a programmed breakpoint is
  // intentional: the next CP status/IRQ synchronization observes it.
  AdvancePointer(&fifo_write_pointer_);
  fifo_rw_distance_ = 32u;
  AdvancePointer(&fifo_read_pointer_);
  fifo_rw_distance_ = 0u;
  return true;
}

bool NativeCP::NotifyGatherWrite(std::uint32_t bytes) {
  // HostRuntime publishes one complete hardware gather line at a time. Avoid
  // accumulator-loop overhead on that steady-state renderer path.
  if (bytes == 32u && gather_bytes_ == 0u) {
    if (!GPLinkEnabled()) return false;
    AdvancePointer(&fifo_write_pointer_);
    const std::uint32_t fifo_span =
        fifo_end_ >= fifo_base_ ? (fifo_end_ - fifo_base_ + 32u) : 0u;
    if (fifo_span != 0u)
      fifo_rw_distance_ = std::min(fifo_rw_distance_ + 32u, fifo_span);
    else
      fifo_rw_distance_ += 32u;
    if (GPReadEnabled()) DrainSynchronousGpu();
    return true;
  }

  gather_bytes_ += bytes;
  bool changed = false;
  while (gather_bytes_ >= 32u) {
    gather_bytes_ -= 32u;
    if (!GPLinkEnabled()) continue;
    changed = true;
    AdvancePointer(&fifo_write_pointer_);
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
