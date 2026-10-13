#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <array>
#include <span>
#include "guest_ram.h"
#include "guest_address_window.h"
#include "core/native_hardware.h"

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
  // Retail software is told that MEM1 is 24 MiB, but the Gekko physical RAM
  // aperture spans 0x00000000..0x01ffffff. Dolphin deliberately keeps the
  // whole 32 MiB aperture host-backed while reporting 24 MiB through lowmem.
  // A few retail titles speculatively touch the normally-unpopulated tail;
  // making it addressable avoids turning those accesses into fake MMIO/faults.
  static constexpr std::uint32_t RetailMem1Size = 0x01800000u;
  static constexpr std::uint32_t CompatMem1BackingSize = 0x02000000u;
  static constexpr std::uint32_t RetailMem2Size = 0x04000000u;
  static constexpr std::uint32_t RetailFakeVmemSize = 0x02000000u;

  explicit AddressSpace(bool enable_mem2 = false)
      : AddressSpace(CompatMem1BackingSize, enable_mem2 ? RetailMem2Size : 0u) {}

  AddressSpace(std::size_t mem1_size, std::size_t mem2_size)
      : window_(mem1_size, mem2_size, RetailFakeVmemSize),
        mem1_(mem1_size, window_.Ready() ? window_.Base() : nullptr),
        mem2_(mem2_size, window_.Ready() && mem2_size ? window_.Base() + Mem2PhysicalBase : nullptr),
        fake_vmem_(RetailFakeVmemSize, window_.Ready() ? window_.Base() + FakeVmemBase : nullptr),
        flat_vmem_view_{fake_vmem_.data(), RetailFakeVmemSize, 0xffffffffu, nullptr, nullptr} {
    flat_vmem_view_.paged_pages = paged_pages_.data();
    flat_vmem_view_.paged_mru = paged_mru_.data();
  }
  AddressSpace(const AddressSpace&) = delete;
  AddressSpace& operator=(const AddressSpace&) = delete;

  // A fixed base is an address-layout contract, not permission to access MMIO
  // or bypass the game's page tables. Resolve() retains all existing checks.
  std::uint8_t* NativeAddressBase() const { return window_.Base(); }

  std::span<std::uint8_t> Mem1() { return mem1_; }
  std::span<const std::uint8_t> Mem1() const { return mem1_; }
  static constexpr std::uint32_t ReportedMem1Size() { return RetailMem1Size; }
  std::span<std::uint8_t> Mem2() { return mem2_; }
  std::span<const std::uint8_t> Mem2() const { return mem2_; }
  std::span<std::uint8_t> FakeVmem() { return paged_vmem_ ? std::span<std::uint8_t>{} : fake_vmem_; }
  // v274: the backing buffer regardless of paging, for write tracking.
  std::span<std::uint8_t> FakeVmemBacking() { return fake_vmem_; }
  std::span<const std::uint8_t> FakeVmem() const { return paged_vmem_ ? std::span<const std::uint8_t>{} : fake_vmem_; }
  const GekkoAOTFlatVmemView* FlatVmemView() const { return &flat_vmem_view_; }
  // v322: DTLB mirror for the paged aperture (layout in native_hardware.h).
  static constexpr std::uint32_t PagedViewPages = RetailFakeVmemSize >> 12u;
  std::span<std::uint32_t> PagedViewPagesTable() { return paged_pages_; }
  std::span<std::uint8_t> PagedViewMru() { return paged_mru_; }
  void SetPagedViewSr(std::uint32_t sr) { flat_vmem_view_.paged_sr = sr; }
  std::uint32_t PagedViewSr() const { return flat_vmem_view_.paged_sr; }
  void SetPagedVmem(bool enabled) {
    flat_vmem_view_.size = enabled ? 0u : RetailFakeVmemSize;
    paged_vmem_ = enabled;
  }
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

  // Number of host-contiguous bytes available from a guest address.  Native
  // SDK/libc replacements use this to operate on whole strings/ranges without
  // repeatedly re-entering the address decoder for every byte.
  std::size_t ContiguousAvailable(std::uint32_t address) const {
    if (address >= FakeVmemBase && address < FakeVmemBase + RetailFakeVmemSize) {
      if (paged_vmem_) return 0u;
      const auto offset = static_cast<std::size_t>(address - FakeVmemBase);
      return offset < fake_vmem_.size() ? fake_vmem_.size() - offset : 0u;
    }
    const auto physical = ToPhysical(address);
    if (physical < mem1_.size()) return mem1_.size() - physical;
    if (physical >= Mem2PhysicalBase) {
      const auto offset = static_cast<std::size_t>(physical - Mem2PhysicalBase);
      return offset < mem2_.size() ? mem2_.size() - offset : 0u;
    }
    return 0u;
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
    if (window_.Ready() && window_.Clear()) return;
    mem1_.Clear();
    mem2_.Clear();
    fake_vmem_.Clear();
  }

private:
  template<class Container>
  static auto ResolveRegion(Container& memory, std::uint32_t offset, std::size_t size)
      -> decltype(memory.data()) {
    if (offset > memory.size() || size > memory.size() - offset) return nullptr;
    return memory.data() + offset;
  }

  // Destroy views before the reservation they borrow.
  GuestAddressWindow window_;
  GuestRam mem1_;
  GuestRam mem2_;
  GuestRam fake_vmem_;
  GekkoAOTFlatVmemView flat_vmem_view_;
  std::array<std::uint32_t, RetailFakeVmemSize / 0x1000u> paged_pages_{};
  std::array<std::uint8_t, 64> paged_mru_{};
  bool paged_vmem_ = false;
};

} // namespace GekkoAOT::Native
