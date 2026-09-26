#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdint>

namespace GekkoAOT::HW::PE {

// Native model of Flipper's Pixel Engine CPU-visible register block.  Raster
// state is still consumed by NativeGX/Aurora; this class owns the MMIO state
// that the GameCube SDK observes, plus token/finish interrupt latches.
class NativePE final {
public:
  static constexpr std::uint32_t BasePhysical = 0x0c001000u;
  static constexpr std::uint32_t WindowSize = 0x1000u;

  NativePE() { Reset(); }

  void Reset();
  bool Handles(std::uint32_t address) const;
  bool Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value) const;
  bool Write(std::uint32_t address, std::uint64_t value, std::uint8_t size);

  // NativeGX reports BP_PE_TOKEN_ID / BP_PE_TOKEN_INT_ID / BP_PE_DONE here.
  void SetToken(std::uint16_t token, bool interrupt);
  void SetFinish();

  bool TokenInterruptPending() const;
  bool FinishInterruptPending() const;

  std::uint16_t Control() const { return control_; }
  std::uint16_t Token() const { return token_; }
  // PE_ALPHAREAD bits 0..1: 0=force 00, 1=force FF, 2=real alpha.
  std::uint16_t AlphaReadMode() const { return config_[4] & 0x3u; }

private:
  static constexpr std::uint32_t ToPhysical(std::uint32_t address) {
    return address & 0x3fffffffu;
  }

  static constexpr std::uint16_t kTokenEnable = 1u << 0;
  static constexpr std::uint16_t kFinishEnable = 1u << 1;
  static constexpr std::uint16_t kTokenAcknowledge = 1u << 2;
  static constexpr std::uint16_t kFinishAcknowledge = 1u << 3;

  bool Read16(std::uint32_t offset, std::uint16_t* value) const;
  bool Write16(std::uint32_t offset, std::uint16_t value);

  // Z/alpha CPU-side mirrors at offsets 0x00..0x08.
  std::array<std::uint16_t, 5> config_{};
  std::uint16_t control_ = 0;
  std::uint16_t token_ = 0;
  std::array<std::uint16_t, 4> bbox_{};
  bool token_signal_ = false;
  bool finish_signal_ = false;
};

} // namespace GekkoAOT::HW::PE
