// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include <cstdlib>
#include <iostream>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static void Tick(HostRuntime& r, std::uint64_t cycles) { r.AdvanceCpuTimers(cycles); }
  static bool Pending(const HostRuntime& r) { return r.decrementer_pending_; }
  static bool Deliver(HostRuntime& r) { return r.TryTakeDecrementerInterrupt(); }
  static bool Execute(HostRuntime& r, std::uint32_t pc) {
    std::uint64_t cycles = 0;
    return r.ExecuteLowMemoryInstruction(pc, &cycles);
  }
};
}
using namespace GekkoAOT::Native;
using Access = HardwareTestAccess;

int main() {
  auto runtime = std::make_unique<HostRuntime>();
  auto& r = *runtime;
  auto& cpu = r.Cpu();
  const auto read = [&](unsigned spr) { return cpu.spr_read(&cpu, spr, 0x80004000); };
  const auto write = [&](unsigned spr, unsigned value) { cpu.spr_write(&cpu, spr, value, 0x80004000); };

  write(22, 0);
  Access::Tick(r, 11);
  CHECK(cpu.timebase == 0 && read(22) == 0 && !Access::Pending(r));
  Access::Tick(r, 1);
  CHECK(cpu.timebase == 1 && read(22) == 0xffffffffu && Access::Pending(r));
  cpu.pc = 0x80004004;
  cpu.msr = 0x32;
  CHECK(!Access::Deliver(r) && Access::Pending(r));
  cpu.msr |= 0x8000;
  CHECK(Access::Deliver(r));
  CHECK(cpu.pc == 0x900 && cpu.srr0 == 0x80004004 && cpu.srr1 == 0x8032);
  CHECK((cpu.msr & 0x8032) == 0 && !Access::Pending(r));
  CHECK(r.Memory().Write32(0x900, 0x4c000064)); // independently encoded rfi
  CHECK(Access::Execute(r, 0x900));
  CHECK(cpu.pc == 0x80004004 && cpu.msr == 0x8032);
  CHECK(!Access::Deliver(r));

  r.Reset();
  write(22, 0xffffffffu);
  Access::Tick(r, 12ull * 0xffffffffu);
  CHECK(read(22) == 0 && !Access::Pending(r));
  Access::Tick(r, 12);
  CHECK(read(22) == 0xffffffffu && Access::Pending(r));
  CHECK(cpu.timebase == 0x100000000ull);
  CHECK(read(268) == 0 && read(269) == 1);

  r.Reset();
  write(22, 10);
  Access::Tick(r, 5);
  write(22, 20);
  Access::Tick(r, 7);
  CHECK(cpu.timebase == 1 && read(22) == 20);
  Access::Tick(r, 5);
  CHECK(read(22) == 19); // DEC write resets its phase, not TB's
  write(22, 0xffffffffu);
  CHECK(Access::Pending(r));

  r.Reset();
  write(952, 0x4b); // PMC1 cycles, PMC2 load/store events (unsupported)
  Access::Tick(r, 120);
  CHECK(read(953) == 120 && read(937) == 120);
  CHECK(read(936) == 0x4b && read(954) == 0);
  const auto tb = cpu.timebase;
  for (unsigned i = 0; i < 100; ++i) CHECK(read(953) == 120);
  CHECK(cpu.timebase == tb); // SPR reads must not charge time a second time
  write(953, 0xfffffff0u);
  Access::Tick(r, 32);
  CHECK(read(953) == 16);

  for (const auto mask : {0x80000000u, 0x40000000u, 0x08000000u}) {
    cpu.msr = 0;
    write(952, mask | 0x40);
    const auto before = read(953);
    Access::Tick(r, 120);
    CHECK(read(953) == before);
  }
  cpu.msr = 0x4000;
  write(952, 0x20000040u);
  const auto before_user = read(953);
  Access::Tick(r, 120);
  CHECK(read(953) == before_user);
  cpu.msr = 4;
  write(952, 0x10000040u);
  Access::Tick(r, 120);
  CHECK(read(953) == before_user);

  r.Reset();
  write(952, 0x41);
  write(956, (1u << 27) | (1u << 22));
  Access::Tick(r, 7);
  for (unsigned spr : {953u, 954u, 957u, 958u}) CHECK(read(spr) == 7);
  CHECK(read(940) == read(956) && read(941) == 7 && read(942) == 7);
  write(952, 0);
  write(956, 0);
  Access::Tick(r, 120);
  CHECK(read(953) == 7 && read(957) == 7);

  // Rising-edge event, all RTCSELECT encodings, including a low TB wrap.
  for (unsigned selector = 0; selector < 4; ++selector) {
    r.Reset();
    constexpr unsigned bits[] = {0, 8, 12, 16};
    const auto half = 1ull << bits[selector];
    cpu.timebase = 0xffffffffull;
    write(952, (selector << 23) | (3u << 6));
    Access::Tick(r, 12);
    CHECK(read(953) == 0);
    Access::Tick(r, 12 * half);
    CHECK(read(953) == 1);
    Access::Tick(r, 12 * half * 6);
    CHECK(read(953) == 4);
  }
  std::cout << "CPU timer/PMC tests passed\n";
}
