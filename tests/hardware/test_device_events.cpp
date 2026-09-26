// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/pi/native_pi.h"
#include "hw/pe/native_pe.h"
#include "hw/vi/native_vi.h"
#include "native/address_space.h"
#include <cstdlib>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

int main() {
  using GekkoAOT::HW::PI::NativePI;
  NativePI pi;
  pi.SetInterrupt(NativePI::Video, false);
  pi.SetInterrupt(NativePI::CommandProcessor);
  CHECK(!pi.InterruptPending());
  CHECK(pi.Write(0xcc003004, NativePI::CommandProcessor, 4));
  CHECK(pi.InterruptPending());
  CHECK(pi.Write(0xcc003000, NativePI::CommandProcessor, 4));
  CHECK(!pi.InterruptPending());
  pi.SetInterrupt(NativePI::CommandProcessor); // still asserted device reestablishes level
  CHECK(pi.InterruptPending());
  CHECK(pi.Write(0xcc003004, 0, 4));
  CHECK(!pi.InterruptPending());

  GekkoAOT::HW::PE::NativePE pe;
  pe.SetToken(0x1234, true);
  pe.SetFinish();
  CHECK(!pe.TokenInterruptPending() && !pe.FinishInterruptPending());
  CHECK(pe.Write(0xcc00100a, 3, 2));
  CHECK(pe.TokenInterruptPending() && pe.FinishInterruptPending());
  CHECK(pe.Write(0xcc00100a, 7, 2)); // ACK token while retaining both enables
  CHECK(!pe.TokenInterruptPending() && pe.FinishInterruptPending());
  CHECK(pe.Token() == 0x1234);
  pe.SetToken(0xabcd, false);
  CHECK(pe.Token() == 0xabcd && !pe.TokenInterruptPending());
  CHECK(pe.Write(0xcc00100a, 11, 2));
  CHECK(!pe.FinishInterruptPending());
  std::uint64_t value = 0;
  CHECK(pe.Read(0xcc00100a, 2, &value) && value == 3);

  GekkoAOT::HW::VI::NativeVI vi;
  CHECK(vi.Write(0xcc002000, 2u << 4, 2)); // 2 active lines per field
  CHECK(vi.Write(0xcc00200c, 0, 4));
  CHECK(vi.Write(0xcc002010, 0, 4));
  CHECK(vi.Write(0xcc002034, 0, 4));
  CHECK(vi.Write(0xcc002006, 100, 2));     // half-line width
  CHECK(vi.Write(0xcc002030, 0x10020001, 4)); // IRQ at line 2, first half
  CHECK(vi.Write(0xcc002002, 1, 2));
  const auto quantum = vi.TicksPerHalfLine();
  vi.AdvanceCycles(quantum - 1);
  CHECK(vi.HalfLineCount() == 0 && !vi.InterruptPending());
  vi.AdvanceCycles(quantum + 1);
  CHECK(vi.HalfLineCount() == 2 && vi.InterruptPending());
  CHECK(vi.Write(0xcc002030, 0x1002, 2)); // clear status, keep target/mask
  CHECK(!vi.InterruptPending());
  vi.AdvanceCycles(6ull * quantum);
  CHECK(vi.ConsumeOddFieldBoundary() && vi.ConsumeEvenFieldBoundary() && vi.ConsumeFrameBoundary());
  CHECK(!vi.ConsumeFrameBoundary());
  CHECK(vi.Write(0xcc00201c, 0x10001000, 4));
  CHECK(vi.Write(0xcc002024, 0x00001800, 4));
  CHECK(vi.XfbAddressTop() == 0x20000 && vi.XfbAddressBottom() == 0x30000);

  GekkoAOT::Native::AddressSpace memory;
  CHECK(memory.Mem1().size() == GekkoAOT::Native::AddressSpace::CompatMem1BackingSize);
  CHECK(GekkoAOT::Native::AddressSpace::ReportedMem1Size() == 24u * 1024u * 1024u);
  CHECK(memory.Write32(0x817ffffc, 0x12345678));
  std::uint32_t word = 0;
  CHECK(memory.Read32(0xc17ffffc, &word) && word == 0x12345678);
  // Dolphin-compatible GC backing keeps the normally unpopulated 24..32 MiB
  // physical aperture addressable while lowmem still reports retail 24 MiB.
  CHECK(memory.Write32(0x81800000, 0xfeedbeef));
  CHECK(memory.Read32(0xc1800000, &word) && word == 0xfeedbeef);
  CHECK(memory.Resolve(0x81ffffff, 1) != nullptr);
  CHECK(memory.Resolve(0x81ffffff, 2) == nullptr);
  CHECK(memory.Resolve(0x82000000, 1) == nullptr);
  CHECK(memory.Resolve(0x7fffffff, 2) == nullptr);
  CHECK(memory.Resolve(0x7dffffff, 1) == nullptr);
  // The currently supported flat compatibility window is only a bounds test,
  // not a claim that SDK virtual-to-physical paging is implemented.
  CHECK(memory.Write32(0x7ffffffc, 0xabcdef01));
  CHECK(memory.Read32(0x7ffffffc, &word) && word == 0xabcdef01);
  std::cout << "PI/PE/VI event and MEM1 boundary tests passed\n";
}
