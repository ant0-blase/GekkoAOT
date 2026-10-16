#include "xxh3_dispatch.h"

#include <cstdint>
#include <cstdlib>

// GEKKOAOT_XXH3_AVX2_TU is set by CMake when xxh3_avx2.cpp is part of the build.
#if defined(GEKKOAOT_XXH3_AVX2_TU) && (defined(_M_X64) || defined(__x86_64__))
#define GEKKOAOT_XXH3_X86 1
#if defined(_MSC_VER)
#include <immintrin.h>
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

#if GEKKOAOT_XXH3_X86
XXH128_hash_t gekkoaot_xxh3_128_avx2(const void* data, std::size_t size) noexcept;
#endif

namespace {

#if GEKKOAOT_XXH3_X86
bool CpuHasAvx2() noexcept {
  unsigned regs[4]{};
#if defined(_MSC_VER)
  int info[4]{};
  __cpuid(info, 0);
  if (info[0] < 7) return false;
  __cpuidex(info, 1, 0);
  for (int i = 0; i < 4; ++i) regs[i] = static_cast<unsigned>(info[i]);
#else
  if (__get_cpuid_max(0, nullptr) < 7) return false;
  __cpuid_count(1, 0, regs[0], regs[1], regs[2], regs[3]);
#endif
  // OSXSAVE and AVX, then XCR0 must enable both the XMM and YMM state.
  constexpr unsigned kOsxsave = 1u << 27, kAvx = 1u << 28;
  if ((regs[2] & (kOsxsave | kAvx)) != (kOsxsave | kAvx)) return false;
#if defined(_MSC_VER)
  const auto xcr0 = static_cast<std::uint64_t>(_xgetbv(0));
#else
  std::uint32_t lo = 0, hi = 0;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  const auto xcr0 = (static_cast<std::uint64_t>(hi) << 32) | lo;
#endif
  if ((xcr0 & 0x6u) != 0x6u) return false;
#if defined(_MSC_VER)
  __cpuidex(info, 7, 0);
  const unsigned ebx = static_cast<unsigned>(info[1]);
#else
  unsigned eax7 = 0, ebx = 0, ecx7 = 0, edx7 = 0;
  __cpuid_count(7, 0, eax7, ebx, ecx7, edx7);
#endif
  return (ebx & (1u << 5)) != 0u;
}

bool UseAvx2() noexcept {
  static const bool enabled = [] {
    const char* knob = std::getenv("GEKKOAOT_XXH3_AVX2");
    if (knob && knob[0] == '0' && knob[1] == '\0') return false;
    return CpuHasAvx2();
  }();
  return enabled;
}
#endif

} // namespace

XXH128_hash_t gekkoaot_xxh3_128(const void* data, std::size_t size) noexcept {
#if GEKKOAOT_XXH3_X86
  if (UseAvx2()) return gekkoaot_xxh3_128_avx2(data, size);
#endif
  return XXH3_128bits(data, size);
}
