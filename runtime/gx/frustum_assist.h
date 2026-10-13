// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace GekkoAOT::GX {

struct FrustumAspectCandidate {
  std::uint32_t aspect_address = 0;
  std::uint32_t fov_address = 0;
  float aspect = 0.0f;
  float fov = 0.0f;
};

inline float ReadBigEndianFloat(const std::uint8_t* p) noexcept {
  if (!p) return 0.0f;
  const std::uint32_t bits = (std::uint32_t(p[0]) << 24u) |
                             (std::uint32_t(p[1]) << 16u) |
                             (std::uint32_t(p[2]) << 8u) |
                             std::uint32_t(p[3]);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline void WriteBigEndianFloat(std::uint8_t* p, float value) noexcept {
  if (!p) return;
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  p[0] = static_cast<std::uint8_t>(bits >> 24u);
  p[1] = static_cast<std::uint8_t>(bits >> 16u);
  p[2] = static_cast<std::uint8_t>(bits >> 8u);
  p[3] = static_cast<std::uint8_t>(bits);
}

inline bool LooksLikeAspect(float value, float expected,
                            float tolerance = 0.0020f) noexcept {
  return std::isfinite(value) && std::isfinite(expected) && expected > 0.0f &&
         std::fabs(value - expected) <= tolerance;
}

inline bool LooksLikeNativeAspect(float value) noexcept {
  return LooksLikeAspect(value, 4.0f / 3.0f, 0.0040f);
}

inline bool LooksLikeCameraFov(float value) noexcept {
  return std::isfinite(value) && value >= 20.0f && value <= 150.0f;
}

inline bool MatchesObservedFov(float value, float observed_degrees) noexcept {
  if (!LooksLikeCameraFov(observed_degrees) || !std::isfinite(value)) return false;
  const float deg_tolerance = std::max(0.35f, observed_degrees * 0.0125f);
  if (std::fabs(value - observed_degrees) <= deg_tolerance) return true;
  constexpr float kPi = 3.14159265358979323846f;
  const float radians = observed_degrees * (kPi / 180.0f);
  const float rad_tolerance = std::max(0.006f, radians * 0.0125f);
  return std::fabs(value - radians) <= rad_tolerance;
}

// v177: do not search for arbitrary "camera-looking" constants.  Aurora first
// reports the perspective projection actually consumed by GX; only RAM pairs
// matching that live FOV + aspect are candidates.  This is still deliberately
// fail-closed: more than one candidate means the runtime does nothing.
inline std::vector<FrustumAspectCandidate> FindProjectionCorrelatedCandidates(
    std::span<const std::uint8_t> memory, std::uint32_t guest_base,
    std::size_t begin_offset, std::size_t end_offset, float observed_fov_degrees,
    float observed_aspect, std::size_t max_candidates = 16u) {
  std::vector<FrustumAspectCandidate> out;
  if (memory.size() < 8u || begin_offset >= memory.size() ||
      !LooksLikeCameraFov(observed_fov_degrees) ||
      !std::isfinite(observed_aspect) || observed_aspect <= 0.0f)
    return out;
  end_offset = std::min(end_offset, memory.size());
  begin_offset = (begin_offset + 3u) & ~std::size_t{3u};
  if (end_offset < begin_offset + 8u) return out;

  for (std::size_t off = begin_offset; off + 8u <= end_offset; off += 4u) {
    const float a = ReadBigEndianFloat(memory.data() + off);
    const float b = ReadBigEndianFloat(memory.data() + off + 4u);
    if (MatchesObservedFov(a, observed_fov_degrees) &&
        LooksLikeAspect(b, observed_aspect)) {
      out.push_back({guest_base + static_cast<std::uint32_t>(off + 4u),
                     guest_base + static_cast<std::uint32_t>(off), b, a});
    } else if (LooksLikeAspect(a, observed_aspect) &&
               MatchesObservedFov(b, observed_fov_degrees)) {
      out.push_back({guest_base + static_cast<std::uint32_t>(off),
                     guest_base + static_cast<std::uint32_t>(off + 4u), a, b});
    }
    if (out.size() >= max_candidates) break;
  }
  return out;
}

}  // namespace GekkoAOT::GX
