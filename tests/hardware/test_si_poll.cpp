// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/si/native_si.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#define CHECK(expr) do { if (!(expr)) { std::cerr << __LINE__ << ": " #expr "\n"; std::exit(1); } } while (false)

using GekkoAOT::HW::SI::NativeSI;

namespace {
constexpr std::uint32_t kBase = 0xcc006400u;
constexpr std::uint32_t kPoll = 0x30u;
constexpr std::uint32_t kComCsr = 0x34u;
constexpr std::uint32_t kStatus = 0x38u;
constexpr std::uint32_t kIoBuffer = 0x80u;

struct PadBus {
  std::array<unsigned, 4> polls{};
  std::array<unsigned, 4> transfers{};

  static bool Poll(void* opaque, std::uint32_t channel,
                   std::uint32_t* hi, std::uint32_t* lo) {
    auto& self = *static_cast<PadBus*>(opaque);
    ++self.polls.at(channel);
    *hi = 0x10000000u | channel;
    *lo = 0x20000000u | channel;
    return true;
  }

  static int Buffer(void* opaque, std::uint32_t channel, std::uint8_t* buffer,
                    std::uint32_t request_length, std::uint32_t expected_length) {
    auto& self = *static_cast<PadBus*>(opaque);
    ++self.transfers.at(channel);
    CHECK(request_length == 1u && expected_length == 1u);
    CHECK(buffer[0] == 0x40u);
    buffer[0] = 0x5au;
    return 1;
  }
};

void Write(NativeSI& si, std::uint32_t offset, std::uint32_t value) {
  CHECK(si.Write(kBase + offset, value, 4u));
}

std::uint32_t Read(NativeSI& si, std::uint32_t offset) {
  std::uint64_t value = 0;
  CHECK(si.Read(kBase + offset, 4u, &value));
  return static_cast<std::uint32_t>(value);
}
}

int main() {
  NativeSI si;
  PadBus pads;
  si.SetDeviceHooks({&pads, &PadBus::Poll, &PadBus::Buffer, nullptr});

  // Hardware reset leaves all automatic-poll enable bits clear.
  CHECK((Read(si, kPoll) & 0xf0u) == 0u);
  Write(si, kComCsr, 1u << 27); // Unmask ready-data interrupt.
  si.PollNow();
  CHECK((pads.polls == std::array<unsigned, 4>{}));
  CHECK(Read(si, kStatus) == 0u);
  CHECK((Read(si, kComCsr) & (1u << 28)) == 0u);
  CHECK(!si.InterruptPending());

  // Enabling port 0 must not probe absent or disabled ports 1–3.
  Write(si, kPoll, (492u << 16) | 0x80u);
  si.PollNow();
  CHECK(pads.polls == (std::array<unsigned, 4>{1u, 0u, 0u, 0u}));
  CHECK(Read(si, kStatus) == 0x20000000u);
  CHECK(si.InterruptPending());
  CHECK(Read(si, 0x04u) == 0x10000000u);
  CHECK((Read(si, kStatus) & 0x20000000u) == 0u);
  CHECK(!si.InterruptPending());

  // Port 2 becomes active; port 0 stops producing samples once disabled.
  Write(si, kPoll, (492u << 16) | 0x20u);
  si.PollNow();
  CHECK(pads.polls == (std::array<unsigned, 4>{1u, 0u, 1u, 0u}));
  CHECK((Read(si, kStatus) & 0x00002000u) != 0u);
  CHECK(Read(si, 0x1cu) == 0x10000002u); // Port 2 SI input high.
  CHECK(!si.InterruptPending());

  // An explicit SI buffer transfer still works on a port whose automatic
  // polling is disabled.
  Write(si, kIoBuffer, 0x40000000u);
  Write(si, kComCsr, (1u << 30) | (1u << 16) | (1u << 8) | (1u << 1) | 1u);
  CHECK(pads.transfers == (std::array<unsigned, 4>{0u, 1u, 0u, 0u}));
  CHECK((Read(si, kComCsr) & (1u << 31)) != 0u);
  CHECK(si.InterruptPending());
  CHECK((Read(si, kIoBuffer) >> 24) == 0x5au);
  Write(si, kComCsr, (1u << 30) | (1u << 31)); // ACK TCINT.
  CHECK(!si.InterruptPending());

  std::cout << "SI selective polling and explicit transfer tests passed\n";
}
