// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include "word_proof.h"

namespace GekkoAOTSdk {
struct DvdFastOpenProof { std::int16_t count_offset, fst_offset; };
// A complete leaf proof, independent of SDK addresses and SDA relocation.
// Longer/debug variants with cancellation/assertion work are left to AOT.
inline std::optional<DvdFastOpenProof> ProveDvdFastOpen(const std::uint8_t* code,
                                                     std::size_t size) noexcept {
  if (!code || size < 116) return {};
  // Fingerprint of the 29-instruction SDK leaf (see word_proof.h); 'H' marks
  // its three SDA-relative loads, of which words 6 and 22 read the same cell.
  static constexpr WordProof proof{"FFHFFFHFFFFFFFFFFFFFFFHFFFFFF", 0xaf84813a7e676534ull};
  if (!MatchesWordProof(code, size, proof)) return {};
  const auto word=[&](unsigned i){return WordProofRead(code+4*i);};
  if ((word(6)&0xffff)!=(word(22)&0xffff)) return {};
  return DvdFastOpenProof{static_cast<std::int16_t>(word(2)),
                          static_cast<std::int16_t>(word(6))};
}
} // namespace GekkoAOTSdk
