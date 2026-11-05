// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Exact structural proofs without stored instruction words.
//
// A proof template is one mask letter per instruction:
//   'F' the whole word takes part,
//   'H' the high halfword (opcode and registers; the immediate is free),
//   'B' the branch opcode and AA/LK bits (the target is free),
//   '-' the word is not part of the fingerprint (checked separately, if at all).
// The fingerprint is StructuralSequenceHash of the masked words, so a proof
// stores its template and one 64-bit hash. tools/word_proof_fingerprint.py
// computes it from instruction words taken from the user's own executable.
#include "structural_signature.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace GekkoAOTSdk {

struct WordProof {
  std::string_view masks;
  std::uint64_t hash;
};

inline constexpr std::uint32_t WordProofMask(char letter) {
  switch (letter) {
  case 'F': return 0xffffffffu;
  case 'H': return 0xffff0000u;
  case 'B': return 0xfc000003u;
  default: return 0u;
  }
}

inline std::uint32_t WordProofRead(const std::uint8_t* p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
         (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

// Same construction as StructuralSequenceHash over the masked words.
inline std::uint64_t WordProofHash(const std::uint8_t* code, std::string_view masks) {
  std::uint64_t h = StructuralMix64(kStructuralSequenceSalt ^ static_cast<std::uint64_t>(masks.size()));
  for (std::size_t i = 0; i < masks.size(); ++i) {
    const std::uint32_t word = WordProofRead(code + 4 * i) & WordProofMask(masks[i]);
    h = StructuralMix64(h ^ (word + 0x9e3779b97f4a7c15ull));
  }
  return h;
}

// True when `code` holds at least the template's words and they match it.
inline bool MatchesWordProof(const std::uint8_t* code, std::size_t size, const WordProof& proof) {
  return code && size >= proof.masks.size() * 4u && WordProofHash(code, proof.masks) == proof.hash;
}

} // namespace GekkoAOTSdk
