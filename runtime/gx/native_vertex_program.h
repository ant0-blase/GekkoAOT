// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define GEKKOAOT_VERTEX_SSE2 1
#endif

namespace GekkoAOT::GX {
// Compile consecutive INDEX16 fields into one native vector operation per
// vertex. Width/count/layout admission belongs to the VCD/VAT compiler. Raw
// guest indices and all shader addresses remain unchanged; this only derives
// the exact immutable resource windows. NBT3/mixed/nonconsecutive layouts keep
// the existing decoder. SSE2 is baseline x86-64, other hosts use exact scalar
// execution with the same descriptor, without CPUState or hardware accesses.
struct NativeVertexProgram {
  struct Bounds { std::uint32_t minimum = 65535, maximum = 0; };
  std::uint16_t stride = 0, offset = 0;
  std::uint8_t fields = 0;
  bool Compile(unsigned vertexStride, std::span<const std::uint16_t> offsets) noexcept {
    *this = {};
    if (offsets.size() < 2 || offsets.size() > 4 || vertexStride > 65535 ||
        unsigned(offsets.front()) + 2u * offsets.size() > vertexStride) return false;
    for (unsigned i = 1; i < offsets.size(); ++i)
      if (unsigned(offsets[i]) != unsigned(offsets.front()) + 2u * i) return false;
    stride = vertexStride; offset = offsets.front(); fields = offsets.size();
    return true;
  }
private:
  template<unsigned N> void Run(const std::uint8_t* vertices, unsigned count,
                                std::span<Bounds> bounds) const noexcept {
#ifdef GEKKOAOT_VERTEX_SSE2
    const auto bias = _mm_set1_epi16(static_cast<short>(0x8000));
    auto minimum = _mm_set1_epi16(32767), maximum = bias;
    unsigned v = 0;
    if constexpr (N == 2) {
      // v336: two consecutive INDEX16 fields (the common position+normal or
      // position+texcoord layout): four vertices per vector, one 32-bit read
      // each. Lane 2k holds field 0, lane 2k+1 field 1; folded below.
      for (; v + 4u <= count; v += 4u, vertices += 4u * stride) {
        std::uint32_t a, b, c, d;
        std::memcpy(&a, vertices + offset, 4u);
        std::memcpy(&b, vertices + stride + offset, 4u);
        std::memcpy(&c, vertices + 2u * stride + offset, 4u);
        std::memcpy(&d, vertices + 3u * stride + offset, 4u);
        const auto raw = _mm_set_epi32(static_cast<int>(d), static_cast<int>(c),
                                       static_cast<int>(b), static_cast<int>(a));
        const auto indices = _mm_xor_si128(
            _mm_or_si128(_mm_slli_epi16(raw, 8), _mm_srli_epi16(raw, 8)), bias);
        minimum = _mm_min_epi16(minimum, indices);
        maximum = _mm_max_epi16(maximum, indices);
      }
      // Fold lanes {2,4,6} onto 0 and {3,5,7} onto 1.
      minimum = _mm_min_epi16(minimum, _mm_srli_si128(minimum, 8));
      maximum = _mm_max_epi16(maximum, _mm_srli_si128(maximum, 8));
      minimum = _mm_min_epi16(minimum, _mm_srli_si128(minimum, 4));
      maximum = _mm_max_epi16(maximum, _mm_srli_si128(maximum, 4));
    }
    for (; v < count; ++v, vertices += stride) {
      // Exact-size unaligned read: no overread at the final vertex or at a
      // protected page boundary, including six-byte/three-field layouts.
      std::uint64_t packed = 0;
      std::memcpy(&packed, vertices + offset, N * 2u);
      const auto raw = _mm_cvtsi64_si128(static_cast<long long>(packed));
      const auto indices = _mm_xor_si128(
          _mm_or_si128(_mm_slli_epi16(raw, 8), _mm_srli_epi16(raw, 8)), bias);
      minimum = _mm_min_epi16(minimum, indices);
      maximum = _mm_max_epi16(maximum, indices);
    }
    std::array<std::uint16_t, 8> low{}, high{};
    _mm_storeu_si128(reinterpret_cast<__m128i*>(low.data()), _mm_xor_si128(minimum, bias));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(high.data()), _mm_xor_si128(maximum, bias));
    for (unsigned i = 0; i < N; ++i) bounds[i] = {low[i], high[i]};
#else
    for (unsigned i = 0; i < N; ++i) bounds[i] = {};
    for (unsigned v = 0; v < count; ++v, vertices += stride)
      for (unsigned i = 0; i < N; ++i) {
        const auto* p = vertices + offset + 2u * i;
        const auto index = (std::uint32_t(p[0]) << 8u) | p[1];
        if (index < bounds[i].minimum) bounds[i].minimum = index;
        if (index > bounds[i].maximum) bounds[i].maximum = index;
      }
#endif
  }
public:
  bool Execute(std::span<const std::uint8_t> vertices, unsigned count,
               std::span<Bounds> bounds) const noexcept {
    if (fields < 2 || fields > 4 || bounds.size() < fields ||
        !stride || offset + 2u * fields > stride ||
        std::size_t(count) * stride != vertices.size()) return false;
    switch (fields) {
    case 2: Run<2>(vertices.data(), count, bounds); break;
    case 3: Run<3>(vertices.data(), count, bounds); break;
    case 4: Run<4>(vertices.data(), count, bounds); break;
    }
    return true;
  }
};
} // namespace GekkoAOT::GX
