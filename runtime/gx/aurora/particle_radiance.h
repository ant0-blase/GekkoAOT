// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "texture.hpp"
#include <array>
namespace aurora::gfx::enhanced {
// Cached at original upload/update: bounded samples of the already decoded
// texture, never another decode or a framebuffer readback per draw.
void capture_particle_texture(TextureRef&,ArrayRef<uint8_t>) noexcept;
struct ParticleMaterial {
  std::array<std::array<float,4>,8> textures{};
  bool known=true;
};
ParticleMaterial particle_material();
// RGB is radiance; W says whether source/TEV evidence was available.
std::array<float,4> particle_radiance(const ParticleMaterial&,const std::array<float,4>& vertexColor);
}
