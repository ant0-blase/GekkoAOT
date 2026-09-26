// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/pi/native_pi.h"

#include <cstdio>

namespace GekkoAOT::HW::PI {
namespace {
constexpr std::uint32_t kInterruptCause = 0x00u;
constexpr std::uint32_t kInterruptMask = 0x04u;
constexpr std::uint32_t kFifoBase = 0x0cu;
constexpr std::uint32_t kFifoEnd = 0x10u;
constexpr std::uint32_t kFifoWritePointer = 0x14u;
constexpr std::uint32_t kFifoReset = 0x18u;
constexpr std::uint32_t kErrorCause = 0x1cu;
constexpr std::uint32_t kErrorAddress = 0x20u;
constexpr std::uint32_t kResetCode = 0x24u;
constexpr std::uint32_t kUnknown = 0x28u;
constexpr std::uint32_t kFlipperRevision = 0x2cu;
constexpr std::uint32_t kFlipperBusStrength = 0x30u;
constexpr std::uint32_t kFlipperRevisionC = 0x246500b1u;
constexpr std::uint32_t kGcPhysicalFifoMask = 0x03ffffe0u;
}

void NativePI::Reset() {
  interrupt_mask_ = 0;
  // Real GameCube boot state: reset button is unpressed and VI starts asserted.
  interrupt_cause_ = ResetButtonState | Video;
  fifo_cpu_base_ = 0;
  fifo_cpu_end_ = 0;
  fifo_cpu_write_pointer_ = 0;
  error_cause_ = 0;
  error_address_ = 0;
  reset_code_ = 0;
  unknown_ = 0x000001ffu;
  flipper_bus_strength_ = 0x02492492u;
  fifo_reset_requested_ = false;
}

bool NativePI::Handles(std::uint32_t address) const {
  const std::uint32_t physical = ToPhysical(address);
  return physical >= BasePhysical && physical < BasePhysical + WindowSize;
}

bool NativePI::Read32(std::uint32_t offset, std::uint32_t* value) const {
  if (!value) return false;
  switch (offset) {
  case kInterruptCause: *value = interrupt_cause_; return true;
  case kInterruptMask: *value = interrupt_mask_; return true;
  case kFifoBase: *value = fifo_cpu_base_; return true;
  case kFifoEnd: *value = fifo_cpu_end_; return true;
  case kFifoWritePointer: *value = fifo_cpu_write_pointer_; return true;
  case kErrorCause: *value = error_cause_; return true;
  case kErrorAddress: *value = error_address_; return true;
  case kResetCode: *value = reset_code_; return true;
  case kUnknown: *value = unknown_; return true;
  case kFlipperRevision: *value = kFlipperRevisionC; return true;
  case kFlipperBusStrength: *value = flipper_bus_strength_; return true;
  default: return false;
  }
}

bool NativePI::Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value) const {
  if (!value || !Handles(address)) return false;
  const std::uint32_t physical = ToPhysical(address);
  const std::uint32_t offset = physical - BasePhysical;

  if (size == 4 && (offset & 3u) == 0) {
    std::uint32_t result = 0;
    if (!Read32(offset, &result)) return false;
    *value = result;
    return true;
  }

  // Hardware permits 16-bit reads of PI by splitting the aligned 32-bit value.
  if (size == 2 && (offset & 1u) == 0) {
    std::uint32_t result = 0;
    if (!Read32(offset & ~3u, &result)) return false;
    *value = (offset & 2u) ? (result & 0xffffu) : (result >> 16);
    return true;
  }

  return false;
}

bool NativePI::Write32(std::uint32_t offset, std::uint32_t value) {
  switch (offset) {
  case kInterruptCause: {
    // PI interrupt cause is write-one-to-clear.
    const std::uint32_t before = interrupt_cause_;
    interrupt_cause_ &= ~value;
    if ((value & Video) != 0u || ((before ^ interrupt_cause_) & Video) != 0u) {
      static unsigned cause_write_logs = 0u;
      if (cause_write_logs++ < 64u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_VI_IRQ_TRACE_V81=1 phase=pi-cause-w1c value=%08x before=%08x after=%08x mask=%08x\n",
                     value, before, interrupt_cause_, interrupt_mask_);
    }
    return true;
  }
  case kInterruptMask: {
    const std::uint32_t before = interrupt_mask_;
    interrupt_mask_ = value;
    if (before != value) {
      static unsigned mask_logs = 0u;
      if (mask_logs++ < 96u)
        std::fprintf(stderr,
                     "GEKKOAOT_NATIVE_VI_IRQ_TRACE_V81=1 phase=pi-mask before=%08x after=%08x video=%u cause=%08x\n",
                     before, interrupt_mask_, (interrupt_mask_ & Video) ? 1u : 0u,
                     interrupt_cause_);
    }
    return true;
  }
  case kFifoBase:
    fifo_cpu_base_ = value & kGcPhysicalFifoMask;
    return true;
  case kFifoEnd:
    fifo_cpu_end_ = value & kGcPhysicalFifoMask;
    return true;
  case kFifoWritePointer:
    fifo_cpu_write_pointer_ = value & kGcPhysicalFifoMask;
    return true;
  case kFifoReset:
    if (value & 1u) fifo_reset_requested_ = true;
    return true;
  case kErrorCause:
    error_cause_ = value & 0x7u;
    return true;
  case kResetCode:
    reset_code_ = value;
    return true;
  case kUnknown:
    unknown_ = value & 0x3ffu;
    return true;
  case kFlipperBusStrength:
    flipper_bus_strength_ = value & 0x07ffffffu;
    return true;
  case kErrorAddress:
  case kFlipperRevision:
  default:
    return false;
  }
}

bool NativePI::Write(std::uint32_t address, std::uint64_t value, std::uint8_t size) {
  if (!Handles(address) || size != 4) return false;
  const std::uint32_t offset = ToPhysical(address) - BasePhysical;
  if ((offset & 3u) != 0) return false;
  return Write32(offset, static_cast<std::uint32_t>(value));
}

void NativePI::SetFifoWritePointerFromCP(std::uint32_t value) {
  fifo_cpu_write_pointer_ = value & kGcPhysicalFifoMask;
}

void NativePI::AdvanceCpuFifoWritePointer() {
  if (fifo_cpu_base_ == 0u && fifo_cpu_end_ == 0u) return;
  if (fifo_cpu_write_pointer_ == fifo_cpu_end_ || fifo_cpu_write_pointer_ > fifo_cpu_end_)
    fifo_cpu_write_pointer_ = fifo_cpu_base_;
  else
    fifo_cpu_write_pointer_ = (fifo_cpu_write_pointer_ + 32u) & kGcPhysicalFifoMask;
}

void NativePI::SetInterrupt(std::uint32_t cause, bool set) {
  const bool already_set = (interrupt_cause_ & cause) != 0u;
  if (already_set == set) return;
  const std::uint32_t before = interrupt_cause_;
  if (set)
    interrupt_cause_ |= cause;
  else
    interrupt_cause_ &= ~cause;

  if (cause == Video) {
    static unsigned video_logs = 0u;
    if (video_logs++ < 128u)
      std::fprintf(stderr,
                   "GEKKOAOT_NATIVE_VI_IRQ_TRACE_V81=1 phase=pi-level set=%u before=%08x after=%08x mask=%08x pending=%u\n",
                   set ? 1u : 0u, before, interrupt_cause_, interrupt_mask_,
                   InterruptPending() ? 1u : 0u);
  }
}

bool NativePI::ConsumeFifoResetRequested() {
  const bool requested = fifo_reset_requested_;
  fifo_reset_requested_ = false;
  return requested;
}

} // namespace GekkoAOT::HW::PI
