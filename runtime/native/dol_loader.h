#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include "native/address_space.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace GekkoAOT::Native {

struct DolLoadResult {
  std::uint32_t entry_point = 0;
  std::uint32_t bss_address = 0;
  std::uint32_t bss_size = 0;
  std::size_t loaded_bytes = 0;
};

namespace detail {
inline std::uint32_t DolBE32(std::span<const std::uint8_t> data, std::size_t offset) {
  if (offset > data.size() || data.size() - offset < 4)
    throw std::runtime_error("truncated DOL header");
  return (std::uint32_t(data[offset]) << 24) |
         (std::uint32_t(data[offset + 1]) << 16) |
         (std::uint32_t(data[offset + 2]) << 8) |
         std::uint32_t(data[offset + 3]);
}

inline bool AddOverflows(std::uint32_t a, std::uint32_t b) {
  return std::uint64_t(a) + std::uint64_t(b) > std::numeric_limits<std::uint32_t>::max();
}
}

inline DolLoadResult LoadDol(std::span<const std::uint8_t> image, AddressSpace& memory) {
  constexpr std::size_t HeaderSize = 0x100;
  if (image.size() < HeaderSize) throw std::runtime_error("DOL is smaller than its header");

  std::array<std::uint32_t, 18> offsets{};
  std::array<std::uint32_t, 18> addresses{};
  std::array<std::uint32_t, 18> sizes{};

  for (unsigned i = 0; i < 7; ++i) offsets[i] = detail::DolBE32(image, 0x00 + i * 4);
  for (unsigned i = 0; i < 11; ++i) offsets[7 + i] = detail::DolBE32(image, 0x1c + i * 4);
  for (unsigned i = 0; i < 7; ++i) addresses[i] = detail::DolBE32(image, 0x48 + i * 4);
  for (unsigned i = 0; i < 11; ++i) addresses[7 + i] = detail::DolBE32(image, 0x64 + i * 4);
  for (unsigned i = 0; i < 7; ++i) sizes[i] = detail::DolBE32(image, 0x90 + i * 4);
  for (unsigned i = 0; i < 11; ++i) sizes[7 + i] = detail::DolBE32(image, 0xac + i * 4);

  DolLoadResult result;
  result.bss_address = detail::DolBE32(image, 0xd8);
  result.bss_size = detail::DolBE32(image, 0xdc);
  result.entry_point = detail::DolBE32(image, 0xe0);

  if ((result.entry_point & 3u) != 0u)
    throw std::runtime_error("DOL entry point is not 4-byte aligned");

  // Zero BSS before copying sections so any legal overlap is resolved in favor
  // of file-backed data, as it would be after executable loading.
  if (result.bss_size != 0) {
    if (detail::AddOverflows(result.bss_address, result.bss_size))
      throw std::runtime_error("DOL BSS range overflows 32-bit address space");
    auto* bss = memory.Resolve(result.bss_address, result.bss_size);
    if (!bss) throw std::runtime_error("DOL BSS is outside native MEM1/MEM2");
    std::fill_n(bss, result.bss_size, std::uint8_t{0});
  }

  for (unsigned i = 0; i < offsets.size(); ++i) {
    const std::uint32_t size = sizes[i];
    if (size == 0) continue;
    const std::uint32_t file_offset = offsets[i];
    const std::uint32_t address = addresses[i];
    if (file_offset < HeaderSize || std::uint64_t(file_offset) + size > image.size())
      throw std::runtime_error("DOL section exceeds input image");
    if (detail::AddOverflows(address, size))
      throw std::runtime_error("DOL section address overflows 32-bit address space");
    auto* dst = memory.Resolve(address, size);
    if (!dst) throw std::runtime_error("DOL section is outside native MEM1/MEM2");
    std::copy_n(image.data() + file_offset, size, dst);
    result.loaded_bytes += size;
  }

  return result;
}

inline std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) throw std::runtime_error("cannot open file: " + path.string());
  const auto end = file.tellg();
  if (end < 0) throw std::runtime_error("cannot determine file size: " + path.string());
  std::vector<std::uint8_t> data(static_cast<std::size_t>(end));
  file.seekg(0);
  if (!data.empty() && !file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
    throw std::runtime_error("cannot read file: " + path.string());
  return data;
}

} // namespace GekkoAOT::Native
