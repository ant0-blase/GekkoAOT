// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace GekkoAOT::Native {
// One host virtual reservation for the complete 32-bit guest address space.
// Only RAM apertures are committed. Physical/cached/uncached RAM aliases share
// backing pages; MMIO and unmapped addresses stay inaccessible guard pages.
class GuestAddressWindow {
public:
  static constexpr std::uint64_t Size = std::uint64_t{1} << 32u;
  GuestAddressWindow(std::size_t mem1, std::size_t mem2, std::size_t vmem);
  ~GuestAddressWindow();
  GuestAddressWindow(const GuestAddressWindow&) = delete;
  GuestAddressWindow& operator=(const GuestAddressWindow&) = delete;
  bool Ready() const { return base_ != nullptr; }
  std::uint8_t* Base() const { return base_; }
  // Restore zero-filled RAM without changing any host address or alias.
  bool Clear();
private:
  std::uint8_t* base_ = nullptr;
  int backing_ = -1;
  std::size_t backing_size_ = 0;
};
} // namespace GekkoAOT::Native
