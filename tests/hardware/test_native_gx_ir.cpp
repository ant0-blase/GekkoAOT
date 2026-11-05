// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/gx/native_command_stream.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (false)
using namespace GekkoAOT::GX;

int main() {
  // Compare the semantic SDK frontend against an independently decoded byte
  // stream, for every gather offset, split, and supported matrix bank.
  constexpr std::array<std::uint32_t, 12> patterns{
    0, 0x80000000, 1, 0x807fffff, 0x7f800000, 0xff800000,
    0x7fc12345, 0x7f812345, 0x3f800000, 0xbf800000, 0x12345678, 0xffffffff};
  unsigned cases = 0;
  for (auto [address, words] : std::array<std::pair<unsigned, unsigned>, 6>{
      {{0,12}, {244,12}, {8,8}, {0x400,9}, {0x457,9}, {0x5e4,12}}}) {
    std::vector<std::uint8_t> packet{0x10, 0, std::uint8_t(words-1),
                                   std::uint8_t(address >> 8), std::uint8_t(address)};
    for (unsigned i = 0; i < words; ++i)
      for (int shift = 24; shift >= 0; shift -= 8) packet.push_back(patterns[i] >> shift);
    NativeMatrixUpload fifo;
    CHECK(LowerNativeMatrix(address, words, std::span(packet).subspan(5), fifo));
    for (unsigned before = 0; before < 32; ++before) for (unsigned chunk = 1; chunk <= 64; ++chunk) {
      NativeCommandStream stream;
      CHECK(stream.StageMatrix(address, words, before));
      std::vector<std::uint8_t> input(before, 0), raw;
      input.insert(input.end(), packet.begin(), packet.end());
      input.insert(input.end(), 7, 0);
      std::vector<NativeOperation> output;
      for (std::size_t offset = 0; offset < input.size(); offset += chunk)
        CHECK(stream.ConsumeOperations(input.data() + offset, std::min<std::size_t>(chunk, input.size()-offset),
            [&](const std::uint8_t* p, std::size_t n) { raw.insert(raw.end(), p, p+n); return true; },
            [] { return true; }, [&](const NativeOperation& op) { output.push_back(op); return true; }));
      CHECK(output.size() == 1 && raw.size() == before + 7);
      CHECK(std::all_of(raw.begin(), raw.end(), [](auto b) { return b == 0; }));
      // Destroy/reuse the source bytes before inspecting the owned operation.
      std::fill(input.begin(), input.end(), 0xff);
      const auto& sdk = std::get<NativeMatrixUpload>(output[0]);
      CHECK(sdk.address == fifo.address && sdk.words == fifo.words && sdk.values == fifo.values);
      CHECK(std::equal(sdk.values.begin(), sdk.values.begin()+words, patterns.begin()));
      ++cases;
    }
  }
  // Partial raw XF loads are legal in position/normal memory; holes, bank
  // crossings and unimplemented post-matrix subloads must not be admitted.
  NativeMatrixUpload matrix{8, 8};
  std::array<std::uint8_t, 48> bytes{};
  for (auto [address, words] : std::array<std::pair<unsigned, unsigned>, 5>{
      {{3,1}, {255,1}, {0x401,11}, {0x45f,1}, {0x500,12}}}) {
    CHECK(LowerNativeMatrix(address, words, std::span(bytes).first(words*4), matrix));
    CHECK(ValidNativeOperation(matrix));
  }
  for (auto [address, words] : std::array<std::pair<unsigned, unsigned>, 10>{
      {{255,2}, {0x3ff,1}, {0x45f,2}, {0x460,1}, {0x500,1}, {0x501,12},
       {0x5f0,12}, {0,0}, {0,13}, {0xffffffff,0xffffffff}}}) {
    const auto before = matrix;
    CHECK(!LowerNativeMatrix(address, words, bytes, matrix));
    CHECK(matrix.address == before.address && matrix.words == before.words && matrix.values == before.values);
    CHECK(!ValidNativeOperation(NativeMatrixUpload{address, words}));
  }
  CHECK(!LowerNativeMatrix(0,12,std::span(bytes).first(47),matrix));
  CHECK(!ValidNativeOperation(NativeArrayWrite{16,0,0}));
  CHECK(!ValidNativeOperation(NativeArrayWrite{0,0,0,NativeArrayFields(4)}));

  // A failed sink is never retried through raw/FIFO after native side effects.
  NativeCommandStream stream;
  const std::uint8_t array[]{8,0xa0,0,0,0x10,0,8,0xb0,0,0,0,12};
  CHECK(stream.Stage({0,0x1000,12},0));
  unsigned native = 0, fallback = 0;
  CHECK(!stream.ConsumeOperations(array,sizeof array,
      [&](auto, auto) { ++fallback; return true; }, [] { return true; },
      [&](const auto&) { ++native; return false; }));
  CHECK(native == 1 && fallback == 0);
  std::printf("NativeGX IR: %u SDK/FIFO differential cases, owned IEEE bits, partial-bank guards and failure ordering passed\n", cases);
}
