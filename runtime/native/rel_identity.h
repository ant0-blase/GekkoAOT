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
  if (read(header + 0x2cu) != std::uint64_t(count) * 8u ||
      table + std::uint64_t(count) * 8u > ram.size()) return false;
  for (std::uint32_t i = 0; i < count; ++i) {
    const auto& proof = proofs[i];
    const auto entry = table + std::uint64_t(i) * 8u;
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
