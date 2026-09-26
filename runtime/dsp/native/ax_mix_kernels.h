#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#if defined(__AVX2__)
#include <immintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace GekkoAOT::DSP::AXKernels {

inline std::int16_t ClampS16(std::int64_t value) {
  return static_cast<std::int16_t>(std::clamp<std::int64_t>(value, -32768, 32767));
}

inline std::int32_t ClampS32(std::int64_t value) {
  return static_cast<std::int32_t>(std::clamp<std::int64_t>(
      value, std::numeric_limits<std::int32_t>::min(),
      std::numeric_limits<std::int32_t>::max()));
}

struct VoiceMixResult {
  std::uint16_t volume = 0;
  std::int16_t last = 0;
};

// Exact GameCube AX fixed-point voice mix. The scalar path is authoritative.
// AVX2/NEON only vectorise the independent multiply/shift/clamp stage when the
// volume is constant; saturated accumulation remains scalar so signed 32-bit
// overflow behaviour is bit-identical to the reference implementation.
inline VoiceMixResult MixVoice(std::int32_t* output, const std::int16_t* input,
                               std::size_t count, std::uint16_t volume,
                               std::uint16_t volume_delta, bool ramp) {
  VoiceMixResult result{volume, 0};
  if (!output || !input || count == 0u) return result;
  if (!ramp) volume_delta = 0u;

  std::size_t i = 0;
  if (volume_delta == 0u) {
#if defined(__AVX2__)
    alignas(32) std::int32_t scaled[8];
    const __m256i vol = _mm256_set1_epi32(static_cast<int>(volume));
    const __m256i lo = _mm256_set1_epi32(-32768);
    const __m256i hi = _mm256_set1_epi32(32767);
    for (; i + 8u <= count; i += 8u) {
      const __m128i in16 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(input + i));
      __m256i in32 = _mm256_cvtepi16_epi32(in16);
      __m256i prod = _mm256_mullo_epi32(in32, vol);
      __m256i q15 = _mm256_srai_epi32(prod, 15);
      q15 = _mm256_max_epi32(q15, lo);
      q15 = _mm256_min_epi32(q15, hi);
      _mm256_store_si256(reinterpret_cast<__m256i*>(scaled), q15);
      for (std::size_t lane = 0; lane < 8u; ++lane) {
        result.last = static_cast<std::int16_t>(scaled[lane]);
        output[i + lane] = ClampS32(static_cast<std::int64_t>(output[i + lane]) + result.last);
      }
    }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    alignas(16) std::int16_t scaled[8];
    const int32x4_t vol32 = vdupq_n_s32(static_cast<std::int32_t>(volume));
    for (; i + 8u <= count; i += 8u) {
      const int16x8_t in16 = vld1q_s16(input + i);
      int32x4_t lo32 = vmovl_s16(vget_low_s16(in16));
      int32x4_t hi32 = vmovl_s16(vget_high_s16(in16));
      lo32 = vshrq_n_s32(vmulq_s32(lo32, vol32), 15);
      hi32 = vshrq_n_s32(vmulq_s32(hi32, vol32), 15);
      const int16x8_t q15 = vcombine_s16(vqmovn_s32(lo32), vqmovn_s32(hi32));
      vst1q_s16(scaled, q15);
      for (std::size_t lane = 0; lane < 8u; ++lane) {
        result.last = scaled[lane];
        output[i + lane] = ClampS32(static_cast<std::int64_t>(output[i + lane]) + result.last);
      }
    }
#endif
  }

  std::uint16_t vol = static_cast<std::uint16_t>(
      volume + static_cast<std::uint16_t>(i * static_cast<std::size_t>(volume_delta)));
  for (; i < count; ++i) {
    result.last = ClampS16((static_cast<std::int64_t>(input[i]) * vol) >> 15u);
    output[i] = ClampS32(static_cast<std::int64_t>(output[i]) + result.last);
    vol = static_cast<std::uint16_t>(vol + volume_delta);
  }
  result.volume = vol;
  return result;
}

} // namespace GekkoAOT::DSP::AXKernels
