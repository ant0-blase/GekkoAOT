// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Native replacement for the paired-single 8x8 IDCT of EA's GameCube
// MPEG-2 movie decoder (RCMP/MPC codec, MSSG-derived; Medal of Honor:
// Frontline). GC_IDCT runs PS_IDCT_row twice per call on 4 row pairs and its
// column twin on 4 column pairs; under AOT those two routines were ~40% of
// the main thread while a movie plays.
//
// Bit-exact with DolRecomp's paired-single model (cpu.c): every paired value
// is f32-exact, so force_25_bit(c) is the identity and ps_mul / ps_madd /
// ps_nmadd / ps_add / ps_sub are IEEE single mul / fused multiply-add
// (negated after rounding) / add / sub under round-to-nearest. GQR7 loads are
// s16 * 2^-4, GQR7 stores truncate toward zero and saturate to s16, GQR0 is
// f32 with stored denormals flushed. Recognition compares fingerprints of the
// three routines (only the constant-table address, GQR7 and the two call
// offsets may differ) and is header-only so gekkoaotctl, the runtime and tests
// share it.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "../sdk/word_proof.h"

namespace GekkoAOT::Native::EaMpeg {

// Fingerprints of the three routines (see sdk/word_proof.h); no instruction
// word is stored. GC_IDCT leaves out the constant table and GQR7 halves ('H')
// and keeps only the opcode of its two calls ('B').
inline constexpr GekkoAOTSdk::WordProof kEaPsIdctRowProof{
    "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
    0x5bb53ddf8416df29ull};
inline constexpr GekkoAOTSdk::WordProof kEaPsIdctColProof{
    "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF",
    0x01b33900d3930aa1ull};
inline constexpr GekkoAOTSdk::WordProof kEaGcIdctProof{
    "FFFFFFFFFFFFFFFFFFFHHHHFFFFFFFFFFFFFBFFFFFFFFBFFFFFFFFFFFFFFFFFFFFFFFF",
    0x74f03686d0d1b7d9ull};
// StructuralSequenceHash of GC_IDCT's first word: cheap scan anchor.
inline constexpr std::uint64_t kEaGcIdctFirstWordHash = 0x894aa2528550ff22ull;

// GC_IDCT words that hold per-build values: the constant table address,
// the GQR7 value (MoH Frontline 0x04070007: s16 loads scaled 2^-4; 007 Agent
// Under Fire's Movie.elf 0x00070007: unscaled) and the two call offsets.
inline constexpr std::size_t kTableHi = 19, kTableLo = 20, kGqrHi = 21, kGqrLo = 22,
                             kCallRow = 36, kCallCol = 45;
// GQR7 written by MoH Frontline's GC_IDCT (load s16 scale 4, store s16 scale 0).
inline constexpr std::uint32_t kIdctGqr7 = 0x04070007u;
// Host-call token of the game-library HLE (runtime kGameLibraryBase | id).
inline constexpr std::uint32_t kGameLibraryBase = 0xfffb0000u;
inline constexpr std::uint32_t kTokenGcIdct = kGameLibraryBase | 1u;

struct IdctPlan {
  std::uint32_t table = 0, row = 0, col = 0, gqr7 = 0;
};

inline std::uint32_t BranchTarget(std::uint32_t word, std::uint32_t pc) {
  std::uint32_t li = word & 0x03fffffcu;
  if (li & 0x02000000u) li |= 0xfc000000u;
  return pc + li;
}

// Reads `count` guest words starting at `address` as big-endian bytes.
template <class Read>
bool ReadGuestWords(std::uint32_t address, std::size_t count, Read&& read, std::uint8_t* out) {
  for (std::size_t i = 0; i < count; ++i) {
    std::uint32_t word = 0;
    if (!read(address + std::uint32_t(i * 4u), &word)) return false;
    out[i * 4] = std::uint8_t(word >> 24); out[i * 4 + 1] = std::uint8_t(word >> 16);
    out[i * 4 + 2] = std::uint8_t(word >> 8); out[i * 4 + 3] = std::uint8_t(word);
  }
  return true;
}

// `read(address)` returns the big-endian guest word at `address` (or false).
template <class Read>
bool MatchGcIdct(std::uint32_t pc, Read&& read, IdctPlan* plan) {
  std::array<std::uint8_t, 4 * 76> code{};
  const auto words = kEaGcIdctProof.masks.size();
  if (!ReadGuestWords(pc, words, read, code.data()) ||
      !GekkoAOTSdk::MatchesWordProof(code.data(), words * 4u, kEaGcIdctProof)) return false;
  const auto word = [&](std::size_t i) { return GekkoAOTSdk::WordProofRead(code.data() + 4 * i); };
  plan->table = (word(kTableHi) << 16) + std::uint32_t(std::int32_t(std::int16_t(word(kTableLo) & 0xffffu)));
  plan->gqr7 = (word(kGqrHi) << 16) + std::uint32_t(std::int32_t(std::int16_t(word(kGqrLo) & 0xffffu)));
  plan->row = BranchTarget(word(kCallRow), pc + std::uint32_t(kCallRow * 4u));
  plan->col = BranchTarget(word(kCallCol), pc + std::uint32_t(kCallCol * 4u));
  const auto same = [&](std::uint32_t base, const GekkoAOTSdk::WordProof& proof) {
    const auto n = proof.masks.size();
    return ReadGuestWords(base, n, read, code.data()) &&
           GekkoAOTSdk::MatchesWordProof(code.data(), n * 4u, proof);
  };
  // Both quantized types must be s16 (the routines' only integer format).
  if (((plan->gqr7 >> 16) & 7u) != 7u || (plan->gqr7 & 7u) != 7u) return false;
  return same(plan->row, kEaPsIdctRowProof) && same(plan->col, kEaPsIdctColProof);
}

inline int GqrScale(std::uint32_t six) { return int(six & 0x20u ? six | ~0x3fu : six); }

// Every rounding below is one guest instruction: the compiler must not fuse a
// product into a following add/sub (e.g. after folding fma(a,c,0) to a*c).
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

// Shared body of PS_IDCT_row and its column twin for one paired lane:
// x[0..7] in, y[0..7] out, k = f22..f29.
inline void IdctLane(const float* x, const float* k, float* y) {
  const float k22 = k[0], k23 = k[1], k24 = k[2], k25 = k[3];
  const float k26 = k[4], k27 = k[5], k28 = k[6], k29 = k[7];
  const auto madd = [](float a, float c, float b) { return std::fma(a, c, b); };
  const auto nmadd = [](float a, float c, float b) { return -std::fma(a, c, b); };
  float f18 = k22 * x[0];
  float f10 = madd(k23, x[2], f18);
  float f11 = madd(k24, x[2], f18);
  float f12 = madd(k23, x[6], f18);
  float f13 = madd(k22, x[4], f18);
  f10 = madd(k22, x[4], f10);
  f10 = madd(k24, x[6], f10);
  f18 = k23 * x[6];
  float f19 = k22 * x[4];
  float f20 = k24 * x[6];
  f18 = nmadd(k22, x[4], f18);
  f19 = nmadd(k24, x[2], f19);
  f20 = nmadd(k23, x[2], f20);
  f11 = f11 + f18;
  f12 = f12 + f19;
  f13 = f13 + f20;
  f18 = k25 * x[1];
  f19 = k26 * x[1];
  f20 = k27 * x[1];
  float f21 = k28 * x[1];
  f18 = madd(k26, x[3], f18);
  f20 = madd(k28, x[5], f20);
  f21 = madd(k26, x[5], f21);
  f18 = madd(k27, x[5], f18);
  f20 = madd(k26, x[7], f20);
  f18 = madd(k28, x[7], f18);
  float f14 = k27 * x[7];
  float f15 = nmadd(k25, x[3], k29);
  float f16 = k25 * x[7];
  f14 = madd(k25, x[5], f14);
  f20 = f20 + f15;
  f16 = nmadd(k27, x[3], f16);
  f14 = nmadd(k28, x[3], f14);
  f21 = f21 + f16;
  f19 = f19 + f14;
  y[7] = f10 - f18;
  y[6] = f11 - f19;
  y[5] = f12 - f20;
  y[4] = f13 - f21;
  y[0] = f10 + f18;
  y[1] = f11 + f19;
  y[2] = f12 + f20;
  y[3] = f13 + f21;
}

inline float FlushStoredDenormal(float value) {
  std::uint32_t bits;
  std::memcpy(&bits, &value, 4);
  if ((bits & 0x7f800000u) == 0u && (bits & 0x007fffffu) != 0u) return 0.f;
  return value;
}

inline std::int16_t QuantizeS16(float value, int scale) {
  if (std::isnan(value)) return 32767;
  const double scaled = std::trunc(std::ldexp(double(value), scale));
  if (scaled <= -32768.0) return -32768;
  if (scaled >= 32767.0) return 32767;
  return std::int16_t(scaled);
}

// block: 64 coefficients, row-major, already in host byte order. gqr7 is the
// value GC_IDCT writes (psq_l/psq_st scales of its s16 accesses).
inline void GcIdct(std::int16_t* block, const float* k, std::uint32_t gqr7 = kIdctGqr7) {
  const int load_scale = GqrScale((gqr7 >> 24) & 0x3fu), store_scale = GqrScale((gqr7 >> 8) & 0x3fu);
  float temp[64], x[8], y[8];
  for (unsigned r = 0; r < 8; ++r) {
    for (unsigned c = 0; c < 8; ++c) x[c] = float(std::ldexp(double(block[r * 8 + c]), -load_scale));
    IdctLane(x, k, y);
    for (unsigned c = 0; c < 8; ++c) temp[r * 8 + c] = FlushStoredDenormal(y[c]);
  }
  for (unsigned c = 0; c < 8; ++c) {
    for (unsigned r = 0; r < 8; ++r) x[r] = temp[r * 8 + c];
    IdctLane(x, k, y);
    for (unsigned r = 0; r < 8; ++r) block[r * 8 + c] = QuantizeS16(y[r], store_scale);
  }
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif

// Scan helper for build-time manifests: entry addresses of every GC_IDCT
// copy inside one executable section.
template <class Read>
void FindGcIdct(std::uint32_t base, std::size_t words, Read&& read,
                std::uint32_t* found, std::size_t capacity, std::size_t* count) {
  *count = 0;
  const auto length = kEaGcIdctProof.masks.size();
  for (std::size_t i = 0; i + length <= words && *count < capacity; ++i) {
    std::uint32_t first = 0;
    if (!read(base + std::uint32_t(i * 4u), &first)) continue;
    const std::uint32_t one[1] = {first};
    if (GekkoAOTSdk::StructuralSequenceHash(one) != kEaGcIdctFirstWordHash) continue;
    IdctPlan plan;
    if (MatchGcIdct(base + std::uint32_t(i * 4u), read, &plan)) found[(*count)++] = base + std::uint32_t(i * 4u);
  }
}

} // namespace GekkoAOT::Native::EaMpeg
