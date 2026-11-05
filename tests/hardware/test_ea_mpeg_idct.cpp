// SPDX-License-Identifier: GPL-3.0-or-later
// The native EA GC_IDCT must equal the guest routine bit for bit. This
// test interprets the routine's actual instruction words (the signatures in
// ea_mpeg_hle.h) under DolRecomp's paired-single reference model (cpu.c:
// force_25_bit, fma with single tie correction, f32 result rounding, psq
// quantization) and compares every output coefficient with GcIdct().
#include "../../runtime/native/ea_mpeg_hle.h"
#include "sdk_fixture_words.h"
#include <map>
#include <string>
#include <vector>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

// EA's routines are not stored in the repository (see sdk_fixture_words.h).
static const std::vector<std::uint32_t>& EaFixture(const char* name) {
  static std::map<std::string, std::vector<std::uint32_t>> cache;
  auto it = cache.find(name);
  if (it == cache.end()) it = cache.emplace(name, RequireSdkFixture(name)).first;
  return it->second;
}

namespace {
namespace Ea = GekkoAOT::Native::EaMpeg;

std::uint64_t Bits(double v) { return std::bit_cast<std::uint64_t>(v); }
double Value(std::uint64_t b) { return std::bit_cast<double>(b); }

double Force25(double value) {  // cpu.c force_25_bit (normal values)
  const std::uint64_t bits = Bits(value);
  return Value((bits & 0xFFFFFFFFF8000000ull) + (bits & 0x0000000008000000ull));
}
double TieCorrected(double a, double c, double addend, double result) {  // cpu.c
  const std::uint64_t bits = Bits(result);
  if ((bits & 0x000000001FFFFFFFull) != 0x0000000010000000ull) return result;
  const double a_prime = addend - result, b_prime = result + a_prime;
  const double error = std::fma(a, c, a_prime) + (addend - b_prime);
  if (error == 0.0) return result;
  return Value((error > 0.0) == (result > 0.0) ? bits + 1 : bits - 1);
}
double Single(double v) { return double(float(v)); }
double Fma(double a, double c, double b) {
  const double rc = Force25(c);
  return Single(TieCorrected(a, rc, b, std::fma(a, rc, b)));
}

struct Cpu {
  double ps0[32]{}, ps1[32]{};
  std::uint32_t r[32]{};
  std::uint32_t gqr[8]{};
  std::vector<std::uint8_t> mem = std::vector<std::uint8_t>(0x1000);
};

std::uint16_t Load16(const Cpu& cpu, std::uint32_t a) { return std::uint16_t(cpu.mem[a] << 8 | cpu.mem[a + 1]); }
std::uint32_t Load32(const Cpu& cpu, std::uint32_t a) {
  return std::uint32_t(cpu.mem[a]) << 24 | std::uint32_t(cpu.mem[a + 1]) << 16 | std::uint32_t(cpu.mem[a + 2]) << 8 | cpu.mem[a + 3];
}
void Store16(Cpu& cpu, std::uint32_t a, std::uint16_t v) { cpu.mem[a] = std::uint8_t(v >> 8); cpu.mem[a + 1] = std::uint8_t(v); }
void Store32(Cpu& cpu, std::uint32_t a, std::uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) cpu.mem[a + i] = std::uint8_t(v >> (24 - 8 * i));
}

double PsqLoadOne(const Cpu& cpu, std::uint32_t ea, unsigned type, int scale) {
  if (type == 0) return double(std::bit_cast<float>(Load32(cpu, ea)));
  if (type == 7) return double(float(std::ldexp(double(std::int16_t(Load16(cpu, ea))), -scale)));
  std::abort();
}
void PsqStoreOne(Cpu& cpu, std::uint32_t ea, unsigned type, int scale, double value) {
  if (type == 0) {
    const float single = float(value);
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(single);
    Store32(cpu, ea, ((bits & 0x7F800000u) == 0 && (bits & 0x007FFFFFu)) ? 0u : bits);
    return;
  }
  if (type == 7) {
    double scaled = std::trunc(std::ldexp(value, scale));
    std::int16_t q = scaled <= -32768.0 ? -32768 : scaled >= 32767.0 ? 32767 : std::int16_t(scaled);
    Store16(cpu, ea, std::uint16_t(q));
    return;
  }
  std::abort();
}
int Scale(std::uint32_t six) { return int(six & 0x20u ? six | ~0x3Fu : six); }

void Run(Cpu& cpu, const std::uint32_t* words, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint32_t w = words[i], op = w >> 26;
    const unsigned d = (w >> 21) & 31, a = (w >> 16) & 31, b = (w >> 11) & 31, c = (w >> 6) & 31;
    if (w == 0x4e800020u) return;
    if (op == 56 || op == 60) {
      const std::uint32_t q = cpu.gqr[(w >> 12) & 7];
      const bool single = (w >> 15) & 1;
      if (single) std::abort();
      std::int32_t disp = std::int32_t(w & 0xfff);
      if (disp & 0x800) disp -= 0x1000;
      const std::uint32_t ea = cpu.r[a] + std::uint32_t(disp);
      if (op == 56) {
        const unsigned type = (q >> 16) & 7; const int scale = Scale((q >> 24) & 0x3f);
        const unsigned size = type == 0 ? 4 : 2;
        cpu.ps0[d] = PsqLoadOne(cpu, ea, type, scale);
        cpu.ps1[d] = PsqLoadOne(cpu, ea + size, type, scale);
      } else {
        const unsigned type = q & 7; const int scale = Scale((q >> 8) & 0x3f);
        const unsigned size = type == 0 ? 4 : 2;
        PsqStoreOne(cpu, ea, type, scale, cpu.ps0[d]);
        PsqStoreOne(cpu, ea + size, type, scale, cpu.ps1[d]);
      }
      continue;
    }
    if (op != 4) std::abort();
    const unsigned xo10 = (w >> 1) & 0x3ff, xo5 = (w >> 1) & 31;
    double r0, r1;
    if (xo10 == 528) { r0 = cpu.ps0[a]; r1 = cpu.ps0[b]; }       // ps_merge00
    else if (xo10 == 624) { r0 = cpu.ps1[a]; r1 = cpu.ps1[b]; }  // ps_merge11
    else if (xo5 == 25) { r0 = Single(cpu.ps0[a] * Force25(cpu.ps0[c])); r1 = Single(cpu.ps1[a] * Force25(cpu.ps1[c])); }
    else if (xo5 == 29) { r0 = Fma(cpu.ps0[a], cpu.ps0[c], cpu.ps0[b]); r1 = Fma(cpu.ps1[a], cpu.ps1[c], cpu.ps1[b]); }
    else if (xo5 == 31) { r0 = -Fma(cpu.ps0[a], cpu.ps0[c], cpu.ps0[b]); r1 = -Fma(cpu.ps1[a], cpu.ps1[c], cpu.ps1[b]); }
    else if (xo5 == 21) { r0 = Single(cpu.ps0[a] + cpu.ps0[b]); r1 = Single(cpu.ps1[a] + cpu.ps1[b]); }
    else if (xo5 == 20) { r0 = Single(cpu.ps0[a] - cpu.ps0[b]); r1 = Single(cpu.ps1[a] - cpu.ps1[b]); }
    else std::abort();
    cpu.ps0[d] = r0; cpu.ps1[d] = r1;
  }
}

// GC_IDCT's driver: constants in f22..f29 (lfs fills both slots), GQR7 set,
// four row calls (r3 += 32, r4 += 64) then four column calls (r3 += 4, r4 += 8).
void Reference(std::int16_t* block, const float* k, std::uint32_t gqr7) {
  Cpu cpu;
  constexpr std::uint32_t kBlock = 0x100, kTemp = 0x400;
  for (unsigned i = 0; i < 64; ++i) Store16(cpu, kBlock + 2 * i, std::uint16_t(block[i]));
  for (unsigned i = 0; i < 8; ++i) cpu.ps0[22 + i] = cpu.ps1[22 + i] = double(k[i]);
  cpu.gqr[0] = 0; cpu.gqr[7] = gqr7;
  cpu.r[3] = kBlock; cpu.r[4] = kTemp;
  for (unsigned i = 0; i < 4; ++i) { Run(cpu, EaFixture("ea_ps_idct_row").data(), EaFixture("ea_ps_idct_row").size()); cpu.r[3] += 32; cpu.r[4] += 64; }
  cpu.r[3] = kBlock; cpu.r[4] = kTemp;
  for (unsigned i = 0; i < 4; ++i) { Run(cpu, EaFixture("ea_ps_idct_col").data(), EaFixture("ea_ps_idct_col").size()); cpu.r[3] += 4; cpu.r[4] += 8; }
  for (unsigned i = 0; i < 64; ++i) block[i] = std::int16_t(Load16(cpu, kBlock + 2 * i));
}
}  // namespace

int main() {
  // MoH Frontline's table (0x8018B900).
  const float k[8] = {0.3535533845424652f, 0.4619397521018982f, 0.19134171307086945f, 0.49039262533187866f,
                      0.41573479771614075f, 0.27778512239456177f, 0.09754516184329987f, 0.0f};
  std::mt19937 rng(387);
  unsigned mismatches = 0, cases = 0;
  for (unsigned n = 0; n < 300000; ++n) {
    std::int16_t block[64]{};
    const unsigned mode = n % 5;
    if (mode == 0) {
      for (auto& v : block) v = std::int16_t(int(rng() % 4096) - 2048);
    } else if (mode == 1) {
      block[0] = std::int16_t(int(rng() % 4096) - 2048);
      const unsigned coded = rng() % 8;
      for (unsigned i = 0; i < coded; ++i) block[rng() % 64] = std::int16_t(int(rng() % 512) - 256);
    } else if (mode == 2) {
      for (auto& v : block) v = std::int16_t(int(rng() % 65536) - 32768);
    } else if (mode == 3) {
      for (auto& v : block) v = std::int16_t(int(rng() % 64) - 32);
    } else {
      block[rng() % 64] = std::int16_t(rng() % 2 ? 32767 : -32768);
    }
    // MoH Frontline's GQR7, then 007 Agent Under Fire Movie.elf's unscaled one.
    const std::uint32_t gqr7 = n & 1u ? 0x00070007u : Ea::kIdctGqr7;
    std::int16_t expected[64], actual[64];
    std::memcpy(expected, block, sizeof(block));
    std::memcpy(actual, block, sizeof(block));
    Reference(expected, k, gqr7);
    Ea::GcIdct(actual, k, gqr7);
    ++cases;
    if (std::memcmp(expected, actual, sizeof(actual)) != 0) {
      if (mismatches++ < 4) {
        for (unsigned i = 0; i < 64; ++i)
          if (expected[i] != actual[i]) { std::fprintf(stderr, "case %u mode %u index %u: guest=%d native=%d\n", n, mode, i, expected[i], actual[i]); break; }
      }
    }
  }
  // Recognition: the signatures match themselves and reject a changed word.
  std::vector<std::uint32_t> image(0x400, 0);
  constexpr std::uint32_t base = 0x80000000u;
  const auto place = [&](std::uint32_t address, const auto& words) {
    for (std::size_t i = 0; i < words.size(); ++i) image[(address - base) / 4 + i] = words[i];
  };
  place(base + 0x000, EaFixture("ea_ps_idct_row"));
  place(base + 0x130, EaFixture("ea_ps_idct_col"));
  place(base + 0x220, EaFixture("ea_gc_idct"));
  const auto read = [&](std::uint32_t address, std::uint32_t* word) {
    if (address < base || (address - base) / 4 >= image.size()) return false;
    *word = image[(address - base) / 4];
    return true;
  };
  Ea::IdctPlan plan;
  const bool matched = Ea::MatchGcIdct(base + 0x220, read, &plan);
  std::uint32_t found[4]; std::size_t count = 0;
  Ea::FindGcIdct(base, image.size(), read, found, 4, &count);
  image[(base + 0x130 - base) / 4 + 9] ^= 0x10u;  // one bit of the column routine
  Ea::IdctPlan broken;
  const bool rejected = !Ea::MatchGcIdct(base + 0x220, read, &broken);
  std::printf("EA_GC_IDCT cases=%u mismatches=%u matched=%d table=%08x gqr7=%08x row=%08x col=%08x found=%zu rejected=%d\n",
              cases, mismatches, matched, plan.table, plan.gqr7, plan.row, plan.col, count, rejected);
  const bool ok = mismatches == 0 && matched && plan.table == 0x8018b900u && plan.gqr7 == Ea::kIdctGqr7 && plan.row == base &&
                  plan.col == base + 0x130 && count == 1 && found[0] == base + 0x220 && rejected;
  return ok ? 0 : 1;
}
