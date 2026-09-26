// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/lc/native_lc.h"

#include "native/address_space.h"

#include <algorithm>
#include <cstring>

namespace GekkoAOT::HW::LC {

void NativeLC::Reset() {
  bytes_.fill(0);
}

bool NativeLC::DecodeRange(std::uint32_t address, std::size_t size,
                           std::size_t* offset) {
  if (!offset || address < Base) return false;
  const std::uint32_t relative = address - Base;
  if (relative > Size || size > Size - relative) return false;
  *offset = relative;
  return true;
}

bool NativeLC::Read(std::uint32_t address, std::uint8_t size,
                    std::uint64_t* value) const {
  if (!value || (size != 1u && size != 2u && size != 4u && size != 8u)) return false;
  std::size_t offset = 0;
  if (!DecodeRange(address, size, &offset)) return false;

  std::uint64_t result = 0;
  for (std::uint8_t i = 0; i < size; ++i)
    result = (result << 8) | bytes_[offset + i];
  *value = result;
  return true;
}

bool NativeLC::Write(std::uint32_t address, std::uint64_t value,
                     std::uint8_t size) {
  if (size != 1u && size != 2u && size != 4u && size != 8u) return false;
  std::size_t offset = 0;
  if (!DecodeRange(address, size, &offset)) return false;

  for (std::uint8_t i = 0; i < size; ++i) {
    const std::uint8_t shift = static_cast<std::uint8_t>((size - 1u - i) * 8u);
    bytes_[offset + i] = static_cast<std::uint8_t>(value >> shift);
  }
  return true;
}

void* NativeLC::Pointer(std::uint32_t address, std::uint32_t size) {
  std::size_t offset = 0;
  if (!DecodeRange(address, size, &offset)) return nullptr;
  return bytes_.data() + offset;
}

const void* NativeLC::Pointer(std::uint32_t address, std::uint32_t size) const {
  std::size_t offset = 0;
  if (!DecodeRange(address, size, &offset)) return nullptr;
  return bytes_.data() + offset;
}

bool NativeLC::MemoryToLockedCache(GekkoAOT::Native::AddressSpace& memory,
                                   std::uint32_t cache_address,
                                   std::uint32_t memory_address,
                                   std::uint32_t blocks) {
  if (blocks == 0u) return true;
  const std::size_t byte_count = std::size_t(blocks) * BlockSize;
  const std::size_t cache_offset = cache_address & (Size - 1u);
  if (cache_offset > Size || byte_count > Size - cache_offset) return false;
  const auto* source = memory.Resolve(memory_address, byte_count);
  if (!source) return false;
  std::memcpy(bytes_.data() + cache_offset, source, byte_count);
  return true;
}

bool NativeLC::LockedCacheToMemory(GekkoAOT::Native::AddressSpace& memory,
                                   std::uint32_t memory_address,
                                   std::uint32_t cache_address,
                                   std::uint32_t blocks) const {
  if (blocks == 0u) return true;
  const std::size_t byte_count = std::size_t(blocks) * BlockSize;
  const std::size_t cache_offset = cache_address & (Size - 1u);
  if (cache_offset > Size || byte_count > Size - cache_offset) return false;
  auto* destination = memory.Resolve(memory_address, byte_count);
  if (!destination) return false;
  std::memcpy(destination, bytes_.data() + cache_offset, byte_count);
  return true;
}

} // namespace GekkoAOT::HW::LC
