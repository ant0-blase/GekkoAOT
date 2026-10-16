// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeHW/EXI/ipl_font.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

namespace GekkoAOT::NativeHW::EXI::IplFont {
namespace {
// GekkoAOT's own glyphs: 5 columns x 8 rows ('#' set), row 6 is the
// baseline row, row 7 the descender. Each source pixel becomes 3x3.
using Glyph = std::array<std::string_view, 8>;
constexpr std::string_view B = ".....";

constexpr std::array<Glyph, 95> kAscii{{
    {B, B, B, B, B, B, B, B},                                                    // ' '
    {"..#..", "..#..", "..#..", "..#..", "..#..", B, "..#..", B},                // !
    {".#.#.", ".#.#.", ".#.#.", B, B, B, B, B},                                  // "
    {".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#.", B},          // #
    {"..#..", ".####", "#.#..", ".###.", "..#.#", "####.", "..#..", B},          // $
    {"##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##", B},          // %
    {".##..", "#..#.", "#.#..", ".#...", "#.#.#", "#..#.", ".##.#", B},          // &
    {"..#..", "..#..", ".#...", B, B, B, B, B},                                  // '
    {"...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#.", B},          // (
    {".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#...", B},          // )
    {B, "..#..", "#.#.#", ".###.", "#.#.#", "..#..", B, B},                      // *
    {B, "..#..", "..#..", "#####", "..#..", "..#..", B, B},                      // +
    {B, B, B, B, B, ".##..", "..#..", ".#..."},                                  // ,
    {B, B, B, "#####", B, B, B, B},                                              // -
    {B, B, B, B, B, ".##..", ".##..", B},                                        // .
    {B, "....#", "...#.", "..#..", ".#...", "#....", B, B},                      // /
    {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###.", B},          // 0
    {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###.", B},          // 1
    {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####", B},          // 2
    {"#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###.", B},          // 3
    {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#.", B},          // 4
    {"#####", "#....", "####.", "....#", "....#", "#...#", ".###.", B},          // 5
    {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###.", B},          // 6
    {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#...", B},          // 7
    {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###.", B},          // 8
    {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##..", B},          // 9
    {B, ".##..", ".##..", B, ".##..", ".##..", B, B},                            // :
    {B, ".##..", ".##..", B, ".##..", "..#..", ".#...", B},                      // ;
    {"...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#.", B},          // <
    {B, B, "#####", B, "#####", B, B, B},                                        // =
    {".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#...", B},          // >
    {".###.", "#...#", "....#", "...#.", "..#..", B, "..#..", B},                // ?
    {".###.", "#...#", "....#", ".##.#", "#.#.#", "#.#.#", ".###.", B},          // @
    {".###.", "#...#", "#...#", "#...#", "#####", "#...#", "#...#", B},          // A
    {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####.", B},          // B
    {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###.", B},          // C
    {"###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###..", B},          // D
    {"#####", "#....", "#....", "####.", "#....", "#....", "#####", B},          // E
    {"#####", "#....", "#....", "####.", "#....", "#....", "#....", B},          // F
    {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####", B},          // G
    {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#", B},          // H
    {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###.", B},          // I
    {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##..", B},          // J
    {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#", B},          // K
    {"#....", "#....", "#....", "#....", "#....", "#....", "#####", B},          // L
    {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#", B},          // M
    {"#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", B},          // N
    {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###.", B},          // O
    {"####.", "#...#", "#...#", "####.", "#....", "#....", "#....", B},          // P
    {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#", B},          // Q
    {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#", B},          // R
    {".####", "#....", "#....", ".###.", "....#", "....#", "####.", B},          // S
    {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", B},          // T
    {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###.", B},          // U
    {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#..", B},          // V
    {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#.", B},          // W
    {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#", B},          // X
    {"#...#", "#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", B},          // Y
    {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####", B},          // Z
    {".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###.", B},          // [
    {B, "#....", ".#...", "..#..", "...#.", "....#", B, B},                      // backslash
    {".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###.", B},          // ]
    {"..#..", ".#.#.", "#...#", B, B, B, B, B},                                  // ^
    {B, B, B, B, B, B, "#####", B},                                              // _
    {".#...", "..#..", "...#.", B, B, B, B, B},                                  // `
    {B, B, ".###.", "....#", ".####", "#...#", ".####", B},                      // a
    {"#....", "#....", "#.##.", "##..#", "#...#", "#...#", "####.", B},          // b
    {B, B, ".###.", "#....", "#....", "#...#", ".###.", B},                      // c
    {"....#", "....#", ".##.#", "#..##", "#...#", "#...#", ".####", B},          // d
    {B, B, ".###.", "#...#", "#####", "#....", ".###.", B},                      // e
    {"..##.", ".#..#", ".#...", "###..", ".#...", ".#...", ".#...", B},          // f
    {B, B, ".####", "#...#", "#...#", ".####", "....#", ".###."},                // g
    {"#....", "#....", "#.##.", "##..#", "#...#", "#...#", "#...#", B},          // h
    {"..#..", B, ".##..", "..#..", "..#..", "..#..", ".###.", B},                // i
    {"...#.", B, "..##.", "...#.", "...#.", "...#.", "#..#.", ".##.."},          // j
    {"#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#.", B},          // k
    {".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###.", B},          // l
    {B, B, "##.#.", "#.#.#", "#.#.#", "#...#", "#...#", B},                      // m
    {B, B, "#.##.", "##..#", "#...#", "#...#", "#...#", B},                      // n
    {B, B, ".###.", "#...#", "#...#", "#...#", ".###.", B},                      // o
    {B, B, "####.", "#...#", "#...#", "####.", "#....", "#...."},                // p
    {B, B, ".####", "#...#", "#...#", ".####", "....#", "....#"},                // q
    {B, B, "#.##.", "##..#", "#....", "#....", "#....", B},                      // r
    {B, B, ".####", "#....", ".###.", "....#", "####.", B},                      // s
    {".#...", ".#...", "###..", ".#...", ".#...", ".#..#", "..##.", B},          // t
    {B, B, "#...#", "#...#", "#...#", "#..##", ".##.#", B},                      // u
    {B, B, "#...#", "#...#", "#...#", ".#.#.", "..#..", B},                      // v
    {B, B, "#...#", "#...#", "#.#.#", "#.#.#", ".#.#.", B},                      // w
    {B, B, "#...#", ".#.#.", "..#..", ".#.#.", "#...#", B},                      // x
    {B, B, "#...#", "#...#", "#...#", ".####", "....#", ".###."},                // y
    {B, B, "#####", "...#.", "..#..", ".#...", "#####", B},                      // z
    {"...#.", "..#..", "..#..", ".#...", "..#..", "..#..", "...#.", B},          // {
    {"..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", B},          // |
    {".#...", "..#..", "..#..", "...#.", "..#..", "..#..", ".#...", B},          // }
    {B, B, ".#...", "#.#.#", "...#.", B, B, B},                                  // ~
}};

// Latin-1 symbols with their own shapes (others in A0-BF stay blank).
struct Special { std::uint8_t code; Glyph glyph; };
constexpr std::array<Special, 24> kSpecial{{
    {0xa1, {B, "..#..", B, "..#..", "..#..", "..#..", "..#..", "..#.."}},                // inverted !
    {0xa2, {"..#..", ".###.", "#.#..", "#.#..", "#.#.#", ".###.", "..#..", B}},          // cent
    {0xa3, {"..##.", ".#..#", ".#...", "###..", ".#...", ".#..#", "#.##.", B}},          // pound
    {0xa5, {"#...#", ".#.#.", "#####", "..#..", "#####", "..#..", "..#..", B}},          // yen
    {0xa7, {".###.", "#....", ".##..", "#..#.", ".##..", "...#.", "###..", B}},          // section
    {0xa8, {".#.#.", B, B, B, B, B, B, B}},                                              // diaeresis
    {0xab, {B, "..#.#", ".#.#.", "#.#..", ".#.#.", "..#.#", B, B}},                      // <<
    {0xac, {B, B, "#####", "....#", B, B, B, B}},                                        // not
    {0xad, {B, B, B, "#####", B, B, B, B}},                                              // soft hyphen
    {0xaf, {"#####", B, B, B, B, B, B, B}},                                              // macron
    {0xb0, {".##..", "#..#.", ".##..", B, B, B, B, B}},                                  // degree
    {0xb1, {"..#..", "..#..", "#####", "..#..", "..#..", B, "#####", B}},                // plus-minus
    {0xb2, {".##..", "...#.", "..#..", ".###.", B, B, B, B}},                            // superscript 2
    {0xb3, {".##..", "..#..", "...#.", ".##..", B, B, B, B}},                            // superscript 3
    {0xb4, {"...#.", "..#..", B, B, B, B, B, B}},                                        // acute
    {0xb5, {B, B, "#...#", "#...#", "#...#", "##.##", "#.#.#", "#...."}},                // micro
    {0xb7, {B, B, B, "..#..", B, B, B, B}},                                              // middle dot
    {0xb8, {B, B, B, B, B, B, "..#..", ".#..."}},                                        // cedilla
    {0xb9, {"..#..", ".##..", "..#..", ".###.", B, B, B, B}},                            // superscript 1
    {0xbb, {B, "#.#..", ".#.#.", "..#.#", ".#.#.", "#.#..", B, B}},                      // >>
    {0xbf, {"..#..", B, "..#..", ".#...", "#....", "#...#", ".###.", B}},                // inverted ?
    {0xd7, {B, "#...#", ".#.#.", "..#..", ".#.#.", "#...#", B, B}},                      // multiply
    {0xdf, {".##..", "#..#.", "#..#.", "#.#..", "#..#.", "#...#", "#.##.", B}},          // sharp s
    {0xf7, {B, "..#..", B, "#####", B, "..#..", B, B}},                                  // divide
}};

// Letters without an ASCII base.
constexpr std::array<Special, 10> kLetters{{
    {0xc6, {".####", "#.#..", "#.#..", "#####", "#.#..", "#.#..", "#.###", B}},          // AE
    {0xd0, {"###..", "#..#.", "#...#", "###.#", "#...#", "#..#.", "###..", B}},          // ETH
    {0xd8, {".###.", "#..##", "#.#.#", "#.#.#", "#.#.#", "##..#", ".###.", B}},          // O stroke
    {0xde, {"#....", "####.", "#...#", "#...#", "####.", "#....", "#....", B}},          // THORN
    {0xe6, {B, B, "##.#.", "..#.#", ".####", "#.#..", ".#.##", B}},                      // ae
    {0xf0, {".#.#.", "..#..", ".#.#.", "....#", ".####", "#...#", ".###.", B}},          // eth
    {0xf8, {B, B, ".###.", "#..##", "#.#.#", "##..#", ".###.", B}},                      // o stroke
    {0xfe, {B, "#....", "####.", "#...#", "#...#", "####.", "#....", "#...."}},          // thorn
    {0xc7, {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###.", "..#.."}},    // C cedilla
    {0xe7, {B, B, ".###.", "#....", "#....", "#...#", ".###.", "..#.."}},                // c cedilla
}};

enum Accent : std::uint8_t { None, Grave, Acute, Circumflex, Tilde, Diaeresis, Ring };

// Accented Latin-1 letters: base letter and accent.
struct Composed { std::uint8_t code; char base; Accent accent; };
constexpr std::array<Composed, 52> kComposed{{
    {0xc0, 'A', Grave}, {0xc1, 'A', Acute}, {0xc2, 'A', Circumflex}, {0xc3, 'A', Tilde},
    {0xc4, 'A', Diaeresis}, {0xc5, 'A', Ring}, {0xc8, 'E', Grave}, {0xc9, 'E', Acute},
    {0xca, 'E', Circumflex}, {0xcb, 'E', Diaeresis}, {0xcc, 'I', Grave}, {0xcd, 'I', Acute},
    {0xce, 'I', Circumflex}, {0xcf, 'I', Diaeresis}, {0xd1, 'N', Tilde}, {0xd2, 'O', Grave},
    {0xd3, 'O', Acute}, {0xd4, 'O', Circumflex}, {0xd5, 'O', Tilde}, {0xd6, 'O', Diaeresis},
    {0xd9, 'U', Grave}, {0xda, 'U', Acute}, {0xdb, 'U', Circumflex}, {0xdc, 'U', Diaeresis},
    {0xdd, 'Y', Acute}, {0xe0, 'a', Grave}, {0xe1, 'a', Acute}, {0xe2, 'a', Circumflex},
    {0xe3, 'a', Tilde}, {0xe4, 'a', Diaeresis}, {0xe5, 'a', Ring}, {0xe8, 'e', Grave},
    {0xe9, 'e', Acute}, {0xea, 'e', Circumflex}, {0xeb, 'e', Diaeresis}, {0xec, 'i', Grave},
    {0xed, 'i', Acute}, {0xee, 'i', Circumflex}, {0xef, 'i', Diaeresis}, {0xf1, 'n', Tilde},
    {0xf2, 'o', Grave}, {0xf3, 'o', Acute}, {0xf4, 'o', Circumflex}, {0xf5, 'o', Tilde},
    {0xf6, 'o', Diaeresis}, {0xf9, 'u', Grave}, {0xfa, 'u', Acute}, {0xfb, 'u', Circumflex},
    {0xfc, 'u', Diaeresis}, {0xfd, 'y', Acute}, {0xff, 'y', Diaeresis}, {0xa0, ' ', None},
}};

using Bitmap = std::array<std::uint8_t, 8>; // 5-bit rows, bit 4 = leftmost column

Bitmap ToBitmap(const Glyph& glyph) {
  Bitmap rows{};
  for (unsigned y = 0; y < 8; ++y)
    for (unsigned x = 0; x < 5; ++x)
      if (glyph[y][x] == '#') rows[y] |= static_cast<std::uint8_t>(0x10u >> x);
  return rows;
}

Bitmap Compose(char base, Accent accent) {
  Bitmap rows = ToBitmap(kAscii[static_cast<unsigned char>(base) - 0x20u]);
  if (accent == None) return rows;
  const bool lower = base >= 'a';
  if (lower) {
    // Lowercase bodies start at row 2: the accent takes rows 0-1 (an 'i'
    // loses its dot).
    static constexpr std::array<std::array<std::uint8_t, 2>, 7> kTwoRow{{
        {0, 0}, {0x08, 0x04}, {0x02, 0x04}, {0x04, 0x0a}, {0x09, 0x16}, {0x00, 0x0a}, {0x04, 0x0a}}};
    rows[0] = kTwoRow[accent][0];
    rows[1] = kTwoRow[accent][1];
    return rows;
  }
  // Capitals fill rows 0-6: drop one body row (a repeated one when the
  // letter has it) and put a one-row accent above.
  static constexpr std::array<std::uint8_t, 7> kOneRow{0, 0x08, 0x02, 0x04, 0x0d, 0x0a, 0x0e};
  unsigned drop = 3;
  for (unsigned y = 1; y < 6; ++y)
    if (rows[y] == rows[y + 1]) { drop = y; break; }
  Bitmap out{};
  out[0] = kOneRow[accent];
  for (unsigned y = 0, o = 1; y < 7; ++y)
    if (y != drop) out[o++] = rows[y];
  out[7] = rows[7];
  return out;
}

std::array<Bitmap, 224> BuildGlyphs() {
  std::array<Bitmap, 224> glyphs{};
  for (unsigned i = 0; i < kAscii.size(); ++i) glyphs[i] = ToBitmap(kAscii[i]);
  for (const auto& s : kSpecial) glyphs[s.code - 0x20u] = ToBitmap(s.glyph);
  for (const auto& s : kLetters) glyphs[s.code - 0x20u] = ToBitmap(s.glyph);
  for (const auto& c : kComposed) glyphs[c.code - 0x20u] = Compose(c.base, c.accent);
  return glyphs;
}

// Scale3x (AdvanceMAME) magnification: 3x3 pixels per source pixel with
// diagonal steps smoothed, still two-level.
std::array<std::array<bool, 15>, 24> Scale3x(const Bitmap& rows) {
  const auto at = [&](int x, int y) {
    return x >= 0 && x < 5 && y >= 0 && y < 8 && (rows[y] & (0x10u >> x)) != 0;
  };
  std::array<std::array<bool, 15>, 24> out{};
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 5; ++x) {
      const bool a = at(x - 1, y - 1), b = at(x, y - 1), c = at(x + 1, y - 1);
      const bool d = at(x - 1, y), e = at(x, y), f = at(x + 1, y);
      const bool g = at(x - 1, y + 1), h = at(x, y + 1), i = at(x + 1, y + 1);
      std::array<bool, 9> p{e, e, e, e, e, e, e, e, e};
      if (b != h && d != f) {
        p[0] = d == b ? d : e;
        p[1] = (d == b && e != c) || (b == f && e != a) ? b : e;
        p[2] = b == f ? f : e;
        p[3] = (d == b && e != g) || (d == h && e != a) ? d : e;
        p[5] = (b == f && e != i) || (h == f && e != c) ? f : e;
        p[6] = d == h ? d : e;
        p[7] = (d == h && e != i) || (h == f && e != g) ? h : e;
        p[8] = h == f ? f : e;
      }
      for (int k = 0; k < 9; ++k) out[y * 3 + k / 3][x * 3 + k % 3] = p[k];
    }
  }
  return out;
}

void Put16(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
  out[at] = static_cast<std::uint8_t>(value >> 8);
  out[at + 1] = static_cast<std::uint8_t>(value);
}
void Put32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
  Put16(out, at, value >> 16);
  Put16(out, at + 2, value & 0xffffu);
}
std::uint32_t Get32(const std::uint8_t* p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
} // namespace

std::vector<std::uint8_t> BuildAnsiFont() {
  constexpr unsigned kCell = 24, kScale = 3, kColumns = 21, kRows = 21, kSheet = 512;
  constexpr unsigned kWidthTable = 0x30, kSheetImage = 0x120, kFirst = 0x20, kLast = 0xff;
  constexpr unsigned kSheetBytes = kSheet * kSheet / 2; // I4 after expansion
  std::vector<std::uint8_t> font(kSheetImage + kSheetBytes / 2, 0u);
  Put16(font, 0x00, 0);              // fontType
  Put16(font, 0x02, kFirst);         // firstChar
  Put16(font, 0x04, kLast);          // lastChar
  Put16(font, 0x06, '?');            // invalChar
  Put16(font, 0x08, 7 * kScale);     // ascent
  Put16(font, 0x0a, 1 * kScale);     // descent
  Put16(font, 0x0c, 6 * kScale);     // width (widest advance)
  Put16(font, 0x0e, kCell);          // leading
  Put16(font, 0x10, kCell);          // cellWidth
  Put16(font, 0x12, kCell);          // cellHeight
  Put32(font, 0x14, kSheetBytes);    // sheetSize
  Put16(font, 0x18, 0);              // sheetFormat GX_TF_I4
  Put16(font, 0x1a, kColumns);
  Put16(font, 0x1c, kRows);
  Put16(font, 0x1e, kSheet);
  Put16(font, 0x20, kSheet);
  Put16(font, 0x22, kWidthTable);
  Put32(font, 0x24, kSheetImage);
  Put32(font, 0x28, kSheetBytes);    // sheetFullSize
  font[0x2c] = 0x00; font[0x2d] = 0x55; font[0x2e] = 0xaa; font[0x2f] = 0xff;

  const auto glyphs = BuildGlyphs();
  for (unsigned code = 0; code < glyphs.size(); ++code) {
    const auto& rows = glyphs[code];
    std::uint8_t used = 0;
    for (const auto row : rows) used |= row;
    // Proportional advance: the used columns plus one pixel before and two
    // after; a blank glyph is a space.
    unsigned first = 0, last = 0;
    if (used) {
      first = 0;
      while (!(used & (0x10u >> first))) ++first;
      last = 4;
      while (!(used & (0x10u >> last))) --last;
    }
    font[kWidthTable + code] = static_cast<std::uint8_t>(used ? (last - first + 1) * kScale + 3 : 4 * kScale);
    const auto big = Scale3x(rows);
    const unsigned cell_x = (code % kColumns) * kCell, cell_y = (code / kColumns) * kCell;
    for (unsigned y = 0; y < 8 * kScale; ++y) {
      for (unsigned x = first * kScale; x < 5 * kScale; ++x) {
        if (!big[y][x]) continue;
        const unsigned px = cell_x + 1 + x - first * kScale, py = cell_y + y;
        // I4 tiles are 8x8: the nibble index the 2 bpp source byte expands to.
        const unsigned nibble = ((py / 8) * (kSheet / 8) + px / 8) * 64 + (py % 8) * 8 + px % 8;
        font[kSheetImage + nibble / 4] |= static_cast<std::uint8_t>(3u << (6 - 2 * (nibble % 4)));
      }
    }
  }
  return font;
}

std::vector<std::uint8_t> Yay0Compress(const std::vector<std::uint8_t>& data) {
  constexpr std::size_t kWindow = 0x1000, kMin = 3, kMax = 0xff + 0x12, kChain = 32;
  const std::size_t n = data.size();
  // Longest match at each position (hash chains over 3-byte prefixes).
  std::vector<std::uint16_t> length(n, 0);
  std::vector<std::uint16_t> distance(n, 0);
  std::vector<std::int32_t> head(1u << 16, -1), prev(n, -1);
  const auto hash = [&](std::size_t i) {
    return ((data[i] << 8) ^ (data[i + 1] << 4) ^ data[i + 2]) & 0xffffu;
  };
  for (std::size_t i = 0; i + kMin <= n; ++i) {
    const auto h = hash(i);
    std::size_t best = 0, best_distance = 0, steps = 0;
    for (std::int32_t j = head[h]; j >= 0 && i - std::size_t(j) <= kWindow && steps < kChain;
         j = prev[std::size_t(j)], ++steps) {
      const std::size_t limit = std::min(kMax, n - i);
      std::size_t l = 0;
      while (l < limit && data[std::size_t(j) + l] == data[i + l]) ++l;
      if (l > best) { best = l; best_distance = i - std::size_t(j); if (l == limit) break; }
    }
    if (best >= kMin) { length[i] = static_cast<std::uint16_t>(best); distance[i] = static_cast<std::uint16_t>(best_distance); }
    prev[i] = head[h];
    head[h] = static_cast<std::int32_t>(i);
  }
  // Minimum-size parse: a literal costs 9 bits, a short match 17, a long one 25.
  std::vector<std::uint32_t> cost(n + 1, 0);
  std::vector<std::uint16_t> choice(n + 1, 0);
  for (std::size_t i = n; i-- > 0;) {
    cost[i] = cost[i + 1] + 9u;
    choice[i] = 1;
    for (std::size_t l = kMin; l <= length[i]; ++l) {
      const std::uint32_t c = cost[i + l] + (l <= 0x11 ? 17u : 25u);
      if (c < cost[i]) { cost[i] = c; choice[i] = static_cast<std::uint16_t>(l); }
    }
  }
  std::vector<std::uint32_t> masks;
  std::vector<std::uint8_t> links, chunks;
  std::uint32_t mask = 0;
  unsigned bits = 0;
  const auto push_bit = [&](bool literal) {
    if (literal) mask |= 0x80000000u >> bits;
    if (++bits == 32) { masks.push_back(mask); mask = 0; bits = 0; }
  };
  for (std::size_t i = 0; i < n;) {
    const std::size_t l = choice[i];
    if (l == 1) {
      push_bit(true);
      chunks.push_back(data[i]);
      ++i;
      continue;
    }
    push_bit(false);
    const std::uint32_t d = distance[i] - 1u;
    const std::uint32_t link = (l <= 0x11 ? (std::uint32_t(l - 2) << 12) : 0u) | d;
    links.push_back(static_cast<std::uint8_t>(link >> 8));
    links.push_back(static_cast<std::uint8_t>(link));
    if (l > 0x11) chunks.push_back(static_cast<std::uint8_t>(l - 0x12));
    i += l;
  }
  if (bits) masks.push_back(mask);
  std::vector<std::uint8_t> out(16u + masks.size() * 4u, 0u);
  std::memcpy(out.data(), "Yay0", 4);
  const std::size_t link_offset = out.size();
  const std::size_t chunk_offset = link_offset + links.size();
  Put32(out, 4, static_cast<std::uint32_t>(n));
  Put32(out, 8, static_cast<std::uint32_t>(link_offset));
  Put32(out, 12, static_cast<std::uint32_t>(chunk_offset));
  for (std::size_t i = 0; i < masks.size(); ++i) Put32(out, 16u + i * 4u, masks[i]);
  out.insert(out.end(), links.begin(), links.end());
  out.insert(out.end(), chunks.begin(), chunks.end());
  while (out.size() % 4u) out.push_back(0u);
  return out;
}

bool Yay0Decompress(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out) {
  if (size < 16u || std::memcmp(data, "Yay0", 4) != 0) return false;
  const std::uint32_t total = Get32(data + 4);
  std::size_t link = Get32(data + 8), chunk = Get32(data + 12), mask_at = 16;
  out.assign(total, 0u);
  std::uint32_t mask = 0, bits = 0;
  for (std::size_t q = 0; q < total;) {
    if (bits == 0) {
      if (mask_at + 4u > size) return false;
      mask = Get32(data + mask_at);
      mask_at += 4u;
      bits = 32;
    }
    if (mask & 0x80000000u) {
      if (chunk >= size) return false;
      out[q++] = data[chunk++];
    } else {
      if (link + 2u > size) return false;
      const std::uint32_t v = (std::uint32_t(data[link]) << 8) | data[link + 1];
      link += 2u;
      std::size_t count = v >> 12;
      if (count == 0) {
        if (chunk >= size) return false;
        count = data[chunk++] + 0x12u;
      } else {
        count += 2u;
      }
      const std::size_t back = (v & 0xfffu) + 1u;
      if (back > q || count > total - q) return false;
      for (; count; --count, ++q) out[q] = out[q - back];
    }
    mask <<= 1;
    --bits;
  }
  return true;
}

bool InstallReplacementFonts(std::vector<std::uint8_t>& rom) {
  const auto ansi = Yay0Compress(BuildAnsiFont());
  if (ansi.size() > kAnsiRomSize || rom.size() < kAnsiRomOffset + kAnsiRomSize) return false;
  std::copy(ansi.begin(), ansi.end(), rom.begin() + kAnsiRomOffset);
  return true;
}

} // namespace GekkoAOT::NativeHW::EXI::IplFont
