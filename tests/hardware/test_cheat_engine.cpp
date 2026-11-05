// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/cheats.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
  using namespace GekkoAOT::Native;
  const auto file = std::filesystem::temp_directory_path() / "gekkoaot_cheat_test.txt";
  {
    std::ofstream stream(file);
    stream << "# Data-only Gecko cheats\n$Health\n"
           << "04000100 12345678\n"
           << "02000200 0000ABCD\n"
           << "00000300 000000EF\n"
           << "06000400 00000009\n"
           << "11223344 55667788\n"
           << "99000000 00000000\n";
  }
  CheatEngine engine;
  std::string err;
  assert(engine.Load(file, &err) && err.empty());
  assert(engine.Count() == 4);
  AddressSpace memory;
  std::uint32_t notifications = 0;
  auto notify = [&](std::uint32_t, std::uint32_t) { ++notifications; };
  assert(engine.Apply(memory, notify) == 4 && notifications == 4);
  assert(engine.Apply(memory, notify) == 0 && notifications == 4);
  std::uint32_t word = 0;
  assert(memory.Read32(0x80000100u, &word) && word == 0x12345678u);
  std::uint16_t halfword = 0;
  assert(memory.Read16(0x80000200u, &halfword) && halfword == 0xabcdu);
  std::uint8_t byte = 0;
  assert(memory.Read8(0x80000300u, &byte) && byte == 0xefu);
  assert(memory.Read8(0x80000408u, &byte) && byte == 0x99u);
  { std::ofstream stream(file, std::ios::trunc); stream << "FF000000 00000001\n"; }
  assert(!engine.Load(file, &err));
  assert(engine.Count() == 4); // Failed reload leaves known-good state intact.
  std::filesystem::remove(file);
}
