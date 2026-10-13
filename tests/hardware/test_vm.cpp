// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/page_table.h"
#include "native/runtime.h"
#include <cstdlib>
#include <iostream>
#include <utility>
#if defined(__linux__)
#include <unistd.h>
#endif
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
using namespace GekkoAOT::Native;
using Status = PageTranslation::Status;
namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static PageTranslation Translate(HostRuntime& r, std::uint32_t ea, bool write = false) {
    const auto result = r.TranslateVmemAddress(ea, write);
    return {result.status, result.physical};
  }
  static bool Strstr(HostRuntime& r, std::uint32_t pc) {
    return r.DispatchSimpleHle(SimpleHleKind::Strstr, pc);
  }
  static bool Simple(HostRuntime& r, SimpleHleKind kind, std::uint32_t pc) {
    return r.DispatchSimpleHle(kind, pc);
  }
  static std::uint32_t DeclineEntry(const HostRuntime& r) { return r.hle_decline_entry_; }
  static bool Execute(HostRuntime& r, std::uint32_t pc) {
    std::uint64_t cycles = 0;
    return r.ExecuteLowMemoryInstruction(pc, &cycles);
  }
};
}
static void VerifyBatVmemAdmission() {
  auto runtime = std::make_unique<HostRuntime>();
  auto& cpu = runtime->Cpu();
  const auto set = [&](unsigned index, unsigned upper, unsigned lower) {
    cpu.spr_write(&cpu, 536u + index * 2u, upper, 0x80004000);
    cpu.spr_write(&cpu, 537u + index * 2u, lower, 0x80004000);
  };
  const auto translation = [&](unsigned ea, bool write = false) {
    return HardwareTestAccess::Translate(*runtime, ea, write);
  };
  cpu.msr = 0x32;
  CHECK(translation(0x7e000020).status == Status::Missing);
  set(0, 0x7e000002, 0x00400002); // supervisor only, 128 KiB
  CHECK(translation(0x7e000020).physical == 0x00400020);
  // A lower-half update changes the physical frame without changing eligibility.
  cpu.spr_write(&cpu, 537, 0x00600002, 0x80004000);
  CHECK(translation(0x7e000020).physical == 0x00600020);
  cpu.msr |= 0x4000;
  CHECK(translation(0x7e000020).status == Status::Missing);
  cpu.spr_write(&cpu, 536, 0x7e000001, 0x80004000); // user only
  CHECK(translation(0x7e000020).physical == 0x00600020);
  cpu.msr &= ~0x4000u;
  CHECK(translation(0x7e000020).status == Status::Missing);
  // An overlapping, protection-denying BAT still takes priority over BAT1/HPT.
  set(0, 0x7e000002, 0x00400000);
  set(1, 0x7e000002, 0x00800002);
  CHECK(translation(0x7e000020).status == Status::Protection);
  cpu.spr_write(&cpu, 536, 0, 0x80004000);
  CHECK(translation(0x7e000020).physical == 0x00800020);
  CHECK(translation(0x7e020000).status == Status::Missing); // just outside BAT1
  // Broad blocks can cover the aperture even when BEPI starts outside it.
  set(2, 0x78000ffe, 0x00000002); // 128 MiB: 0x78000000..0x7fffffff
  CHECK(translation(0x7f000020).status == Status::Mapped);
  CHECK(translation(0x7f000020).physical == 0x07000020);
  set(2, 0x80000ffe, 0x00000002); // now excludes every VM address
  CHECK(translation(0x7f000020).status == Status::Missing);
  CHECK(translation(0x80000020).physical == 0x20); // other apertures keep BAT search
  cpu.msr &= ~0x10u;
  CHECK(translation(0x7e000020).physical == 0x7e000020); // DR off
  runtime->Reset();
  cpu.msr = 0x32;
  CHECK(translation(0x7e000020).status == Status::Missing);
}
static void VerifyLazyGuestRam() {
  GuestRam empty(0u);
  empty.Clear();
  CHECK(empty.empty() && empty.begin() == empty.end());
  GuestRam ram(3u * 4096u + 37u);
  auto* stable = ram.data();
#if defined(__linux__)
  const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
  std::vector<unsigned char> residency((ram.size()+page-1u)/page);
  CHECK(mincore(ram.data(), ram.size(), residency.data()) == 0);
  for (auto state : residency) CHECK((state & 1u) == 0u);
#endif
  ram[0] = 0xa5u; ram[4095] = 0xb6u; ram[4096] = 0xc7u;
  ram[ram.size()-1u] = 0xd8u;
  CHECK(ram[8192] == 0u);
  ram.Clear();
  CHECK(ram.data() == stable);
#if defined(__linux__)
  CHECK(mincore(ram.data(), ram.size(), residency.data()) == 0);
  for (auto state : residency) CHECK((state & 1u) == 0u);
#endif
  for (const auto byte : ram) CHECK(byte == 0u);
  ram[ram.size()-1u] = 0x59u;
  CHECK(stable[ram.size()-1u] == 0x59u);
  ram.Clear();
  CHECK(stable[ram.size()-1u] == 0u);
}

// v322: the generated-code paged-VM mirror is exactly the modeled DTLB.
static void VerifyPagedViewMirror() {
  constexpr unsigned ea = 0x7e123abc, segment = 0x005abcde, table = 0x10000;
  constexpr unsigned primary = 0x17f40, tag = 0xad5e6f38;
  constexpr unsigned index = (ea - 0x7e000000u) >> 12u, set = (ea >> 12u) & 0x3fu;
  auto runtime = std::make_unique<HostRuntime>();
  auto& cpu = runtime->Cpu();
  auto& ram = runtime->Memory();
  const auto& view = *ram.FlatVmemView();
  cpu.msr = 0x32;
  cpu.sr[7] = segment;
  cpu.spr_write(&cpu, 25, table, 0x80004000);
  CHECK(view.size == 0u && view.paged_pages && view.paged_mru);
  CHECK(view.paged_pages[index] == 0u);
  CHECK(ram.Write32(primary + 56, tag) && ram.Write32(primary + 60, 0x200002));
  CHECK(ram.Write32(0x200abc, 0x12345678));
  CHECK(cpu.external_read(&cpu, ea, 4) == 0x12345678);
  // Read fill: frame | read, no write permission until C is set.
  CHECK(view.paged_sr == segment && view.paged_pages[index] == 0x200001u);
  CHECK(view.paged_mru[set] == 0u);
  cpu.external_write(&cpu, ea, 0xaabbccdd, 4);
  CHECK(cpu.exception == 0 && view.paged_pages[index] == 0x200003u);
  // tlbie withdraws the translation; the next access republishes it.
  cpu.cache_control(&cpu, 4u, ea, 0x80004000);
  CHECK(view.paged_pages[index] == 0u);
  CHECK(cpu.external_read(&cpu, ea, 4) == 0xaabbccdd);
  CHECK(view.paged_pages[index] == 0x200003u);
  // A read-only page (pp=3) never gains the write bit.
  CHECK(ram.Write32(primary + 60, 0x200003));
  cpu.cache_control(&cpu, 4u, ea, 0x80004000);
  CHECK(cpu.external_read(&cpu, ea, 4) == 0xaabbccdd);
  CHECK(view.paged_pages[index] == 0x200001u);
  // User mode fills are not published (admission requires PR=0).
  CHECK(ram.Write32(primary + 60, 0x200002));
  cpu.cache_control(&cpu, 4u, ea, 0x80004000);
  cpu.msr |= 0x4000u;
  CHECK(cpu.external_read(&cpu, ea, 4) == 0xaabbccdd);
  CHECK(view.paged_pages[index] == 0u);
  cpu.msr &= ~0x4000u;
  CHECK(cpu.external_read(&cpu, ea, 4) == 0xaabbccdd);  // DTLB hit republishes
  CHECK(view.paged_pages[index] == 0x200001u);            // PTE rewritten with C=0
  // A supervisor BAT over the aperture takes precedence: mirror withdrawn.
  cpu.spr_write(&cpu, 536, 0x7e000002, 0x80004000);
  cpu.spr_write(&cpu, 537, 0x00400002, 0x80004000);
  CHECK(view.paged_pages[index] == 0u);
  cpu.spr_write(&cpu, 536, 0, 0x80004000);
  // A segment change makes every old mirror entry unusable.
  CHECK(cpu.external_read(&cpu, ea, 4) == 0xaabbccdd);
  CHECK(view.paged_pages[index] != 0u);
  runtime->Reset();
  CHECK(view.paged_pages[index] == 0u);
}

int main() {
  VerifyBatVmemAdmission();
  VerifyPagedViewMirror();
  VerifyLazyGuestRam();
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
  // The 24..32 MiB compatibility tail is physically backed like Dolphin's GC RAM aperture.
  CHECK(TranslatePage(memory, ea, segment, 0x1800000, false, PageAccess::Read).status != Status::InvalidTable);
  CHECK(TranslatePage(memory, ea, segment, 0x2000000, false, PageAccess::Read).status == Status::InvalidTable);
  CHECK(memory.Mem1().size() == AddressSpace::CompatMem1BackingSize);
  CHECK(AddressSpace::ReportedMem1Size() == 24u * 1024u * 1024u);
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
  // Gekko DTLB residency is architectural: replacing the backing PTE is not
  // visible until tlbie invalidates both ways of the selected set.
  CHECK(ram.Write32(primary + 60, 0x300002));
  CHECK(ram.Write32(0x300abc, 0xdeadbeef));
  CHECK(cpu.external_read(&cpu, ea, 4) == 0x12345678);
  cpu.cache_control(&cpu, 4u, ea, 0x80004000);
  CHECK(cpu.external_read(&cpu, ea, 4) == 0xdeadbeef);
  CHECK(ram.Write32(primary + 60, 0x200002));
  cpu.cache_control(&cpu, 4u, ea, 0x80004000);
  CHECK(cpu.external_read(&cpu, ea, 4) == 0x12345678);

  // A resident read-only (C=0) DTLB entry cannot authorize the first store by
  // itself. Gekko re-walks the HPT to set C; if software removed the PTE
  // meanwhile, the store must fault instead of dirtying the stale frame.
  CHECK(ram.Write32(primary + 60, 0x200002));
  cpu.cache_control(&cpu, 4u, ea, 0x80004000);
  CHECK(cpu.external_read(&cpu, ea, 4) == 0x12345678);
  CHECK(ram.Read32(primary + 60, &lower) && (lower & 0x80u) == 0u);
  CHECK(ram.Write32(primary + 56, 0));
  cpu.pc = 0x80004000;
  cpu.exception = 0;
  cpu.external_write(&cpu, ea, 0x55667788, 4);
  CHECK(cpu.pc == 0x300 && cpu.srr0 == 0x80004000 && (cpu.exception & 2u));
  CHECK(ram.Read32(0x200abc, &lower) && lower == 0x12345678);
  cpu.exception = 0;
  cpu.msr = 0x32;
  CHECK(ram.Write32(primary + 56, tag));
  cpu.cache_control(&cpu, 4u, ea, 0x80004000);

  cpu.external_write(&cpu, ea, 0xaabbccdd, 4);
  CHECK(ram.Read32(0x200abc, &lower) && lower == 0xaabbccdd);
  // v219 single-page VM fast path preserves BE scalar semantics for every
  // width accepted by the external memory ABI.
  cpu.external_write(&cpu, ea, 0x5au, 1);
  CHECK(cpu.external_read(&cpu, ea, 1) == 0x5au);
  cpu.external_write(&cpu, ea, 0x1122u, 2);
  CHECK(cpu.external_read(&cpu, ea, 2) == 0x1122u);
  cpu.external_write(&cpu, ea, 0x1122334455667788ull, 8);
  CHECK(cpu.external_read(&cpu, ea, 8) == 0x1122334455667788ull);
  cpu.external_write(&cpu, ea, 0xaabbccddu, 4);
  CHECK(cpu.external_pointer(&cpu, ea, 4) == nullptr); // no unsafe HLE pointer
  // Native strstr reads the same mapped pages as guest loads without exposing
  // a contiguous host pointer for virtual memory.
  CHECK(ram.Write32(0x200abc, 0x61626300));
  CHECK(ram.Write32(0x40000, 0x62630000));
  cpu.pc=0x80005000; cpu.lr=0x80006000; cpu.gpr[3]=ea; cpu.gpr[4]=0x80040000;
  CHECK(HardwareTestAccess::Strstr(*runtime,cpu.pc));
  CHECK(cpu.gpr[3]==ea+1 && cpu.pc==cpu.lr);
  // v300: native memory helpers use the same per-page DTLB view as guest
  // loads/stores (memcpy into, memcmp/memchr across MEM1 and a VM page).
  CHECK(ram.Write32(0x41000, 0x01020304) && ram.Write32(0x41004, 0x05060708));
  cpu.pc=0x80005000; cpu.gpr[3]=ea; cpu.gpr[4]=0x80041000; cpu.gpr[5]=8;
  CHECK(HardwareTestAccess::Simple(*runtime,SimpleHleKind::Memcpy,cpu.pc));
  CHECK(cpu.gpr[3]==ea && cpu.pc==cpu.lr && cpu.exception==0);
  CHECK(ram.Read32(0x200abc,&lower) && lower==0x01020304);
  CHECK(ram.Read32(0x200ac0,&lower) && lower==0x05060708);
  cpu.pc=0x80005000; cpu.gpr[3]=ea; cpu.gpr[4]=0x80041000; cpu.gpr[5]=8;
  CHECK(HardwareTestAccess::Simple(*runtime,SimpleHleKind::Memcmp,cpu.pc) && cpu.gpr[3]==0);
  CHECK(ram.Write32(0x41004, 0x05060709));
  cpu.pc=0x80005000; cpu.gpr[3]=ea; cpu.gpr[4]=0x80041000; cpu.gpr[5]=8;
  CHECK(HardwareTestAccess::Simple(*runtime,SimpleHleKind::Memcmp,cpu.pc) &&
        static_cast<std::int32_t>(cpu.gpr[3])==-1);
  cpu.pc=0x80005000; cpu.gpr[3]=ea; cpu.gpr[4]=7; cpu.gpr[5]=8;
  CHECK(HardwareTestAccess::Simple(*runtime,SimpleHleKind::Memchr,cpu.pc) && cpu.gpr[3]==ea+6);
  // v301 bounded string helpers across MEM1 and a VM page: "abc" lives at ea.
  CHECK(ram.Write32(0x200abc, 0x61626300) && ram.Write32(0x41000, 0x61627a00));
  cpu.pc=0x80005000; cpu.gpr[3]=ea; cpu.gpr[4]=0x80041000; cpu.gpr[5]=2;
  CHECK(HardwareTestAccess::Simple(*runtime,SimpleHleKind::Strncmp,cpu.pc) && cpu.gpr[3]==0);
  cpu.pc=0x80005000; cpu.gpr[3]=ea; cpu.gpr[4]=0x80041000; cpu.gpr[5]=8;
  CHECK(HardwareTestAccess::Simple(*runtime,SimpleHleKind::Strncmp,cpu.pc) &&
        static_cast<std::int32_t>(cpu.gpr[3])==int('c')-int('z'));
  cpu.pc=0x80005000; cpu.gpr[3]=ea; cpu.gpr[4]=2;
  CHECK(HardwareTestAccess::Simple(*runtime,SimpleHleKind::Strnlen,cpu.pc) && cpu.gpr[3]==2);
  cpu.pc=0x80005000; cpu.gpr[3]=ea; cpu.gpr[4]=64;
  CHECK(HardwareTestAccess::Simple(*runtime,SimpleHleKind::Strnlen,cpu.pc) && cpu.gpr[3]==3);
  CHECK(ram.Write32(0x41100, 0xeeeeeeee) && ram.Write32(0x41104, 0xeeeeeeee));
  cpu.pc=0x80005000; cpu.gpr[3]=0x80041100; cpu.gpr[4]=ea; cpu.gpr[5]=6;
  CHECK(HardwareTestAccess::Simple(*runtime,SimpleHleKind::Strncpy,cpu.pc) && cpu.gpr[3]==0x80041100);
  CHECK(ram.Read32(0x41100,&lower) && lower==0x61626300);
  CHECK(ram.Read32(0x41104,&lower) && lower==0x0000eeee);
  CHECK(ram.Write32(0x41100, 0x78790000));
  cpu.pc=0x80005000; cpu.gpr[3]=0x80041100; cpu.gpr[4]=ea; cpu.gpr[5]=2;
  CHECK(HardwareTestAccess::Simple(*runtime,SimpleHleKind::Strncat,cpu.pc) && cpu.gpr[3]==0x80041100);
  CHECK(ram.Read32(0x41100,&lower) && lower==0x78796162);
  CHECK(ram.Read32(0x41104,&lower) && lower==0x0000eeee);
  // v300 native PADRead: retail PADStatus[4] layout. A port is either absent
  // (PAD_ERR_NO_CONTROLLER = -1, all fields cleared) or OK; the host keyboard
  // may legitimately occupy port 0, so only the encoding contract is checked.
  for (std::uint32_t i = 0; i < 48u; i += 4u) CHECK(ram.Write32(0x42000u + i, 0xa5a5a5a5u));
  cpu.pc=0x80005000; cpu.gpr[3]=0x80042000;
  CHECK(HardwareTestAccess::Simple(*runtime,SimpleHleKind::PadRead,cpu.pc));
  CHECK((cpu.gpr[3] & 0x0fffffffu)==0u && cpu.pc==cpu.lr && cpu.exception==0);
  for (std::uint32_t port = 0; port < 4u; ++port) {
    std::uint32_t w0=0, w1=0, w2=0;
    CHECK(ram.Read32(0x42000u + port * 12u, &w0) && ram.Read32(0x42004u + port * 12u, &w1) &&
          ram.Read32(0x42008u + port * 12u, &w2));
    const std::uint32_t err = (w2 >> 8) & 0xffu;
    CHECK((w2 & 0xffu) == 0u && (err == 0u || err == 0xffu));
    if (err == 0xffu) CHECK(w0 == 0u && w1 == 0u && w2 == 0x0000ff00u);
  }
  CHECK(ram.Write32(0x200abc, 0x61626300));
  CHECK(ram.Write32(primary + 56, 0));
  cpu.cache_control(&cpu, 4u, ea, 0x80005000);
  cpu.pc=0x80005000; cpu.gpr[3]=ea; cpu.gpr[4]=0x80040000;
  // v301: a non-resident VM operand declines the service without raising or
  // modifying anything; the region probe then admits exactly this entry once
  // so the original guest body runs and faults page by page.
  const auto region_probe=[&](std::uint32_t start,std::uint32_t end) {
    cpu.external_addr=start; cpu.external_value=end; cpu.external_rid=0;
    const bool blocked=cpu.host_call(&cpu,0xfffffffcu);
    return std::pair<bool,unsigned>(blocked,cpu.external_rid);
  };
  CHECK(!HardwareTestAccess::Strstr(*runtime,cpu.pc));
  CHECK(cpu.pc==0x80005000 && cpu.gpr[3]==ea && cpu.exception==0);
  CHECK(HardwareTestAccess::DeclineEntry(*runtime)==0x80005000);
  CHECK(region_probe(0x80006000,0x80007000)==std::make_pair(false,0xffu));
  CHECK(HardwareTestAccess::DeclineEntry(*runtime)==0x80005000);  // other region
  CHECK(region_probe(0x80004f00,0x80006000)==std::make_pair(false,0xffu));
  CHECK(HardwareTestAccess::DeclineEntry(*runtime)==0u);           // consumed once
  // v301: memset on the unmapped page writes nothing and leaves r3-r5 intact.
  cpu.pc=0x80005000; cpu.gpr[3]=ea; cpu.gpr[4]=0xff; cpu.gpr[5]=4;
  CHECK(!HardwareTestAccess::Simple(*runtime,SimpleHleKind::Memset,cpu.pc));
  CHECK(cpu.pc==0x80005000 && cpu.gpr[3]==ea && cpu.gpr[5]==4 && cpu.exception==0);
  CHECK(ram.Read32(0x200abc,&lower) && lower==0x61626300);
  CHECK(region_probe(0x80004f00,0x80006000)==std::make_pair(false,0xffu));
  // An empty needle does not dereference an unmapped haystack.
  CHECK(ram.Write32(0x40000,0));
  cpu.pc=0x80005000; cpu.gpr[3]=ea; cpu.gpr[4]=0x80040000;
  CHECK(HardwareTestAccess::Strstr(*runtime,cpu.pc));
  CHECK(cpu.pc==cpu.lr && cpu.gpr[3]==ea && cpu.exception==0);
  CHECK(ram.Write32(0x200abc,0xaabbccdd));
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
  cpu.cache_control(&cpu, 4u, ea, 0x80004000);
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
  cpu.cache_control(&cpu, 4u, 0x7e124000, 0x80004000);
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
  CHECK(ram.Mem1().size() == AddressSpace::CompatMem1BackingSize);
  CHECK(AddressSpace::ReportedMem1Size() == 0x1800000u);
  std::cout << "VM hashed pages/protection/history/bounds/DSI/RFI tests passed\n";
}
