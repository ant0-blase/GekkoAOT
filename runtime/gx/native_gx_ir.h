// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace GekkoAOT::GX {
// Renderer operations, independent of FIFO opcodes, guest pointers and CPUState.
// Extend this union only when both frontends can preserve the operation's
// observable ordering and side effects. In particular BP writes are not plain
// state assignments (masks, copies, tokens and cache commands have side effects).
enum class NativeArrayFields : std::uint32_t { Base = 1, Stride = 2, Both = 3 };
struct NativeArrayWrite {
  std::uint32_t index = 0, base = 0, stride = 0;
  NativeArrayFields fields = NativeArrayFields::Both;
};

inline bool NativeMatrixRange(std::uint32_t address, std::uint32_t words) noexcept {
  if (address < 0x100u)
    return !(address & 3u) && (words == 8u || words == 12u) && words <= 0x100u - address;
  if (address >= 0x400u && address < 0x460u)
    return (address - 0x400u) % 3u == 0u && words == 9u && words <= 0x460u - address;
  return address >= 0x500u && address < 0x5f0u && (address - 0x500u) % 12u == 0u &&
         words == 12u && words <= 0x5f0u - address;
}

struct NativeMatrixUpload {
  std::uint32_t address = 0, words = 0;
  // Host-endian IEEE-754 bit patterns. Preserve NaNs, signed zero and subnormals;
  // SDK PSQ normalization belongs to the SDK frontend, never to the renderer.
  // Own the data: a queue slot/guest buffer may be reused after lowering.
  std::array<std::uint32_t, 12> values{};
};
enum class NativeVertexStateKind : std::uint8_t { MatrixIndexA, MatrixIndexB, DescriptorLow, DescriptorHigh, VatA, VatB, VatC };
struct NativeVertexState {
  NativeVertexStateKind kind;
  std::uint8_t format = 0;
  std::uint32_t value = 0;
};
struct NativeProjection { std::array<std::uint32_t, 7> values{}; };
struct NativeViewport { std::array<std::uint32_t, 6> values{}; };
// Channel, light and texture-generator parameters retain their GX field
// encodings, not an XF wire header. Application shares Aurora's alias/cache
// handling with the byte frontend and never serializes these words again.
struct NativeTransformState {
  std::uint32_t address = 0, words = 0;
  std::array<std::uint32_t, 16> values{};
};
enum class NativePixelDomain : std::uint8_t {
  Geometry, Scissor, DepthBlendAlpha, Texture, Tev, Fog, CopyState,
  WriteMask, Copy, Finish, Token, TokenInterrupt, Tlut, TextureInvalidate, BoundingBox
};
struct NativePixelState {
  NativePixelDomain domain;
  std::uint8_t reg = 0;
  std::uint32_t value = 0; // Unmasked 24-bit fields; consume the current mask at execution.
};
using NativeOperation = std::variant<NativeArrayWrite, NativeMatrixUpload,
    NativeVertexState, NativeProjection, NativeViewport, NativeTransformState, NativePixelState>;
enum class NativeOrigin : std::uint8_t { Sdk, Fifo };

inline bool NativeMatrixUploadRange(std::uint32_t address, std::uint32_t words) noexcept {
  if (words == 0u || words > 12u) return false;
  // Raw XF writes can update individual rows/elements. Do not impose the SDK's
  // whole-matrix alignment on them, or cross from one physical bank to another.
  if (address < 0x100u) return words <= 0x100u - address;
  if (address >= 0x400u && address < 0x460u) return words <= 0x460u - address;
  return address >= 0x500u && address < 0x5f0u && (address - 0x500u) % 12u == 0u &&
         words == 12u && words <= 0x5f0u - address;
}

inline bool NativeTransformStateRange(std::uint32_t address, std::uint32_t words) noexcept {
  if (!words || words > 16u) return false;
  if (address >= 0x600u && address < 0x680u)
    return words <= 16u - (address & 15u);
  if (address < 0x1000u || address + words > 0x1058u) return false;
  // Projection/viewport blocks have their own typed operations. Do not admit
  // partial block semantics or cross their boundaries into ordinary registers.
  for (unsigned i = 0; i < words; ++i) {
    const auto reg = address + i;
    if (!((reg >= 0x1000u && reg <= 0x1009u) || (reg >= 0x100au && reg <= 0x1019u) ||
          (reg >= 0x103fu && reg <= 0x1057u))) return false;
  }
  return true;
}

inline std::uint8_t NativeVertexRegister(const NativeVertexState& state) noexcept {
  constexpr std::uint8_t base[]{0x30,0x40,0x50,0x60,0x70,0x80,0x90};
  return base[static_cast<unsigned>(state.kind)] + state.format;
}

inline bool LowerNativeCp(std::uint8_t reg, std::uint32_t value, NativeOperation& output) noexcept {
  if (reg >= 0xa0 && reg <= 0xbf) {
    output = NativeArrayWrite{std::uint32_t(reg & 15u), reg < 0xb0 ? value : 0u,
        reg >= 0xb0 ? value : 0u, reg < 0xb0 ? NativeArrayFields::Base : NativeArrayFields::Stride};
    return true;
  }
  NativeVertexState state{}; state.value = value;
  switch (reg) {
  case 0x30: state.kind=NativeVertexStateKind::MatrixIndexA; break;
  case 0x40: state.kind=NativeVertexStateKind::MatrixIndexB; break;
  case 0x50: state.kind=NativeVertexStateKind::DescriptorLow; break;
  case 0x60: state.kind=NativeVertexStateKind::DescriptorHigh; break;
  default:
    if (reg >= 0x70 && reg <= 0x77) state.kind=NativeVertexStateKind::VatA;
    else if (reg >= 0x80 && reg <= 0x87) state.kind=NativeVertexStateKind::VatB;
    else if (reg >= 0x90 && reg <= 0x97) state.kind=NativeVertexStateKind::VatC;
    else return false;
    state.format=reg & 7u;
  }
  output=state;return true;
}

inline bool LowerNativeBp(std::uint8_t reg, std::uint32_t value, NativePixelState& output) noexcept {
  NativePixelDomain kind;
  switch (reg) {
  case 0xfe: kind=NativePixelDomain::WriteMask;break;
  case 0x52: kind=NativePixelDomain::Copy;break;
  case 0x45: kind=NativePixelDomain::Finish;break;
  case 0x47: kind=NativePixelDomain::Token;break;
  case 0x48: kind=NativePixelDomain::TokenInterrupt;break;
  case 0x65: kind=NativePixelDomain::Tlut;break;
  case 0x66: kind=NativePixelDomain::TextureInvalidate;break;
  case 0x55: case 0x56: kind=NativePixelDomain::BoundingBox;break;
  case 0x20: case 0x21: case 0x59: kind=NativePixelDomain::Scissor;break;
  case 0: case 0xf4: case 0xf5: kind=NativePixelDomain::Geometry;break;
  case 0xf3: kind=NativePixelDomain::DepthBlendAlpha;break;
  default:
    if (reg >= 0x40 && reg <= 0x43) kind=NativePixelDomain::DepthBlendAlpha;
    else if (reg >= 0x49 && reg <= 0x51) kind=NativePixelDomain::CopyState;
    else if ((reg >= 0x80 && reg <= 0xbb) || reg == 0x64 || (reg >= 0x30 && reg <= 0x3f)) kind=NativePixelDomain::Texture;
    else if ((reg >= 0xe8 && reg <= 0xf2)) kind=NativePixelDomain::Fog;
    else if ((reg >= 0xc0 && reg <= 0xe7) || (reg >= 0xf6 && reg <= 0xfd) || (reg >= 0x10 && reg <= 0x1f) || (reg >= 0x25 && reg <= 0x2f) || reg == 0x06) kind=NativePixelDomain::Tev;
    else return false;
  }
  output={kind,reg,value & 0xffffffu};return true;
}

inline bool ValidNativeOperation(const NativeOperation& operation) noexcept {
  if (const auto* array = std::get_if<NativeArrayWrite>(&operation))
    return array->index < 16u && std::uint32_t(array->fields) >= 1u &&
           std::uint32_t(array->fields) <= 3u;
  if (const auto* matrix = std::get_if<NativeMatrixUpload>(&operation))
    return NativeMatrixUploadRange(matrix->address, matrix->words);
  if (const auto* state = std::get_if<NativeVertexState>(&operation))
    return static_cast<unsigned>(state->kind) <= 6u && state->format < 8u &&
           (static_cast<unsigned>(state->kind) >= 4u || state->format == 0u);
  if (const auto* projection = std::get_if<NativeProjection>(&operation)) return projection->values[6] <= 1u;
  if (std::holds_alternative<NativeViewport>(operation)) return true;
  if (const auto* state = std::get_if<NativeTransformState>(&operation))
    return NativeTransformStateRange(state->address, state->words);
  const auto& pixel = std::get<NativePixelState>(operation);
  NativePixelState canonical;
  return pixel.value <= 0xffffffu && LowerNativeBp(pixel.reg,pixel.value,canonical) && canonical.domain == pixel.domain;
}

// Compatibility boundary only. The renderer consumes scalar words and never
// reconstructs an XF header or performs a second endian conversion.
inline bool LowerNativeMatrix(std::uint32_t address, std::uint32_t words,
                              std::span<const std::uint8_t> data,
                              NativeMatrixUpload& output) noexcept {
  if (!NativeMatrixUploadRange(address, words) || data.size() != words * 4u) return false;
  NativeMatrixUpload result{address, words};
  for (unsigned i = 0; i < words; ++i) {
    const auto* p = data.data() + i * 4u;
    result.values[i] = (std::uint32_t(p[0]) << 24u) | (std::uint32_t(p[1]) << 16u) |
                       (std::uint32_t(p[2]) << 8u) | p[3];
  }
  output = result;
  return true;
}

inline bool LowerNativeXf(std::uint32_t address, std::uint32_t words,
                          std::span<const std::uint8_t> data, NativeOperation& output) noexcept {
  NativeMatrixUpload matrix;
  if (LowerNativeMatrix(address,words,data,matrix)) { output=matrix; return true; }
  if (words > 16u || !words || data.size() != words*4u) return false;
  const bool projection = address == 0x1020u && words == 7u;
  const bool viewport = address == 0x101au && words == 6u;
  if (!projection && !viewport && !NativeTransformStateRange(address,words)) return false;
  const auto word=[&](unsigned i) {
    const auto* p=data.data()+4u*i;
    return (std::uint32_t(p[0])<<24)|(std::uint32_t(p[1])<<16)|(std::uint32_t(p[2])<<8)|p[3];
  };
  // Validate first, then construct the actual owned alternative in place.
  // No temporary 16-word block or variant assignment is needed for the small
  // projection/viewport blocks or for light/channel/texture-generator state.
  if (projection && word(6)>1u) return false;
  if (projection) {
    auto& value=output.emplace<NativeProjection>();
    for(unsigned i=0;i<7;++i)value.values[i]=word(i);
  } else if (viewport) {
    auto& value=output.emplace<NativeViewport>();
    for(unsigned i=0;i<6;++i)value.values[i]=word(i);
  } else {
    auto& value=output.emplace<NativeTransformState>();
    value.address=address;value.words=words;
    for(unsigned i=0;i<words;++i)value.values[i]=word(i);
  }
  return true;
}
} // namespace GekkoAOT::GX
