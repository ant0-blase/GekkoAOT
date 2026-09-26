// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/si/native_si.h"

namespace GekkoAOT::HW::SI {

void NativeSI::Reset() {
  channels_ = {};
  // Retail SI reset state uses X=492 lines. Y and all polling-enable bits are
  // zero until the SDK configures a sampling rate.
  poll_ = 492u << 16;
  com_csr_ = 0;
  status_ = 0;
  exi_clock_count_ = 0;
  buffer_.fill(0);
}

bool NativeSI::Handles(std::uint32_t address) const {
  const std::uint32_t physical = ToPhysical(address);
  return physical >= BasePhysical && physical < BasePhysical + WindowSize;
}

bool NativeSI::ReadBuffer(std::uint32_t offset, std::uint8_t size,
                          std::uint64_t* value) const {
  if (!value || offset < kIoBuffer || offset + size > kIoBufferEnd) return false;
  if (size != 1u && size != 2u && size != 4u) return false;
  if ((size == 4u && (offset & 3u)) || (size == 2u && (offset & 1u))) return false;
  const std::uint32_t index = offset - kIoBuffer;
  std::uint64_t out = 0;
  for (std::uint32_t i = 0; i < size; ++i)
    out = (out << 8) | buffer_[index + i];
  *value = out;
  return true;
}

bool NativeSI::WriteBuffer(std::uint32_t offset, std::uint64_t value,
                           std::uint8_t size) {
  if (offset < kIoBuffer || offset + size > kIoBufferEnd) return false;
  if (size != 1u && size != 2u && size != 4u) return false;
  if ((size == 4u && (offset & 3u)) || (size == 2u && (offset & 1u))) return false;
  const std::uint32_t index = offset - kIoBuffer;
  for (std::uint32_t i = 0; i < size; ++i) {
    const std::uint32_t shift = (size - 1u - i) * 8u;
    buffer_[index + i] = static_cast<std::uint8_t>(value >> shift);
  }
  return true;
}

bool NativeSI::ReadRegister32(std::uint32_t offset, std::uint32_t* value) {
  if (!value) return false;

  if (offset < kPoll) {
    const std::uint32_t channel = offset / 0x0cu;
    const std::uint32_t reg = offset % 0x0cu;
    if (channel >= ChannelCount) return false;
    switch (reg) {
    case 0x00u:
      *value = channels_[channel].out;
      return true;
    case 0x04u:
      *value = channels_[channel].in_hi;
      status_ &= ~ReadDataBit(channel);
      UpdateInterruptState();
      return true;
    case 0x08u:
      *value = channels_[channel].in_lo;
      status_ &= ~ReadDataBit(channel);
      UpdateInterruptState();
      return true;
    default:
      return false;
    }
  }

  switch (offset) {
  case kPoll: *value = poll_; return true;
  case kComCsr: *value = com_csr_; return true;
  case kStatus: *value = status_; return true;
  case kExiClockCount: *value = exi_clock_count_; return true;
  default: return false;
  }
}

bool NativeSI::Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value) {
  if (!Handles(address) || !value) return false;
  const std::uint32_t offset = ToPhysical(address) - BasePhysical;
  if (offset >= kIoBuffer && offset < kIoBufferEnd)
    return ReadBuffer(offset, size, value);

  // Retail SI registers are 32-bit MMIO. The transfer RAM separately permits
  // 16/32-bit accesses (and byte reads are harmless for the standalone host).
  if (size != 4u || (offset & 3u)) return false;
  std::uint32_t word = 0;
  if (!ReadRegister32(offset, &word)) return false;
  *value = word;
  return true;
}

void NativeSI::SetNoResponse(std::uint32_t channel) {
  if (channel >= ChannelCount) return;
  status_ |= NoResponseBit(channel);
  // SI input high: bit 30 ERRLATCH, bit 31 ERRSTAT.
  channels_[channel].in_hi |= 0xc0000000u;
}

void NativeSI::UpdateInterruptState() {
  const bool any_read_data = (status_ & (ReadDataBit(0) | ReadDataBit(1) |
                                         ReadDataBit(2) | ReadDataBit(3))) != 0u;
  if (any_read_data)
    com_csr_ |= kComRdstInt;
  else
    com_csr_ &= ~kComRdstInt;
}

bool NativeSI::InterruptPending() const {
  return ((com_csr_ & kComRdstInt) && (com_csr_ & kComRdstIntMask)) ||
         ((com_csr_ & kComTcInt) && (com_csr_ & kComTcIntMask));
}

void NativeSI::RunBufferTransfer() {
  const std::uint32_t channel = (com_csr_ & kComChannelMask) >> 1;
  const std::uint32_t request_length = DecodeLength((com_csr_ & kComOutLengthMask) >> 16);
  const std::uint32_t expected_length = DecodeLength((com_csr_ & kComInLengthMask) >> 8);

  int response = -1; // no native device attached by default
  if (hooks_.buffer)
    response = hooks_.buffer(hooks_.user, channel, buffer_.data(),
                             request_length, expected_length);

  if (response == 0)
    return; // asynchronous device deliberately left TSTART asserted

  com_csr_ &= ~kComTStart;
  if (response < 0) {
    com_csr_ |= kComError;
    SetNoResponse(channel);
  } else {
    com_csr_ &= ~kComError;
  }
  com_csr_ |= kComTcInt;
  UpdateInterruptState();
}

void NativeSI::SendDirectCommands() {
  for (std::uint32_t channel = 0; channel < ChannelCount; ++channel) {
    if (hooks_.direct)
      hooks_.direct(hooks_.user, channel, channels_[channel].out,
                    (poll_ & PollEnableBit(channel)) != 0u);
    status_ &= ~WriteStatusBit(channel);
  }
  status_ &= ~kStatusWrite;
}

bool NativeSI::WriteRegister32(std::uint32_t offset, std::uint32_t value) {
  if (offset < kPoll) {
    const std::uint32_t channel = offset / 0x0cu;
    const std::uint32_t reg = offset % 0x0cu;
    if (channel >= ChannelCount) return false;
    switch (reg) {
    case 0x00u: channels_[channel].out = value; return true;
    // Dolphin exposes these as direct writes too. Keep that behavior for
    // compatibility with diagnostics even though games normally only read them.
    case 0x04u: channels_[channel].in_hi = value; return true;
    case 0x08u: channels_[channel].in_lo = value; return true;
    default: return false;
    }
  }

  switch (offset) {
  case kPoll:
    poll_ = value;
    return true;

  case kComCsr: {
    constexpr std::uint32_t state_mask =
        kComChannelMask | kComInLengthMask | kComOutLengthMask |
        kComRdstIntMask | kComTcIntMask;
    com_csr_ = (com_csr_ & ~state_mask) | (value & state_mask);
    if (value & kComRdstInt) com_csr_ &= ~kComRdstInt;
    if (value & kComTcInt) com_csr_ &= ~kComTcInt;

    if (value & kComTStart) {
      com_csr_ |= kComTStart;
      RunBufferTransfer();
    }
    UpdateInterruptState();
    return true;
  }

  case kStatus:
    // UNRUN/OVRUN/COLL/NOREP are read/write-clear bits. RDST/WRST are state.
    for (std::uint32_t channel = 0; channel < ChannelCount; ++channel)
      status_ &= ~(value & ErrorBits(channel));
    if (value & kStatusWrite) SendDirectCommands();
    UpdateInterruptState();
    return true;

  case kExiClockCount:
    exi_clock_count_ = value;
    return true;

  default:
    return false;
  }
}

bool NativeSI::Write(std::uint32_t address, std::uint64_t value, std::uint8_t size) {
  if (!Handles(address)) return false;
  const std::uint32_t offset = ToPhysical(address) - BasePhysical;
  if (offset >= kIoBuffer && offset < kIoBufferEnd)
    return WriteBuffer(offset, value, size);
  if (size != 4u || (offset & 3u)) return false;
  return WriteRegister32(offset, static_cast<std::uint32_t>(value));
}

void NativeSI::PollNow() {
  for (std::uint32_t channel = 0; channel < ChannelCount; ++channel) {
    // SIPOLL.EN0..EN3 gate automatic controller sampling per channel.
    // Disabled ports may still take explicit COMCSR buffer transfers.
    if ((poll_ & PollEnableBit(channel)) == 0u) continue;
    const std::uint32_t old_err_latch = channels_[channel].in_hi & 0x40000000u;
    std::uint32_t hi = 0, lo = 0;
    const bool fresh = hooks_.poll && hooks_.poll(hooks_.user, channel, &hi, &lo);
    if (fresh) {
      channels_[channel].in_hi = (hi & ~0x40000000u) | old_err_latch;
      channels_[channel].in_lo = lo;
      status_ |= ReadDataBit(channel);
    } else {
      SetNoResponse(channel);
      channels_[channel].in_hi |= old_err_latch;
    }
  }
  UpdateInterruptState();
}

} // namespace GekkoAOT::HW::SI
