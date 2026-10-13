// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
using namespace GekkoAOT::Native;
namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static void FreezePoll(HostRuntime& r) {
    // Differential register reads have no elapsed host time. Timing progression
    // is tested separately; the shared poll service must remain on both paths.
    r.hardware_clock_started_ = false;
    r.hardware_tsc_checked_ = true;
    r.hardware_tsc_enabled_ = false;
  }
  static void AdvanceVI(HostRuntime& r, std::uint64_t cycles) { r.vi_.AdvanceCycles(cycles); }
  static void Compare(HostRuntime& a, HostRuntime& b) {
    CHECK(std::memcmp(&a.cpu_, &b.cpu_, offsetof(CPUState, external_read)) == 0);
    CHECK(a.cpu_.downcount == b.cpu_.downcount && a.cpu_.cycle_budget == b.cpu_.cycle_budget);
    CHECK(a.fault_.kind == b.fault_.kind && a.fault_.address == b.fault_.address);
    CHECK(a.mmio_reads_ == b.mmio_reads_);
    CHECK(a.last_mmio_read_ == b.last_mmio_read_);
    CHECK(a.same_mmio_read_streak_ == b.same_mmio_read_streak_);
    CHECK(a.cp_.Status() == b.cp_.Status());
    CHECK(a.cp_.FifoReadPointer() == b.cp_.FifoReadPointer());
    CHECK(a.cp_.FifoReadWriteDistance() == b.cp_.FifoReadWriteDistance());
    CHECK(a.pi_.InterruptPending() == b.pi_.InterruptPending());
    CHECK(a.pi_.FifoWritePointer() == b.pi_.FifoWritePointer());
    a.fault_ = {}; b.fault_ = {};
  }
};
}
int main() {
  auto a = std::make_unique<HostRuntime>(), b = std::make_unique<HostRuntime>();
  const auto& native = HostRuntime::NativeHardwareServices();
  CHECK(native.version == 1 && native.size == sizeof(native));
  auto& ca = a->Cpu(); auto& cb = b->Cpu();
  std::uint32_t random = 0x243abcdeu;
  const auto next = [&]() { return random = random * 1664525u + 1013904223u; };
  // Full CP/VI/PI apertures include valid registers, reserved offsets, aliases,
  // unsupported widths and misalignment: direct guards must preserve faults.
  for (unsigned trial = 0; trial < 2048; ++trial) {
    const auto epoch = trial % 4 == 0 ? 255u : next() & 255u;
    ca.external_read_count = cb.external_read_count = epoch;
    ca.external_write_count = cb.external_write_count = epoch;
    const auto alias = (trial & 1) ? 0xcc000000u : 0x8c000000u;
    const unsigned cp_offset = trial < 32 ? trial * 2 : next() & 0xfffu;
    const auto width = trial % 8 == 7 ? 4u : 2u;
    const auto cp_value = next();
    ca.external_write(&ca, alias + (cp_offset & ~1u), cp_value, 2);
    cb.external_write(&cb, alias + (cp_offset & ~1u), cp_value, 2);
    CHECK(ca.external_read(&ca, alias + cp_offset, width) ==
          native.cp_read16(&cb, alias + cp_offset, width));
    HardwareTestAccess::Compare(*a, *b);
    HardwareTestAccess::FreezePoll(*a); HardwareTestAccess::FreezePoll(*b);
    HardwareTestAccess::AdvanceVI(*a, 127); HardwareTestAccess::AdvanceVI(*b, 127);
    const auto vi_offset = trial < 59 ? trial * 2 : next() & 0xfffu;
    CHECK(ca.external_read(&ca, alias + 0x2000u + vi_offset, width) ==
          native.vi_read16(&cb, alias + 0x2000u + vi_offset, width));
    HardwareTestAccess::Compare(*a, *b);
    const auto pi_offset = trial < 16 ? trial * 4 : next() & 0xfffu;
    const auto value = next();
    const auto pi_width = trial % 8 == 7 ? 2u : 4u;
    ca.external_write(&ca, alias + 0x3000u + pi_offset, value, pi_width);
    native.pi_write32(&cb, alias + 0x3000u + pi_offset, value, pi_width);
    HardwareTestAccess::Compare(*a, *b);
    for (unsigned off = 0; off <= 0x30; off += 4) {
      std::uint64_t va = 0, vb = 0;
      CHECK(a->ProcessorInterface().Read(alias + 0x3000u + off, 4, &va) ==
            b->ProcessorInterface().Read(alias + 0x3000u + off, 4, &vb));
      CHECK(va == vb);
    }
  }
  // The native VI service must keep a compiled polling loop alive without
  // returning to the outer runtime. Sample host time then exercise 128 polls.
  auto timed = std::make_unique<HostRuntime>();
  auto& ct = timed->Cpu();
  ct.external_write(&ct, 0xcc002002, 1, 2);
  const auto before = ct.timebase;
  for (unsigned i = 0; i < 4096; ++i) native.vi_read16(&ct, 0xcc00202e, 2);
  CHECK(ct.timebase > before);
  std::cout << "Native CP/VI/PI state, faults, epochs, aliases and timed VI polling match\n";
}
