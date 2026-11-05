// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while(false)
namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static void Configure(HostRuntime& r, bool enabled, bool irq, bool dec) {
    r.ee_pending_barrier_enabled_ = enabled;
    r.ee_pending_query_enabled_ = true;
    r.compat_diagnostics_enabled_ = false;
    r.os_hooks_.clear();
    r.decrementer_pending_ = dec;
    r.pi_.Write(0xcc003004, GekkoAOT::HW::PI::NativePI::Serial, 4);
    r.pi_.SetInterrupt(GekkoAOT::HW::PI::NativePI::Serial, irq);
  }
};
}
using namespace GekkoAOT::Native;
int main() {
  auto r = std::make_unique<HostRuntime>();
  auto& cpu = r->Cpu();
  for (bool enabled : {false, true}) for (bool irq : {false, true})
    for (bool dec : {false, true}) for (unsigned epoch = 0; epoch != 256; ++epoch) {
      HardwareTestAccess::Configure(*r, enabled, irq, dec);
      cpu.external_write_count = epoch;
      cpu.pc = 0x80012340; cpu.lr = 0x80023450; cpu.msr = 0x2032;
      CPUState expected; std::memcpy(&expected, &cpu, sizeof(cpu));
      const bool pending = irq || dec;
      if (pending && enabled && epoch != 255) ++expected.external_write_count;
      CHECK(cpu.host_call(&cpu, 0xfffffff4u) == pending);
      CHECK(std::memcmp(&cpu, &expected, sizeof(cpu)) == 0);
    }
  std::cout << "EE boundary: pending PI/DEC only, independent disable, saturation and full state passed\n";
}
