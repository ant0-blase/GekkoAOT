// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/si/native_si.h"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
using GekkoAOT::HW::SI::NativeSI;
namespace {
constexpr uint32_t base = 0xcc006400u, csr = base + 0x34, status = base + 0x38, buffer = base + 0x80;
constexpr uint32_t start = 1, done = 1u << 31, mask = 1u << 30, error = 1u << 29;
constexpr uint64_t never = std::numeric_limits<uint64_t>::max();
uint32_t Read(NativeSI& si, uint32_t address) {
  uint64_t v = 0; CHECK(si.Read(address, 4, &v)); return uint32_t(v);
}
void Write(NativeSI& si, uint32_t address, uint32_t value) { CHECK(si.Write(address, value, 4)); }
struct Bus {
  unsigned calls = 0;
  int response = 3;
  uint32_t channel = 0, request = 1, expected = 3;
  static int Transfer(void* opaque, uint32_t channel, uint8_t* bytes, uint32_t request, uint32_t expected) {
    auto& b = *static_cast<Bus*>(opaque);
    CHECK(channel == b.channel && request == b.request && expected == b.expected);
    CHECK(bytes[0] == 0x40); ++b.calls;
    if (b.response > 0) { bytes[0] = 0x09; bytes[1] = 0; bytes[2] = 0x5a; }
    return b.response;
  }
};
void Attach(NativeSI& si, Bus& b, bool events) {
  si.SetDeviceHooks({&b, nullptr, &Bus::Transfer, nullptr}); si.SetTransferEvents(events);
}
void Begin(NativeSI& si, Bus& b) {
  Write(si, buffer, 0x40000011);
  Write(si, csr, mask | start | (b.channel << 1) | ((b.request & 127) << 16) | ((b.expected & 127) << 8));
}
uint64_t Delay(const Bus& b) { return (uint64_t(b.request + b.expected) * 8 + 2) * 1944; }
}
int main() {
  // Exact equality at completion, including wrap encoding for length 128,
  // disconnected devices and all four channels. Response bytes and IRQ must
  // remain invisible before completion; ACK never cancels an active transfer.
  for (uint32_t channel = 0; channel != 4; ++channel)
    for (uint32_t request : {1u, 128u}) for (uint32_t expected : {3u, 128u})
      for (int response : {3, -1}) {
        NativeSI old, events;
        Bus a, b; a.channel = b.channel = channel; a.request = b.request = request;
        a.expected = b.expected = expected; a.response = b.response = response;
        Attach(old, a, false); Attach(events, b, true); Begin(old, a); Begin(events, b);
        CHECK(a.calls == 1 && b.calls == 1);
        CHECK(old.CyclesUntilTransfer() == never && old.InterruptPending());
        CHECK(events.CyclesUntilTransfer() == Delay(b));
        CHECK((Read(events, csr) & start) && !(Read(events, csr) & done));
        CHECK(!events.InterruptPending() && Read(events, buffer) == 0x40000011);
        events.AdvanceCycles(Delay(b) - 1);
        CHECK(events.CyclesUntilTransfer() == 1 && !events.InterruptPending());
        events.AdvanceCycles(1);
        CHECK(events.CyclesUntilTransfer() == never && events.InterruptPending());
        CHECK(Read(events, csr) == Read(old, csr));
        CHECK(Read(events, buffer) == Read(old, buffer));
        CHECK(Read(events, status) == Read(old, status));
        CHECK(bool(Read(events, csr) & error) == (response < 0));
        events.AdvanceCycles(never); CHECK(b.calls == 1);
        Write(events, csr, mask | done); CHECK(!events.InterruptPending());
      }
  NativeSI first, second; Bus a, b;
  Attach(first, a, true); Attach(second, b, true); Begin(first, a); Begin(second, b);
  // An interrupt acknowledgment while busy cannot prematurely publish data.
  Write(first, csr, mask | done | (1u << 16) | (3u << 8));
  first.AdvanceCycles(Delay(a)); CHECK(first.InterruptPending());
  CHECK(!second.InterruptPending() && second.CyclesUntilTransfer() == Delay(b));
  second.Reset(); second.AdvanceCycles(never);
  CHECK(!second.InterruptPending() && Read(second, csr) == 0 && b.calls == 1);
  // A pending device is retried at a deadline, never once per MMIO read.
  b.response = 0; Begin(second, b); CHECK(b.calls == 2);
  for (unsigned i = 0; i < 10000; ++i) CHECK(Read(second, csr) & start);
  CHECK(b.calls == 2);
  second.AdvanceCycles(Delay(b)); CHECK(b.calls == 3 && !second.InterruptPending());
  b.response = 3; second.AdvanceCycles(Delay(b)); CHECK(b.calls == 4);
  CHECK(!second.InterruptPending()); // reply still has its own wire deadline
  second.AdvanceCycles(Delay(b)); CHECK(second.InterruptPending());
  std::cout << "SI transfer deadline, visibility, fallback equivalence, lengths, errors and owner tests passed\n";
}
