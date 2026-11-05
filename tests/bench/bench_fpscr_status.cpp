// SPDX-License-Identifier: GPL-3.0-or-later
// Microbenchmark for GEKKOAOT_FPSCR_STATUS_ELISION_V380: the scalar fmuls /
// fadds fast path DolRecomp emits in state-in-memory mode, with the exact
// FPSCR status update (round flags + FPRF) versus the elided form.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

struct CPU { double fpr[32]; double ps1[32]; std::uint32_t fpscr; };

static inline std::uint32_t Bits(float f) { std::uint32_t b; std::memcpy(&b, &f, 4); return b; }
static inline std::uint64_t Bits(double d) { std::uint64_t b; std::memcpy(&b, &d, 8); return b; }
static inline double FromBits(std::uint64_t b) { double d; std::memcpy(&d, &b, 8); return d; }

static inline std::uint32_t Classify(float value) {
  const std::uint32_t bits = Bits(value);
  const bool sign = bits >> 31, expZero = !(bits & 0x7f800000u), expMax = (bits & 0x7f800000u) == 0x7f800000u,
             fracZero = !(bits & 0x7fffffu);
  const std::uint32_t normal = sign ? 0x08u : 0x04u, zero = sign ? 0x12u : 0x02u, sub = sign ? 0x18u : 0x14u,
                      inf = sign ? 0x09u : 0x05u;
  return expZero ? (fracZero ? zero : sub) : expMax ? (fracZero ? inf : 0x11u) : normal;
}

static inline std::uint32_t RoundFlags(std::uint32_t fpscr, double exact, float rounded) {
  const double rounded64 = rounded;
  const bool inexact = exact != rounded64;
  std::uint32_t base = fpscr & ~(0x00020000u | 0x00040000u);
  std::uint32_t exception = base | 0x02000000u;
  if (!(base & 0x02000000u)) exception |= 0x80000000u;
  exception = (exception & ~0x20000000u) | ((exception & 0x01f80700u) ? 0x20000000u : 0u);
  exception = (exception & ~0x40000000u) | ((((exception >> 22) & exception) & 0xf8u) ? 0x40000000u : 0u);
  exception |= 0x00020000u;
  if ((Bits(rounded64) & 0x7fffffffffffffffull) > (Bits(exact) & 0x7fffffffffffffffull)) exception |= 0x00040000u;
  return inexact ? exception : base;
}

template <bool Exact, bool Mul>
__attribute__((always_inline)) inline void Op(CPU* cpu, int d, int a, int b) {
  const double left = cpu->fpr[a];
  double right = cpu->fpr[b];
  const std::uint32_t fpscr = cpu->fpscr;
  if (Mul) {  // 25-bit Gekko multiplier precision
    const std::uint64_t bits = Bits(right);
    right = FromBits((bits & 0xfffffffff8000000ull) + (bits & 0x0000000008000000ull));
  }
  const double exact = Mul ? left * right : left + right;
  const bool nonIEEE = fpscr & 4u;
  const bool tiny = (Bits(exact) & 0x7fffffffffffffffull) < 0x3810000000000000ull;
  if (__builtin_expect(!std::isfinite(exact) || (nonIEEE && tiny), 0)) { cpu->fpr[d] = 0; return; }
  const float rounded = static_cast<float>(exact);
  cpu->fpr[d] = rounded;
  cpu->ps1[d] = rounded;
  if (Exact) {
    std::uint32_t status = RoundFlags(fpscr, exact, rounded);
    status = (status & ~(0x1fu << 12)) | (Classify(rounded) << 12);
    cpu->fpscr = status;
  }
}

template <bool Exact>
__attribute__((noinline)) double Run(CPU& cpu, long iterations) {
  const auto start = std::chrono::steady_clock::now();
  for (long i = 0; i < iterations; ++i) {
    // A 3x3 row update: the shape of game-side matrix/vector code.
    Op<Exact, true>(&cpu, 1, 4, 7); Op<Exact, true>(&cpu, 2, 5, 8); Op<Exact, false>(&cpu, 1, 1, 2);
    Op<Exact, true>(&cpu, 3, 6, 9); Op<Exact, false>(&cpu, 1, 1, 3);
    Op<Exact, true>(&cpu, 4, 1, 10); Op<Exact, false>(&cpu, 5, 4, 11); Op<Exact, true>(&cpu, 6, 5, 12);
  }
  const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
  return ns / (iterations * 8.0);
}

int main() {
  CPU cpu{};
  for (int i = 0; i < 32; ++i) cpu.fpr[i] = 0.75 + i * 0.03125;
  const long iterations = 20'000'000;
  double exact = 1e9, elided = 1e9;
  for (int round = 0; round < 5; ++round) {
    for (int i = 0; i < 32; ++i) cpu.fpr[i] = 0.75 + i * 0.03125;
    exact = std::min(exact, Run<true>(cpu, iterations));
    for (int i = 0; i < 32; ++i) cpu.fpr[i] = 0.75 + i * 0.03125;
    elided = std::min(elided, Run<false>(cpu, iterations));
  }
  std::printf("exact-status  %.3f ns/op\nelided-status %.3f ns/op\nspeedup       %.2fx (%.0f%% less time per FP op)\n",
              exact, elided, exact / elided, 100.0 * (1.0 - elided / exact));
  return cpu.fpscr == 0xdeadbeefu;
}
