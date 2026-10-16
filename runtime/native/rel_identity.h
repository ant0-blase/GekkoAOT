#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/module_abi.h"
#include <cstddef>
#include <cstdint>
#include <span>

namespace GekkoAOT::Native {
inline bool MatchRelImportProofs(std::span<const std::uint8_t> ram,
                                std::uint32_t header,
                                const GekkoAOTRelImportProof* proofs,
                                std::uint32_t count) {
  if (!proofs || std::uint64_t(header) + 0x30u > ram.size()) return false;
  const auto read = [&](std::size_t p) {
    return (std::uint32_t(ram[p]) << 24) | (std::uint32_t(ram[p+1]) << 16) |
           (std::uint32_t(ram[p+2]) << 8) | ram[p+3];
  };
  // OSLink rebases the import table and stream pointers, leaving records intact.
  const auto physical = [&](std::uint32_t address) -> std::uint64_t {
    if (address >= 0x80000000u && address < 0xc0000000u) return address - 0x80000000u;
    if (address >= 0xc0000000u) return address - 0xc0000000u;
    return std::uint64_t(header) + address;
  };
  if (!count) return read(header + 0x2cu) == 0u;
  const auto table = physical(read(header + 0x28u));
  const std::uint64_t live_count = read(header + 0x2cu) / 8u;
  // OSLinkFixed (REL v3+, Wind Waker's DynamicModuleControl) applies the
  // module's self and main-program (id 0) imports, then trims them from the
  // import table: their streams lie past fixSize and the game frees that tail.
  // Accept exactly that trimmed form; every retained import must still match.
  const std::uint32_t self_id = read(header);
  std::uint32_t trimmed = 0u;
  for (std::uint32_t i = 0; i < count; ++i)
    if (proofs[i].module_id == 0u || proofs[i].module_id == self_id) ++trimmed;
  const bool fixed_link = live_count != count && read(header + 0x1cu) >= 3u &&
                          live_count == std::uint64_t(count - trimmed);
  if ((live_count != count && !fixed_link) || (read(header + 0x2cu) & 7u) ||
      table + live_count * 8u > ram.size()) return false;
  for (std::uint32_t i = 0, live = 0; i < count; ++i) {
    const auto& proof = proofs[i];
    if (fixed_link && (proof.module_id == 0u || proof.module_id == self_id)) continue;
    const auto entry = table + std::uint64_t(live++) * 8u;
    if (read(entry) != proof.module_id || !proof.stream_bytes || (proof.stream_bytes & 7u))
      return false;
    const auto stream = physical(read(entry + 4u));
    if (stream + proof.stream_bytes > ram.size()) return false;
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::uint32_t j = 0; j < proof.stream_bytes; ++j) {
      hash ^= ram[stream + j]; hash *= 1099511628211ull;
    }
    if (hash != proof.stream_hash) return false;
  }
  return true;
}
}
