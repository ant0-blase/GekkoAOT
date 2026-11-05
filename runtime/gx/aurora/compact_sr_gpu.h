// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "compact_sr.h"
namespace aurora::gfx::hd {
std::vector<float> compact_gpu_infer(const GekkoAOT::GX::CompactSR::Model&,std::span<const float>,unsigned,unsigned);
// Padded 34-pixel halo; only the interior output is specified.
std::vector<float> compact_gpu_infer_tile(const GekkoAOT::GX::CompactSR::Model&,std::span<const float>,unsigned,unsigned);
void compact_gpu_shutdown();
}
