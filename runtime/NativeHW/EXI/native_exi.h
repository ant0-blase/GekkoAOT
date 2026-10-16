#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>
#include "NativeHW/EXI/native_card.h"

namespace GekkoAOT::NativeHW::EXI {

// Host-backed IPL RTC/SRAM service for the supported EXI transaction shape.
// Other devices and commands return unhandled to the existing register path.
class HostService final {
public:
  HostService();
  ~HostService();
  void Enable();
  bool Select(std::uint32_t channel, std::uint8_t chip_select);
  bool Present(std::uint32_t channel, std::uint8_t chip_select) const;
  bool Immediate(std::uint32_t channel, std::uint8_t chip_select,
                 std::uint8_t direction, std::uint8_t length, std::uint32_t* data);
  bool Dma(std::uint32_t channel, std::uint8_t chip_select,
           std::uint8_t direction, std::uint8_t* memory, std::uint32_t length);

private:
  enum class Region : std::uint8_t { None, Sram, Rom };
  void Save() const;
  void LoadRom();
  bool RomReadable(std::uint32_t address);
  std::array<std::uint8_t, 0x44> sram_{};
  std::vector<std::uint8_t> rom_;
  // No user IPL: only the font slots are served, from GekkoAOT's own font.
  bool rom_replacement_ = false;
  bool rom_replacement_failed_ = false;
  std::filesystem::path save_path_;
  std::uint32_t command_ = 0;
  std::uint32_t cursor_ = 0;
  std::uint8_t selected_ = 0;
  std::uint8_t command_bytes_ = 0;
  Region region_ = Region::None;
  bool command_valid_ = false;
  bool sram_dirty_ = false;
  std::array<HostCard, 2> cards_{};
};
} // namespace GekkoAOT::NativeHW::EXI
