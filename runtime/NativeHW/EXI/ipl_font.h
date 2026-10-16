#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <vector>

namespace GekkoAOT::NativeHW::EXI::IplFont {

// The boot ROM holds two Yay0-compressed fonts that OSInitFont/OSLoadFont
// read through EXI. Games without a user IPL dump get a replacement drawn
// from GekkoAOT's own 5x8 glyphs (Latin-1, 24x24 cells, 2 bpp sheet).
inline constexpr std::uint32_t kAnsiRomOffset = 0x1fcf00u;
inline constexpr std::uint32_t kAnsiRomSize = 0x3000u;   // OS_FONT_ROM_SIZE_ANSI
inline constexpr std::uint32_t kAnsiDataSize = 0x20120u; // OS_FONT_SIZE_ANSI

// Uncompressed font as the SDK sees it after Decode(): 48-byte header,
// width table, then the 2 bpp sheet that OSInitFont expands to I4.
std::vector<std::uint8_t> BuildAnsiFont();

std::vector<std::uint8_t> Yay0Compress(const std::vector<std::uint8_t>& data);
// Reference decoder (the SDK's Decode loop), for verification.
bool Yay0Decompress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out);

// Writes the compressed replacement fonts into a 2 MiB ROM image. Returns
// false (leaving the region untouched) when a font does not fit its slot.
bool InstallReplacementFonts(std::vector<std::uint8_t>& rom);

} // namespace GekkoAOT::NativeHW::EXI::IplFont
