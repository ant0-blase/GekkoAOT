// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <memory>
#define CHECK(x) do {if(!(x)){std::cerr << __LINE__ << ": " << #x << '\n';std::abort();}} while(false)
namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static void Prepare(HostRuntime& runtime,bool enabled) {
    runtime.native_counter_poll_enabled_=enabled;
    runtime.native_counter_sample_divider_=0;
    runtime.hardware_tsc_checked_=true;runtime.hardware_tsc_enabled_=false;
    runtime.hardware_clock_started_=true;
    runtime.hardware_clock_last_=std::chrono::steady_clock::now()-std::chrono::milliseconds(1);
    runtime.hardware_clock_remainder_=0;
  }
  static auto Cycles(const HostRuntime& runtime){return runtime.perf_hardware_cycles_total_;}
  static void PendingMaskedInterrupt(HostRuntime& runtime) {
    runtime.pi_.SetInterrupt(GekkoAOT::HW::PI::NativePI::Debug);
    CHECK(runtime.pi_.Write(0xcc003004u,GekkoAOT::HW::PI::NativePI::Debug,4));
    CHECK(runtime.pi_.InterruptPending());
  }
  static void WaitMasked(HostRuntime& runtime,RunResult& result) {
    runtime.WaitForIdleEvent(result,486000u,true);
    CHECK(runtime.pi_.InterruptPending());
  }
};
}
using namespace GekkoAOT::Native;
int main() {
  auto runtime=std::make_unique<HostRuntime>();auto& cpu=runtime->Cpu();
  cpu.msr=0;cpu.pc=0x80010000;cpu.lr=0x80020000;cpu.cr=0x12345678;
  for(unsigned r=0;r<32;++r) cpu.gpr[r]=r*0x10204080u;
  cpu.spr_write(&cpu,952,64,0x80010000); // PMC1 counts core cycles.
  cpu.spr_write(&cpu,953,0xffff0000u,0x80010000);
  HardwareTestAccess::Prepare(*runtime,false);
  const auto before=cpu.spr_read(&cpu,953,0x80010000);
  for(unsigned i=0;i<128;++i) CHECK(cpu.spr_read(&cpu,953,0x80010000)==before);
  CPUState original;std::memcpy(&original,&cpu,sizeof cpu);
  const auto hardware_before=HardwareTestAccess::Cycles(*runtime);
  HardwareTestAccess::Prepare(*runtime,true);
  // This models an AOT function that never returns to the runtime between
  // reads. The user alias and ordinary SPR must see the SAME advancing clock.
  for(unsigned i=0;i<7;++i) CHECK(cpu.spr_read(&cpu,937,0x80010000)==before);
  const auto after=cpu.spr_read(&cpu,953,0x80010000);
  const auto elapsed=HardwareTestAccess::Cycles(*runtime)-hardware_before;
  CHECK(elapsed>=486000u && elapsed<48600000u);
  CHECK(after-before==static_cast<std::uint32_t>(elapsed));
  CHECK(cpu.timebase>original.timebase);
  CHECK(std::memcmp(cpu.gpr,original.gpr,sizeof cpu.gpr)==0 &&
        cpu.pc==original.pc && cpu.lr==original.lr && cpu.cr==original.cr && cpu.msr==original.msr);
  // A frozen counter preserves its value while the independent clock runs.
  cpu.spr_write(&cpu,952,0x80000040u,0x80010000);
  const auto frozen=cpu.spr_read(&cpu,953,0x80010000);
  HardwareTestAccess::Prepare(*runtime,true);
  for(unsigned i=0;i<8;++i) CHECK(cpu.spr_read(&cpu,953,0x80010000)==frozen);
  // Masked pending IRQs cannot deliver a guest exception inside a finite wait.
  HardwareTestAccess::PendingMaskedInterrupt(*runtime);
  RunResult result;result.status=RunStatus::DispatchLimitReached;
  HardwareTestAccess::WaitMasked(*runtime,result);
  CHECK(result.status==RunStatus::DispatchLimitReached && cpu.pc==original.pc && cpu.msr==0);
  std::cout << "native counter clock PASS inside_aot=1 alias=1 exact_wrap=1 frozen=1 masked_irq=latched gpr+pc+lr+cr=unchanged\n";
}
