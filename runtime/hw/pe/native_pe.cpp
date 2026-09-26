// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/pe/native_pe.h"

namespace GekkoAOT::HW::PE {
namespace {
constexpr std::uint32_t kZConf = 0x00u;
constexpr std::uint32_t kAlphaConf = 0x02u;
constexpr std::uint32_t kDstAlphaConf = 0x04u;
constexpr std::uint32_t kAlphaMode = 0x06u;
constexpr std::uint32_t kAlphaRead = 0x08u;
constexpr std::uint32_t kControl = 0x0au;
constexpr std::uint32_t kToken = 0x0eu;
constexpr std::uint32_t kBboxLeft = 0x10u;
constexpr std::uint32_t kBboxRight = 0x12u;
constexpr std::uint32_t kBboxTop = 0x14u;
constexpr std::uint32_t kBboxBottom = 0x16u;
constexpr std::uint32_t kPerfFirst = 0x18u;
constexpr std::uint32_t kPerfLast = 0x2eu;
}

void NativePE::Reset() {
  config_.fill(0);
  control_ = 0;
  token_ = 0;
  bbox_.fill(0);
  token_signal_ = false;
  finish_signal_ = false;
}

bool NativePE::Handles(std::uint32_t address) const {
  const std::uint32_t physical = ToPhysical(address);
  return physical >= BasePhysical && physical < BasePhysical + WindowSize;
}

bool NativePE::Read16(std::uint32_t offset, std::uint16_t* value) const {
  if (!value) return false;
  switch (offset) {
  case kZConf: *value = config_[0]; return true;
  case kAlphaConf: *value = config_[1]; return true;
  case kDstAlphaConf: *value = config_[2]; return true;
  case kAlphaMode: *value = config_[3]; return true;
  case kAlphaRead: *value = config_[4]; return true;
  case kControl:
    // Signal/acknowledge bits are write-only. Reads expose the two enables.
    *value = control_ & static_cast<std::uint16_t>(kTokenEnable | kFinishEnable);
    return true;
  case kToken: *value = token_; return true;
  case kBboxLeft: *value = bbox_[0]; return true;
  case kBboxRight: *value = bbox_[1]; return true;
  case kBboxTop: *value = bbox_[2]; return true;
  case kBboxBottom: *value = bbox_[3]; return true;
  default:
    // Performance-query registers are read-only.  NativeGX/Aurora does not yet
    // export query counters through the standalone ABI, so expose the hardware
    // register bank with a neutral zero value rather than routing to Dolphin.
    if (offset >= kPerfFirst && offset <= kPerfLast && (offset & 1u) == 0u) {
      *value = 0;
      return true;
    }
    return false;
  }
}

bool NativePE::Write16(std::uint32_t offset, std::uint16_t value) {
  switch (offset) {
  case kZConf: config_[0] = value; return true;
  case kAlphaConf: config_[1] = value; return true;
  case kDstAlphaConf: config_[2] = value; return true;
  case kAlphaMode: config_[3] = value; return true;
  case kAlphaRead: config_[4] = value; return true;
  case kControl:
    if ((value & kTokenAcknowledge) != 0u) token_signal_ = false;
    if ((value & kFinishAcknowledge) != 0u) finish_signal_ = false;
    control_ = value & static_cast<std::uint16_t>(kTokenEnable | kFinishEnable);
    return true;
  case kToken:
  case kBboxLeft:
  case kBboxRight:
  case kBboxTop:
  case kBboxBottom:
    return false;
  default:
    return false;
  }
}

bool NativePE::Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value) const {
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

bool NativePE::Write(std::uint32_t address, std::uint64_t value, std::uint8_t size) {
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

void NativePE::SetToken(std::uint16_t token, bool interrupt) {
  token_ = token;
  if (interrupt) token_signal_ = true;
}

void NativePE::SetFinish() {
  finish_signal_ = true;
}

bool NativePE::TokenInterruptPending() const {
  return token_signal_ && (control_ & kTokenEnable) != 0u;
}

bool NativePE::FinishInterruptPending() const {
  return finish_signal_ && (control_ & kFinishEnable) != 0u;
}

} // namespace GekkoAOT::HW::PE
