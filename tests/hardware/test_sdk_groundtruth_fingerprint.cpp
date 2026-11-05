// SPDX-License-Identifier: GPL-3.0-or-later
// The SDK reference table stores fingerprints, not instruction words. Checks
// that the runtime hashes match tools/sdk_groundtruth_fingerprint.py (fixed
// vectors), that the table is self-consistent, and that every reference body
// matches itself exactly and with full similarity.
#include "sdk/sdk_groundtruth_v1.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

using namespace GekkoAOTSdk;

int main() {
  // Vectors computed by tools/sdk_groundtruth_fingerprint.py.
  static_assert(StructuralMix64(0) == 0);
  static_assert(StructuralMix64(1) == 0x5692161d100b05e5ull);
  const std::vector<std::uint32_t> words{0x12345678u, 0x4e800020u, 0x38600005u};
  CHECK(StructuralSequenceHash(words) == 0x6d6246778bb44ba1ull);
  CHECK(StructuralPrefixHash(words[0], words[1]) == 0x1f1e935833f905cdull);
  CHECK(StructuralPrefixHash(words[0], words[1], words[2]) == StructuralSequenceHash(words));
  const std::uint16_t strict[] = {106u, 116u, 176u};
  const std::uint16_t relaxed[] = {34u, 77u, 199u};
  for (std::size_t i = 0; i < words.size(); ++i) {
    CHECK(StructuralWordHash(words[i], kStructuralStrictSalt) == strict[i]);
    CHECK(StructuralWordHash(GroundTruthRelaxedWord(words[i]), kStructuralRelaxedSalt) == relaxed[i]);
  }

  // A candidate built from code gets the same fingerprints.
  std::vector<std::uint8_t> code;
  for (const auto w : words)
    for (int shift = 24; shift >= 0; shift -= 8) code.push_back(static_cast<std::uint8_t>(w >> shift));
  const auto shape = BuildStructuralShape(code.data(), static_cast<std::uint32_t>(code.size()));
  CHECK(shape.skeleton_hash == StructuralSequenceHash(shape.skeleton));
  CHECK(shape.strict_hash.size() == words.size() && shape.relaxed_hash.size() == words.size());
  CHECK(GroundTruthExact(shape, shape));

  // Per-instruction hashes stay at most 8 bits wide (non-reversible).
  static_assert(kStructuralWordHashBits <= 8);
  for (std::size_t i = 0; i < kGroundTruthStrictHash.size(); ++i)
    CHECK(kGroundTruthStrictHash[i] <= 0xffu && kGroundTruthRelaxedHash[i] <= 0xffu);

  // Table consistency and self-correspondence of every reference body.
  const auto& shapes = GroundTruthShapes();
  CHECK(shapes.size() == kGroundTruthExemplars.size());
  for (std::size_t i = 0; i < shapes.size(); ++i) {
    const auto& e = kGroundTruthExemplars[i];
    const auto& s = shapes[i];
    CHECK(e.hash_offset + e.instruction_count <= kGroundTruthStrictHash.size());
    CHECK(s.skeleton.empty());
    CHECK(s.strict_hash.size() == e.instruction_count && s.relaxed_hash.size() == e.instruction_count);
    unsigned ops = 0;
    for (const auto n : s.primary_ops) ops += n;
    CHECK(ops == e.instruction_count);
    CHECK(e.call_offset + e.call_count <= kGroundTruthCallNames.size());
    CHECK(GroundTruthExact(s, s));
    if (e.instruction_count)
      CHECK(GroundTruthSimilarity(s, s) > 0.999999);
  }
  std::printf("%zu reference bodies, %zu fingerprinted instructions\n", shapes.size(),
              kGroundTruthStrictHash.size());
  return 0;
}
