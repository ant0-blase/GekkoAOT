#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace GekkoAOT::Native {

class AddressSpace {
public:
  static constexpr std::uint32_t Mem1PhysicalBase = 0x00000000u;
  static constexpr std::uint32_t Mem2PhysicalBase = 0x10000000u;
  // Retail GameCube software using the SDK VM library addresses a 32 MiB
  // virtual window immediately below 0x80000000.  On hardware this library
  // pages data through ARAM/MMU state; a standalone AOT runtime without a full
  // MMU needs a host-backed compatibility window instead of treating it as
  // device MMIO.
  static constexpr std::uint32_t FakeVmemBase = 0x7e000000u;
  static constexpr std::uint32_t RetailMem1Size = 0x01800000u;
  static constexpr std::uint32_t RetailMem2Size = 0x04000000u;
  static constexpr std::uint32_t RetailFakeVmemSize = 0x02000000u;

  explicit AddressSpace(bool enable_mem2 = false)
      : mem1_(RetailMem1Size), mem2_(enable_mem2 ? RetailMem2Size : 0u),
        fake_vmem_(RetailFakeVmemSize) {}

  AddressSpace(std::size_t mem1_size, std::size_t mem2_size)
      : mem1_(mem1_size), mem2_(mem2_size), fake_vmem_(RetailFakeVmemSize) {}

  std::span<std::uint8_t> Mem1() { return mem1_; }
  std::span<const std::uint8_t> Mem1() const { return mem1_; }
  std::span<std::uint8_t> Mem2() { return mem2_; }
  std::span<const std::uint8_t> Mem2() const { return mem2_; }
  std::span<std::uint8_t> FakeVmem() { return paged_vmem_ ? std::span<std::uint8_t>{} : fake_vmem_; }
  std::span<const std::uint8_t> FakeVmem() const { return paged_vmem_ ? std::span<const std::uint8_t>{} : fake_vmem_; }
  void SetPagedVmem(bool enabled) { paged_vmem_ = enabled; }
  bool PagedVmem() const { return paged_vmem_; }
  static constexpr bool IsVmem(std::uint32_t address) {
    return address >= FakeVmemBase && address < FakeVmemBase + RetailFakeVmemSize;
  }

  static constexpr std::uint32_t ToPhysical(std::uint32_t address) {
    return (address & 0x80000000u) != 0u ? address & 0x3fffffffu : address;
  }

  std::uint8_t* Resolve(std::uint32_t address, std::size_t size) {
    // FakeVMEM uses an otherwise non-MMIO physical aperture.  Check it before
    // the generic MEM2 probe so 0x7e000000..0x7fffffff is host RAM and never
    // reaches the external hardware/MMIO path.
    if (address >= FakeVmemBase &&
        address < FakeVmemBase + RetailFakeVmemSize)
      return paged_vmem_ ? nullptr : ResolveRegion(fake_vmem_, address - FakeVmemBase, size);

    const auto physical = ToPhysical(address);
    if (physical < mem1_.size()) return ResolveRegion(mem1_, physical, size);
    if (physical >= Mem2PhysicalBase)
      return ResolveRegion(mem2_, physical - Mem2PhysicalBase, size);
    return nullptr;
  }

  const std::uint8_t* Resolve(std::uint32_t address, std::size_t size) const {
    if (address >= FakeVmemBase &&
        address < FakeVmemBase + RetailFakeVmemSize)
      return paged_vmem_ ? nullptr : ResolveRegion(fake_vmem_, address - FakeVmemBase, size);

    const auto physical = ToPhysical(address);
    if (physical < mem1_.size()) return ResolveRegion(mem1_, physical, size);
    if (physical >= Mem2PhysicalBase)
      return ResolveRegion(mem2_, physical - Mem2PhysicalBase, size);
    return nullptr;
  }

  bool Read8(std::uint32_t address, std::uint8_t* out) const {
    if (!out) return false;
    const auto* p = Resolve(address, 1); if (!p) return false;
    *out = p[0]; return true;
  }
  bool Read16(std::uint32_t address, std::uint16_t* out) const {
    if (!out) return false;
    const auto* p = Resolve(address, 2); if (!p) return false;
    *out = (std::uint16_t(p[0]) << 8) | std::uint16_t(p[1]); return true;
  }
  bool Read32(std::uint32_t address, std::uint32_t* out) const {
    if (!out) return false;
    const auto* p = Resolve(address, 4); if (!p) return false;
    *out = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]); return true;
  }
  bool Read64(std::uint32_t address, std::uint64_t* out) const {
    if (!out) return false;
    const auto* p = Resolve(address, 8); if (!p) return false;
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value = (value << 8) | p[i];
    *out = value; return true;
  }
  bool Write8(std::uint32_t address, std::uint8_t value) {
    auto* p = Resolve(address, 1); if (!p) return false; p[0] = value; return true;
  }
  bool Write16(std::uint32_t address, std::uint16_t value) {
    auto* p = Resolve(address, 2); if (!p) return false;
    p[0] = std::uint8_t(value >> 8); p[1] = std::uint8_t(value); return true;
  }
  bool Write32(std::uint32_t address, std::uint32_t value) {
    auto* p = Resolve(address, 4); if (!p) return false;
    p[0] = std::uint8_t(value >> 24); p[1] = std::uint8_t(value >> 16);
    p[2] = std::uint8_t(value >> 8); p[3] = std::uint8_t(value); return true;
  }
  bool Write64(std::uint32_t address, std::uint64_t value) {
    auto* p = Resolve(address, 8); if (!p) return false;
    for (int i = 7; i >= 0; --i) { p[i] = std::uint8_t(value); value >>= 8; }
    return true;
  }

  void Clear() {
    std::fill(mem1_.begin(), mem1_.end(), 0);
    std::fill(mem2_.begin(), mem2_.end(), 0);
    std::fill(fake_vmem_.begin(), fake_vmem_.end(), 0);
  }

private:
  template<class Container>
  static auto ResolveRegion(Container& memory, std::uint32_t offset, std::size_t size)
      -> decltype(memory.data()) {
    if (offset > memory.size() || size > memory.size() - offset) return nullptr;
    return memory.data() + offset;
  }

  std::vector<std::uint8_t> mem1_;
  std::vector<std::uint8_t> mem2_;
  std::vector<std::uint8_t> fake_vmem_;
  bool paged_vmem_ = false;
};

} // namespace GekkoAOT::Native
