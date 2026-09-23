#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstddef>
#include <cstdint>

namespace GekkoAOT::Native { class AddressSpace; }

namespace GekkoAOT::HW::LC {

// Gekko locked-L1 cache aperture. Retail SDK code maps the locked cache at
// 0xE0000000 and uses 32-byte cache blocks. Keep the full 256 KiB addressable
// aperture used by the hardware DMA address field; the SDK's LCEnable bootstrap
// initially allocates/zeros the first 512 blocks (16 KiB).
class NativeLC final {
public:
  static constexpr std::uint32_t Base = 0xe0000000u;
  static constexpr std::uint32_t Size = 0x00040000u;
  static constexpr std::uint32_t BlockSize = 32u;

  void Reset();

  bool Read(std::uint32_t address, std::uint8_t size, std::uint64_t* value) const;
  bool Write(std::uint32_t address, std::uint64_t value, std::uint8_t size);
  void* Pointer(std::uint32_t address, std::uint32_t size);
  const void* Pointer(std::uint32_t address, std::uint32_t size) const;

  bool MemoryToLockedCache(GekkoAOT::Native::AddressSpace& memory,
                           std::uint32_t cache_address,
                           std::uint32_t memory_address,
                           std::uint32_t blocks);
  bool LockedCacheToMemory(GekkoAOT::Native::AddressSpace& memory,
                           std::uint32_t memory_address,
                           std::uint32_t cache_address,
                           std::uint32_t blocks) const;

private:
  static bool DecodeRange(std::uint32_t address, std::size_t size,
                          std::size_t* offset);

  std::array<std::uint8_t, Size> bytes_{};
};

} // namespace GekkoAOT::HW::LC
