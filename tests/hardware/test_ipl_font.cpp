// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeHW/EXI/ipl_font.h"

#include <chrono>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void Expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}
unsigned Get16(const std::vector<std::uint8_t>& v, unsigned at) { return (v[at] << 8) | v[at + 1]; }
unsigned Get32(const std::vector<std::uint8_t>& v, unsigned at) { return (Get16(v, at) << 16) | Get16(v, at + 2); }
} // namespace

using namespace GekkoAOT::NativeHW::EXI::IplFont;

int main() {
  const auto start = std::chrono::steady_clock::now();
  std::vector<std::uint8_t> rom(0x200000u, 0u);
  Expect(InstallReplacementFonts(rom), "replacement fonts fit the boot ROM");
  const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

  // OSInitFont: read OS_FONT_ROM_SIZE_ANSI bytes, Decode, then expand the
  // 2 bpp sheet in place inside an OS_FONT_SIZE_ANSI buffer.
  std::vector<std::uint8_t> font;
  Expect(Yay0Decompress(rom.data() + kAnsiRomOffset, kAnsiRomSize, font), "ANSI font decodes");
  Expect(font == BuildAnsiFont(), "Yay0 round trip");
  Expect(Get16(font, 2) == 0x20 && Get16(font, 4) == 0xff, "Latin-1 range");
  Expect(Get16(font, 0x10) == 24 && Get16(font, 0x12) == 24, "24x24 cells");
  const unsigned image = Get32(font, 0x24), full = Get32(font, 0x28);
  Expect(image + full <= kAnsiDataSize, "expanded font fits OS_FONT_SIZE_ANSI");
  Expect(font.size() <= kAnsiDataSize - kAnsiRomSize, "decoded font leaves the ROM read buffer intact");
  std::vector<std::uint8_t> buffer(kAnsiDataSize, 0u);
  std::copy(font.begin(), font.end(), buffer.begin());
  for (int i = int(full / 2) - 1; i >= 0; --i) {
    const std::uint8_t s = buffer[image + i];
    const auto c = [&](unsigned k) { return buffer[0x2c + k]; };
    buffer[image + i * 2 + 0] = (c((s >> 6) & 3) & 0xf0) | (c((s >> 4) & 3) & 0x0f);
    buffer[image + i * 2 + 1] = (c((s >> 2) & 3) & 0xf0) | (c((s >> 0) & 3) & 0x0f);
  }
  // 'A' (code 0x21 in the sheet): its top row is ".###.", so pixel (4,0)
  // of the cell is set and (1,0) is clear.
  const auto texel = [&](unsigned code, unsigned x, unsigned y) {
    const unsigned px = (code % 21) * 24 + x, py = (code / 21) * 24 + y;
    const unsigned nibble = ((py / 8) * 64 + px / 8) * 64 + (py % 8) * 8 + px % 8;
    const std::uint8_t b = buffer[image + nibble / 2];
    return nibble % 2 ? (b & 0x0f) : (b >> 4);
  };
  Expect(texel('A' - 0x20, 4, 0) == 0xf, "A top bar is drawn");
  Expect(texel('A' - 0x20, 1, 0) == 0x0, "A top corner is empty");
  Expect(texel(' ' - 0x20, 4, 4) == 0x0, "space is blank");
  Expect(font[0x30 + ('i' - 0x20)] < font[0x30 + ('m' - 0x20)], "proportional widths");
  const auto compressed = Yay0Compress(BuildAnsiFont()).size();
  Expect(compressed <= kAnsiRomSize, "compressed font fits OS_FONT_ROM_SIZE_ANSI");
  std::printf("ipl font: compressed=%zu/%u bytes build=%.1f ms%s\n", compressed, kAnsiRomSize, ms,
              failures ? "" : " ok");
  return failures == 0 ? 0 : 1;
}
