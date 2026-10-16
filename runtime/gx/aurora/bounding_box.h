#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>

namespace GekkoAOT::GX {
bool WriteNativeBoundingBox(std::uint32_t reg, std::uint32_t value);
bool NativeBoundingBoxActive();
// v358: true once the guest has read the bounds (GXInit's reset alone is not use).
bool NativeBoundingBoxRead();
bool ReadNativeBoundingBox(std::uint16_t* bounds);
void ShutdownNativeBoundingBox();
}
