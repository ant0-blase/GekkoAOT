// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/page_table.h"
#include "native/runtime.h"
#include <cstdlib>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
using namespace GekkoAOT::Native;
using Status = PageTranslation::Status;
namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static bool Execute(HostRuntime& r, std::uint32_t pc) {
    std::uint64_t cycles = 0;
    return r.ExecuteLowMemoryInstruction(pc, &cycles);
  }
};
}
int main() {
  AddressSpace memory;
  constexpr unsigned ea = 0x7e123abc, segment = 0x005abcde, table = 0x10000;
  // Fixed expected hash/group values, computed independently of the walker.
  constexpr unsigned primary = 0x17f40, secondary = 0x18080;
  constexpr unsigned tag = 0xad5e6f38;
  CHECK(TranslatePage(memory, ea, segment, table, false, PageAccess::Read).status == Status::Missing);
  CHECK(memory.Write32(primary + 56, tag));
  CHECK(memory.Write32(primary + 60, 0x00200002));
  auto result = TranslatePage(memory, ea, segment, table, false, PageAccess::Read);
  CHECK(result.status == Status::Mapped && result.physical == 0x200abc);
  unsigned lower = 0;
  CHECK(memory.Read32(primary + 60, &lower) && lower == 0x200102);
  CHECK(TranslatePage(memory, ea, segment, table, false, PageAccess::Write).status == Status::Mapped);
  CHECK(memory.Read32(primary + 60, &lower) && lower == 0x200182);
  CHECK(memory.Write32(primary + 56, 0));
  CHECK(memory.Write32(secondary + 24, tag | 0x40));
  CHECK(memory.Write32(secondary + 28, 0x00300002));
  result = TranslatePage(memory, ea, segment, table, false, PageAccess::Read);
  CHECK(result.status == Status::Mapped && result.physical == 0x300abc);
  // All protection combinations and both supervisor/user key selections.
  for (bool user : {false, true}) for (unsigned key = 0; key < 2; ++key)
    for (unsigned pp = 0; pp < 4; ++pp) for (bool store : {false, true}) {
      CHECK(memory.Write32(secondary + 28, 0x300000 | pp));
      const auto sr = segment | (key ? (user ? 0x20000000u : 0x40000000u) : 0u);
      result = TranslatePage(memory, ea, sr, table, user, store ? PageAccess::Write : PageAccess::Read);
      const bool allowed = (!key || pp != 0) && (!store || pp == 2 || (!key && pp < 3));
      CHECK(result.status == (allowed ? Status::Mapped : Status::Protection));
      CHECK(memory.Read32(secondary + 28, &lower));
      CHECK((lower & 0x100) != 0 && bool(lower & 0x80) == (allowed && store));
    }
  CHECK(memory.Write32(secondary + 28, 0x300002));
  CHECK(TranslatePage(memory, ea, segment | 0x10000000, table, false, PageAccess::Execute).status == Status::NoExecute);
  CHECK(memory.Read32(secondary + 28, &lower) && lower == 0x300002);
  CHECK(memory.Write32(secondary + 28, 0x30000a));
  CHECK(TranslatePage(memory, ea, segment, table, false, PageAccess::Execute).status == Status::NoExecute);
  CHECK(memory.Read32(secondary + 28, &lower) && lower == 0x30000a);
  CHECK(TranslatePage(memory, ea, segment | 0x80000000, table, false, PageAccess::Read).status == Status::DirectStore);
  CHECK(TranslatePage(memory, ea, segment, 0x10002, false, PageAccess::Read).status == Status::InvalidTable);
  CHECK(TranslatePage(memory, ea, segment, 0x10001, false, PageAccess::Read).status == Status::InvalidTable);
  CHECK(TranslatePage(memory, ea, segment, 0x1800000, false, PageAccess::Read).status == Status::InvalidTable);
  CHECK(memory.Mem1().size() == 24u * 1024 * 1024);
  auto runtime = std::make_unique<HostRuntime>();
  auto& cpu = runtime->Cpu();
  auto& ram = runtime->Memory();
  cpu.msr = 0x32;
  cpu.sr[7] = segment;
  cpu.spr_write(&cpu, 25, table, 0x80004000);
  CHECK(ram.PagedVmem() && ram.Resolve(ea, 4) == nullptr && ram.FakeVmem().empty());
  CHECK(ram.Write32(primary + 56, tag));
  CHECK(ram.Write32(primary + 60, 0x200002));
  CHECK(ram.Write32(0x200abc, 0x12345678));
  CHECK(cpu.external_read(&cpu, ea, 4) == 0x12345678);
  cpu.external_write(&cpu, ea, 0xaabbccdd, 4);
  CHECK(ram.Read32(0x200abc, &lower) && lower == 0xaabbccdd);
  CHECK(cpu.external_pointer(&cpu, ea, 4) == nullptr); // no unsafe HLE pointer
  CHECK(ram.Write32(primary + 56, 0));
  cpu.pc = 0x80004000;
  CHECK(cpu.external_read(&cpu, ea, 4) == 0);
  CHECK(cpu.pc == 0x300 && cpu.srr0 == 0x80004000 && cpu.srr1 == 0x32);
  CHECK(cpu.dar == ea && cpu.dsisr == 0x40000000 && (cpu.exception & 2));
  CHECK((cpu.msr & 0x10) == 0);
  // The guest handler supplies a physical frame, then RFI retries the load.
  CHECK(ram.Write32(primary + 56, tag));
  CHECK(ram.Write32(0x300, 0x4c000064));
  CHECK(HardwareTestAccess::Execute(*runtime, 0x300));
  CHECK(cpu.pc == 0x80004000 && cpu.msr == 0x32 && cpu.exception == 0);
  CHECK(cpu.external_read(&cpu, ea, 4) == 0xaabbccdd);
  CHECK(ram.Write32(primary + 60, 0x200003));
  cpu.external_write(&cpu, ea, 0xffffffff, 4);
  CHECK(cpu.pc == 0x300 && cpu.dsisr == 0x0a000000 && cpu.srr0 == 0x80004000);
  CHECK(ram.Read32(0x200abc, &lower) && lower == 0xaabbccdd);
  CHECK(HardwareTestAccess::Execute(*runtime, 0x300));
  // DBAT overrides a missing or protected hashed mapping.
  cpu.spr_write(&cpu, 536, 0x7e000002, cpu.pc);
  cpu.spr_write(&cpu, 537, 0x00400002, cpu.pc);
  CHECK(ram.Write32(0x400020, 0x87654321));
  CHECK(cpu.external_read(&cpu, 0x7e000020, 4) == 0x87654321);
  // A scalar straddles two independently mapped physical frames.
  CHECK(ram.Write32(primary + 60, 0x200002));
  CHECK(ram.Write32(0x17e80, tag));
  CHECK(ram.Write32(0x17e84, 0x500002));
  cpu.external_write(&cpu, 0x7e123ffe, 0x11223344, 4);
  CHECK(cpu.exception == 0);
  CHECK(cpu.external_read(&cpu, 0x7e123ffe, 4) == 0x11223344);
  std::uint16_t half = 0;
  CHECK(ram.Read16(0x200ffe, &half) && half == 0x1122);
  CHECK(ram.Read16(0x500000, &half) && half == 0x3344);
  CHECK(ram.Write32(0x17e80, 0));
  cpu.external_write(&cpu, 0x7e123ffe, 0xaabbccdd, 4);
  CHECK(cpu.pc == 0x300 && cpu.dsisr == 0x42000000);
  CHECK(cpu.dar == 0x7e124000);
  CHECK(ram.Read16(0x200ffe, &half) && half == 0x1122);
  CHECK(HardwareTestAccess::Execute(*runtime, 0x300));
  cpu.pc = 0x80004004;
  cpu.gpr[4] = 0x7e123ffc;
  cpu.fpr[1] = 99.0;
  cpu.ps1[1] = 77.0;
  cpu.instruction_fallback(&cpu, (49u << 26) | (1u << 21) | (4u << 16) | 4u, cpu.pc);
  CHECK(cpu.pc == 0x300 && cpu.srr0 == 0x80004004);
  CHECK(cpu.gpr[4] == 0x7e123ffc && cpu.fpr[1] == 99.0 && cpu.ps1[1] == 77.0);
  CHECK(HardwareTestAccess::Execute(*runtime, 0x300));
  cpu.instruction_fallback(&cpu, 0x80640000, cpu.pc); // unsupported fallback must unwind AOT
  CHECK((cpu.exception & 0x80000000u) != 0);
  CHECK(ram.Mem1().size() == 0x1800000);
  std::cout << "VM hashed pages/protection/history/bounds/DSI/RFI tests passed\n";
}
