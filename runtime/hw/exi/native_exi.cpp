// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/exi/native_exi.h"

namespace GekkoAOT::HW::EXI {
namespace {
constexpr std::uint32_t kStatus = 0x00u;
constexpr std::uint32_t kDmaAddress = 0x04u;
constexpr std::uint32_t kDmaLength = 0x08u;
constexpr std::uint32_t kControl = 0x0cu;
constexpr std::uint32_t kImmediateData = 0x10u;

std::uint64_t SliceBigEndian(std::uint32_t word, std::uint32_t byte_offset, std::uint8_t size) {
  if (size == 4u) return word;
  if (size == 2u) return (word >> ((2u - byte_offset) * 8u)) & 0xffffu;
  return (word >> ((3u - byte_offset) * 8u)) & 0xffu;
}

std::uint32_t MergeBigEndian(std::uint32_t old_word, std::uint32_t byte_offset,
                             std::uint8_t size, std::uint64_t value) {
  if (size == 4u) return static_cast<std::uint32_t>(value);
  const std::uint32_t width = static_cast<std::uint32_t>(size) * 8u;
  const std::uint32_t shift = (4u - byte_offset - size) * 8u;
  const std::uint32_t mask = (width == 32u ? 0xffffffffu : ((1u << width) - 1u)) << shift;
  return (old_word & ~mask) | ((static_cast<std::uint32_t>(value) << shift) & mask);
}
}

void NativeEXI::Reset() {
  channels_ = {};
  // Retail-compatible channel reset state. Channels 0/1 begin with the
  // external-event cause latched; channel 1 also powers up with CS0 selected.
  channels_[0].status = kExtInt;
  channels_[1].status = kExtInt | (1u << 7);
}

bool NativeEXI::Handles(std::uint32_t address) const {
  const std::uint32_t physical = ToPhysical(address);
  return physical >= BasePhysical && physical < BasePhysical + WindowSize;
}

bool NativeEXI::Decode(std::uint32_t address, std::uint32_t* channel,
                       std::uint32_t* reg_offset) const {
  if (!channel || !reg_offset || !Handles(address)) return false;
  const std::uint32_t offset = ToPhysical(address) - BasePhysical;
  *channel = offset / ChannelStride;
  *reg_offset = offset % ChannelStride;
  return *channel < ChannelCount;
}

bool NativeEXI::DevicePresent(std::uint32_t channel, std::uint8_t chip_select) const {
  if (hooks_.present)
    return hooks_.present(hooks_.user, channel, chip_select);
  return false;
}

std::uint32_t NativeEXI::Read32(std::uint32_t channel, std::uint32_t reg_offset) const {
  const Channel& ch = channels_[channel];
  switch (reg_offset) {
  case kStatus: {
    std::uint32_t status = ch.status & ~kExternalPresent;
    // EXT reports the external slot device (CS0 / chip-select value 1) on
    // channels 0 and 1. Channel 2 does not expose an external insertion pin.
    if (channel < 2u && DevicePresent(channel, 1u)) status |= kExternalPresent;
    return status;
  }
  case kDmaAddress: return ch.dma_address;
  case kDmaLength: return ch.dma_length;
  case kControl: return ch.control;
  case kImmediateData: return ch.immediate_data;
  default: return 0;
  }
}

bool NativeEXI::ReadRegister(std::uint32_t address, std::uint8_t size,
                             std::uint64_t* value) const {
  if (!value || (size != 1u && size != 2u && size != 4u)) return false;
  std::uint32_t channel = 0, reg_offset = 0;
  if (!Decode(address, &channel, &reg_offset)) return false;
  const std::uint32_t aligned = reg_offset & ~3u;
  const std::uint32_t byte_offset = reg_offset & 3u;
  if (aligned > kImmediateData || byte_offset + size > 4u) return false;
  if ((size == 4u && byte_offset != 0u) || (size == 2u && (byte_offset & 1u))) return false;
  *value = SliceBigEndian(Read32(channel, aligned), byte_offset, size);
  return true;
}

bool NativeEXI::Write32(std::uint32_t channel, std::uint32_t reg_offset,
                        std::uint32_t value) {
  Channel& ch = channels_[channel];
  switch (reg_offset) {
  case kStatus: {
    const std::uint32_t old = ch.status;
    std::uint32_t next = old;
    // Interrupt mask bits are normal writable state; cause bits are W1C.
    next = (next & ~(kExiIntMask | kTcIntMask | kClockMask | kChipSelectMask)) |
           (value & (kExiIntMask | kTcIntMask | kClockMask | kChipSelectMask));
    if (channel < 2u)
      next = (next & ~kExtIntMask) | (value & kExtIntMask);
    if (channel == 0u)
      next = (next & ~kRomDisable) | (value & kRomDisable);
    if (value & kExiInt) next &= ~kExiInt;
    if (value & kTcInt) next &= ~kTcInt;
    if (channel < 2u && (value & kExtInt)) next &= ~kExtInt;
    // EXT is a read-only pin synthesized in Read32().
    next &= ~kExternalPresent;
    ch.status = next;
    return true;
  }
  case kDmaAddress:
    ch.dma_address = value;
    return true;
  case kDmaLength:
    ch.dma_length = value;
    return true;
  case kControl:
    ch.control = value & kControlMask;
    if (ch.control & kTransferStart) RunTransfer(channel);
    return true;
  case kImmediateData:
    ch.immediate_data = value;
    return true;
  default:
    return false;
  }
}

bool NativeEXI::WriteRegister(std::uint32_t address, std::uint64_t value,
                              std::uint8_t size) {
  if (size != 1u && size != 2u && size != 4u) return false;
  std::uint32_t channel = 0, reg_offset = 0;
  if (!Decode(address, &channel, &reg_offset)) return false;
  const std::uint32_t aligned = reg_offset & ~3u;
  const std::uint32_t byte_offset = reg_offset & 3u;
  if (aligned > kImmediateData || byte_offset + size > 4u) return false;
  if ((size == 4u && byte_offset != 0u) || (size == 2u && (byte_offset & 1u))) return false;
  std::uint32_t merged = MergeBigEndian(Read32(channel, aligned), byte_offset, size, value);
  if (aligned == kStatus && size != 4u) {
    // A partial store must not write back interrupt causes read from other
    // byte lanes. They are W1C, so ordinary read/merge/write would clear
    // an unrelated pending interrupt.
    const std::uint32_t shift = (4u - byte_offset - size) * 8u;
    const std::uint32_t lane_mask = static_cast<std::uint32_t>(
        ((1ull << (size * 8u)) - 1ull) << shift);
    merged &= ~((kExiInt | kTcInt | kExtInt) & ~lane_mask);
  }
  return Write32(channel, aligned, merged);
}

void NativeEXI::RunTransfer(std::uint32_t channel) {
  Channel& ch = channels_[channel];
  const std::uint8_t chip_select = static_cast<std::uint8_t>((ch.status & kChipSelectMask) >> 7);
  const std::uint8_t direction = static_cast<std::uint8_t>((ch.control & kDirectionMask) >> 2);
  const bool dma_mode = (ch.control & kDmaMode) != 0u;

  bool handled = false;
  if (dma_mode) {
    if (hooks_.dma)
      handled = hooks_.dma(hooks_.user, channel, chip_select, direction,
                           ch.dma_address, ch.dma_length);
  } else {
    const std::uint8_t length = static_cast<std::uint8_t>(((ch.control & kTransferLengthMask) >> 4) + 1u);
    if (hooks_.immediate)
      handled = hooks_.immediate(hooks_.user, channel, chip_select, direction,
                                 length, &ch.immediate_data);
    if (!handled && direction == Read)
      ch.immediate_data = 0xffffffffu; // open bus / no device
  }

  (void)handled;
  ch.control &= ~kTransferStart;
  ch.status |= kTcInt;
}

bool NativeEXI::InterruptPending() const {
  // Interrupt cause/mask bits live in the latched status register. Do not call
  // Read32() here: that path also probes the external-device presence pin and
  // was being executed for every CPU dispatch/timing slice.
  for (std::uint32_t i = 0; i < ChannelCount; ++i) {
    const std::uint32_t status = channels_[i].status;
    if ((status & kExiInt) && (status & kExiIntMask)) return true;
    if ((status & kTcInt) && (status & kTcIntMask)) return true;
    if ((status & kExtInt) && (status & kExtIntMask)) return true;
  }
  return false;
}

std::uint32_t NativeEXI::Status(std::uint32_t channel) const {
  return channel < ChannelCount ? Read32(channel, kStatus) : 0u;
}
std::uint32_t NativeEXI::DmaAddress(std::uint32_t channel) const {
  return channel < ChannelCount ? channels_[channel].dma_address : 0u;
}
std::uint32_t NativeEXI::DmaLength(std::uint32_t channel) const {
  return channel < ChannelCount ? channels_[channel].dma_length : 0u;
}
std::uint32_t NativeEXI::Control(std::uint32_t channel) const {
  return channel < ChannelCount ? channels_[channel].control : 0u;
}
std::uint32_t NativeEXI::ImmediateData(std::uint32_t channel) const {
  return channel < ChannelCount ? channels_[channel].immediate_data : 0u;
}

} // namespace GekkoAOT::HW::EXI
