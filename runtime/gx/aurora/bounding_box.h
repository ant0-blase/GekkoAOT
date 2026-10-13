#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>

namespace GekkoAOT::GX {
bool WriteNativeBoundingBox(std::uint32_t reg, std::uint32_t value);
bool NativeBoundingBoxActive();
bool ReadNativeBoundingBox(std::uint16_t* bounds);
void ShutdownNativeBoundingBox();
}
