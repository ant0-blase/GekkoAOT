// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include "word_proof.h"
namespace GekkoAOTSdk {
// Whole SDK leaves, including the helper reached by older wrappers. Debug
// variants with assertions or additional GX state changes remain guest AOT.
// Fingerprints (see word_proof.h): the 3x3 normal-matrix leaf, its packed
// variant, the older wrapper (word 11 is its call, checked separately) and
// the helper that wrapper calls.
inline constexpr WordProof kGxNormalMatrixProof{"FFFFFFFFFFFFFFFFFFFF", 0x04ab64fde6498826ull};
inline constexpr WordProof kGxNormalMatrixPackedProof{"FFFFFFFFFFFFFFFFFF", 0x631cdbee67cc4d38ull};
inline constexpr WordProof kGxNormalMatrixWrapperProof{"FFFFFFFFFFF-FFFF", 0x2f030a46f67aa912ull};
inline constexpr WordProof kGxNormalMatrixHelperProof{"FFFFFFFFFFFFF", 0xd9b42c0be9d90659ull};
template<class Resolve>
inline bool ProveGxNormalMatrix(const std::uint8_t* code,std::size_t size,
                                std::uint32_t pc,bool packed,Resolve resolve) {
  if (packed) return MatchesWordProof(code,size,kGxNormalMatrixPackedProof);
  if (MatchesWordProof(code,size,kGxNormalMatrixProof)) return true;
  if (!code || size<64u) return false;
  if (!MatchesWordProof(code,size,kGxNormalMatrixWrapperProof)) return false;
  const auto branch=WordProofRead(code+44);
  if ((branch&0xfc000003u)!=0x48000001u) return false;
  const auto displacement=static_cast<std::int32_t>((branch&0x03fffffcu)<<6)>>6;
  return MatchesWordProof(resolve(pc+44u+static_cast<std::uint32_t>(displacement),52u),52u,
                          kGxNormalMatrixHelperProof);
}
} // namespace GekkoAOTSdk
