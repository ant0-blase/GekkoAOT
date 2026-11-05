#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdint>

namespace GekkoAOT::HW::MI {

// Minimal standalone GameCube Memory Interface register model.
//
// This deliberately models the hardware-visible register bank rather than
// linking Dolphin's MemoryInterfaceManager.  Memory protection enforcement and
// MI performance-counter semantics can be added later without changing the
// HostRuntime MMIO contract.
class NativeMI final {
public:
  static constexpr std::uint32_t BasePhysical = 0x0c004000u;
  static constexpr std::uint32_t WindowSize = 0x1000u;

  NativeMI() { Reset(); }

  void Reset();
  bool Handles(std::uint32_t address) const;
  bool Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value) const;
  bool Write(std::uint32_t address, std::uint64_t value, std::uint8_t size);

  std::uint16_t IrqMask() const { return irq_mask_; }
  std::uint16_t IrqFlag() const { return irq_flag_; }

private:
  static constexpr std::uint32_t ToPhysical(std::uint32_t address) {
    return address & 0x3fffffffu;
  }

  bool Read16(std::uint32_t offset, std::uint16_t* value) const;
  bool Write16(std::uint32_t offset, std::uint16_t value);

  std::array<std::uint16_t, 4> region_first_{};
  std::array<std::uint16_t, 4> region_last_{};
  std::uint16_t prot_type_ = 0;
  std::uint16_t irq_mask_ = 0;
  std::uint16_t irq_flag_ = 0;
  std::uint16_t unknown1_ = 0;
  // The hardware register names are historically confusing: at 0x22 lives
  // the upper half of the host-side 32-bit storage and at 0x24 the lower half.
  std::uint16_t prot_addr_lo_register_ = 0;
  std::uint16_t prot_addr_hi_register_ = 0;
  std::array<std::uint32_t, 10> timers_{};
  std::uint16_t unknown2_ = 0;
};

} // namespace GekkoAOT::HW::MI
