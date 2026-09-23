#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>

namespace GekkoAOTSdk {
struct CardSyncProof {
  std::uint32_t callback;
  std::uint32_t wait;
};
inline std::uint32_t CardInstruction(const std::uint8_t* p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
         (std::uint32_t(p[2]) << 8) | p[3];
}
inline std::uint32_t RelativeBranchTarget(std::uint32_t pc, std::uint32_t word) {
  auto displacement = word & 0x03fffffcu;
  if (displacement & 0x02000000u) displacement |= 0xfc000000u;
  return pc + displacement;
}
// Recognize the complete canonical SDK sync adapter, never just a nearby bl.
// Arguments r3..r(2+argc) reach Async unchanged; the next register receives
// __CARDSyncCallback. Return Async's error, or __CARDSync(original r3).
// Other compiler layouts remain guest code until independently proven.
inline std::optional<CardSyncProof> DecodeCardSyncWrapper(
    const std::uint8_t* code, std::size_t size, std::uint32_t pc,
    std::uint32_t async, unsigned argc) {
  if (!code || size != 72 || argc < 1 || argc > 5 || (pc & 3) || (async & 3))
    return std::nullopt;
  std::uint32_t w[18];
  for (unsigned i = 0; i < 18; ++i) w[i] = CardInstruction(code + 4*i);
  const unsigned callback_reg = 3 + argc;
  if (w[0] != 0x7c0802a6 || (w[1] & 0xffff0000u) != (0x3c000000u | callback_reg << 21) ||
      w[2] != 0x90010004 ||
      (w[3] & 0xffff0000u) != (0x38000000u | callback_reg << 21 | callback_reg << 16) ||
      w[4] != 0x9421ffe0 || w[5] != 0x93e1001c ||
      (w[6] != 0x3be30000 && w[6] != 0x7c7f1b78) ||
      (w[7] & 0xfc000003u) != 0x48000001u ||
      RelativeBranchTarget(pc + 28, w[7]) != async ||
      w[8] != 0x2c030000 || w[9] != 0x40800008 || w[10] != 0x4800000c ||
      (w[11] != 0x7fe3fb78 && w[11] != 0x387f0000) ||
      (w[12] & 0xfc000003u) != 0x48000001u ||
      w[13] != 0x80010024 || w[14] != 0x83e1001c || w[15] != 0x38210020 ||
      w[16] != 0x7c0803a6 || w[17] != 0x4e800020)
    return std::nullopt;
  const std::uint32_t callback = (w[1] << 16) +
      static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(w[3])));
  return CardSyncProof{callback, RelativeBranchTarget(pc + 48, w[12])};
}
} // namespace GekkoAOTSdk
