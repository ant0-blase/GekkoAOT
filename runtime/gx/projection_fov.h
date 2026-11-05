// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
namespace GekkoAOT::GX {
// GX perspective coefficients are cot(vertical FOV / 2). Scale both focal
// lengths together to retain aspect ratio and keep off-axis lens offsets.
inline float PerspectiveFovScale(float vertical, float degrees) noexcept {
  if (degrees == 0 || !std::isfinite(degrees) || degrees < 20 || degrees > 150 ||
      !std::isfinite(vertical) || std::abs(vertical) < 1e-7f)
    return 1;
  return 1.f / (std::tan(degrees * 0.008726646259971648f) * std::abs(vertical));
}
}
