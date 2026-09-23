// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/mi/native_mi.h"

namespace GekkoAOT::HW::MI {
namespace {
constexpr std::uint32_t kRegion0First = 0x000u;
constexpr std::uint32_t kRegion3Last = 0x00eu;
constexpr std::uint32_t kProtType = 0x010u;
constexpr std::uint32_t kIrqMask = 0x01cu;
constexpr std::uint32_t kIrqFlag = 0x01eu;
constexpr std::uint32_t kUnknown1 = 0x020u;
constexpr std::uint32_t kProtAddrLo = 0x022u;
constexpr std::uint32_t kProtAddrHi = 0x024u;
constexpr std::uint32_t kTimer0Hi = 0x032u;
constexpr std::uint32_t kTimer9Lo = 0x058u;
constexpr std::uint32_t kUnknown2 = 0x05au;
}

void NativeMI::Reset() {
  region_first_.fill(0);
  region_last_.fill(0);
  prot_type_ = 0;
  irq_mask_ = 0;
  irq_flag_ = 0;
  unknown1_ = 0;
  prot_addr_lo_register_ = 0;
  prot_addr_hi_register_ = 0;
  timers_.fill(0);
  unknown2_ = 0;
}

bool NativeMI::Handles(std::uint32_t address) const {
  const std::uint32_t physical = ToPhysical(address);
  return physical >= BasePhysical && physical < BasePhysical + WindowSize;
}

bool NativeMI::Read16(std::uint32_t offset, std::uint16_t* value) const {
  if (!value || (offset & 1u)) return false;

  if (offset >= kRegion0First && offset <= kRegion3Last) {
    const std::size_t region = offset / 4u;
    *value = (offset & 2u) ? region_last_[region] : region_first_[region];
    return true;
  }

  switch (offset) {
  case kProtType: *value = prot_type_; return true;
  case kIrqMask: *value = irq_mask_; return true;
  case kIrqFlag: *value = irq_flag_; return true;
  case kUnknown1: *value = unknown1_; return true;
  case kProtAddrLo: *value = prot_addr_lo_register_; return true;
  case kProtAddrHi: *value = prot_addr_hi_register_; return true;
  case kUnknown2: *value = unknown2_; return true;
  default: break;
  }

  if (offset >= kTimer0Hi && offset <= kTimer9Lo) {
    const std::uint32_t relative = offset - kTimer0Hi;
    if ((relative & 1u) != 0) return false;
    const std::size_t timer = relative / 4u;
    if (timer >= timers_.size()) return false;
    *value = (relative & 2u)
                 ? static_cast<std::uint16_t>(timers_[timer] & 0xffffu)
                 : static_cast<std::uint16_t>(timers_[timer] >> 16);
    return true;
  }

  return false;
}

bool NativeMI::Write16(std::uint32_t offset, std::uint16_t value) {
  if (offset & 1u) return false;

  if (offset >= kRegion0First && offset <= kRegion3Last) {
    const std::size_t region = offset / 4u;
    if (offset & 2u)
      region_last_[region] = value;
    else
      region_first_[region] = value;
    return true;
  }

  switch (offset) {
  case kProtType: prot_type_ = value; return true;
  case kIrqMask: irq_mask_ = value; return true;
  case kIrqFlag: irq_flag_ = value; return true;
  case kUnknown1: unknown1_ = value; return true;
  case kProtAddrLo: prot_addr_lo_register_ = value; return true;
  case kProtAddrHi: prot_addr_hi_register_ = value; return true;
  case kUnknown2: unknown2_ = value; return true;
  default: break;
  }

  if (offset >= kTimer0Hi && offset <= kTimer9Lo) {
    const std::uint32_t relative = offset - kTimer0Hi;
    if ((relative & 1u) != 0) return false;
    const std::size_t timer = relative / 4u;
    if (timer >= timers_.size()) return false;
    if (relative & 2u)
      timers_[timer] = (timers_[timer] & 0xffff0000u) | value;
    else
      timers_[timer] = (timers_[timer] & 0x0000ffffu) | (static_cast<std::uint32_t>(value) << 16);
    return true;
  }

  return false;
}

bool NativeMI::Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value) const {
  if (!value || !Handles(address)) return false;
  const std::uint32_t offset = ToPhysical(address) - BasePhysical;

  if (size == 2 && (offset & 1u) == 0) {
    std::uint16_t result = 0;
    if (!Read16(offset, &result)) return false;
    *value = result;
    return true;
  }

  // Dolphin maps aligned 32-bit MI accesses to the two 16-bit registers in
  // big-endian guest order: lower address is the high halfword.
  if (size == 4 && (offset & 3u) == 0) {
    std::uint16_t hi = 0, lo = 0;
    if (!Read16(offset, &hi) || !Read16(offset + 2u, &lo)) return false;
    *value = (static_cast<std::uint32_t>(hi) << 16) | lo;
    return true;
  }

  return false;
}

bool NativeMI::Write(std::uint32_t address, std::uint64_t value, std::uint8_t size) {
  if (!Handles(address)) return false;
  const std::uint32_t offset = ToPhysical(address) - BasePhysical;

  if (size == 2 && (offset & 1u) == 0)
    return Write16(offset, static_cast<std::uint16_t>(value));

  if (size == 4 && (offset & 3u) == 0) {
    // Do not partially commit an access if either half is an unmapped MI register.
    std::uint16_t ignored = 0;
    if (!Read16(offset, &ignored) || !Read16(offset + 2u, &ignored)) return false;
    const auto hi = static_cast<std::uint16_t>((value >> 16) & 0xffffu);
    const auto lo = static_cast<std::uint16_t>(value & 0xffffu);
    return Write16(offset, hi) && Write16(offset + 2u, lo);
  }

  return false;
}

} // namespace GekkoAOT::HW::MI
