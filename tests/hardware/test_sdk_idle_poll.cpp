// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../runtime/os/sdk_idle_poll.h"
#include <array>
#include <iostream>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
int main() {
  std::array<std::uint32_t, 12> code{
      0x7c6000a6, 0x60648000, 0x7c800124, 0x54638ffe, 0x4e800020,
      0, 0x4bffffe9, 0x800df000, 0x28000000, 0x4182fff8, 0, 0};
  for (const auto base : {0x80004000u, 0x81001000u}) {
    const auto read = [&](std::uint32_t pc, std::uint32_t* value) {
      const auto offset = pc - base;
      if ((offset & 3u) || offset >= sizeof(code)) return false;
      *value = code[offset / 4u]; return true;
    };
    CHECK(GekkoAOT::NativeOS::SdkIdlePollOffset(base + 28u, read) == -4096);
    code[1] = 0x60640000; // A similar loop that leaves interrupts disabled.
    CHECK(!GekkoAOT::NativeOS::SdkIdlePollOffset(base + 28u, read));
    code[1] = 0x60648000;
    code[9] = 0x4182fff4; // More work in the loop; cannot be parked as an SDK poll.
    CHECK(!GekkoAOT::NativeOS::SdkIdlePollOffset(base + 28u, read));
    code[9] = 0x4182fff8;
    CHECK(!GekkoAOT::NativeOS::SdkIdlePollOffset(base + 32u, read));
  }
  std::cout << "SDK idle-poll recognition passed\n";
}
