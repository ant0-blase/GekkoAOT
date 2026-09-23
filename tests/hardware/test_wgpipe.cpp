// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include <cstdlib>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
extern "C" std::uint32_t gekkoaot_test_gx_bytes();
extern "C" std::uint8_t gekkoaot_test_gx_byte(std::uint32_t);
namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static bool Execute(HostRuntime& r, std::uint32_t pc) {
    std::uint64_t cycles = 0;
    return r.ExecuteLowMemoryInstruction(pc, &cycles);
  }
  static bool Faulted(const HostRuntime& r) { return r.fault_.kind != HostRuntime::FaultKind::None; }
};
}
using namespace GekkoAOT::Native;
int main(int argc, char** argv) {
  CHECK(argc == 2);
  auto runtime = std::make_unique<HostRuntime>();
  auto& r = *runtime;
  auto& cpu = r.Cpu();
  CHECK(r.InitializeNativeGX(argv[1]));
  const auto write = [&](unsigned address, unsigned value, unsigned size = 2) {
    cpu.external_write(&cpu, address, value, size);
  };
  const auto pair = [&](unsigned offset, unsigned value) {
    write(0xcc000000 + offset, value); write(0xcc000002 + offset, value >> 16);
  };
  pair(0x20, 0x10000); pair(0x24, 0x1007c);
  pair(0x34, 0x10000); pair(0x38, 0x10000);
  write(0xcc00300c, 0x10000, 4); write(0xcc003010, 0x1007c, 4);
  write(0xcc003014, 0x10000, 4);
  write(0xcc000002, 0x10); // producer linked, GP stopped
  cpu.spr_write(&cpu, 921, 0x0c008000, 0x80004000);
  for (unsigned i = 0; i < 8; ++i) write(0xcc008000, 0x01020304, 4);
  CHECK(gekkoaot_test_gx_bytes() == 0);
  CHECK(r.CommandProcessor().FifoReadWriteDistance() == 32);
  pair(0x3c, 0x10020);
  write(0xcc000002, 0x33); // GP advances to breakpoint after one burst
  CHECK(gekkoaot_test_gx_bytes() == 32);
  CHECK(r.CommandProcessor().InterruptPending());
  for (unsigned i = 0; i < 8; ++i) write(0xcc008000, 0x05060708, 4);
  CHECK(gekkoaot_test_gx_bytes() == 32); // no execution past breakpoint
  CHECK(r.CommandProcessor().FifoReadWriteDistance() == 32);
  write(0xcc000002, 0x11);
  CHECK(gekkoaot_test_gx_bytes() == 64);
  CHECK(gekkoaot_test_gx_byte(32) == 5);
  std::uint32_t word = 0;
  CHECK(r.Memory().Read32(0x10020, &word) && word == 0x05060708);

  // A partial physical gather is retained across the PPCSync syscall vector.
  write(0xcc008000, 0xaabbccdd, 4);
  // WPAR.BNE reports outstanding bus transfers. The staged, incomplete line
  // is retained but does not itself keep BNE set; GXFlush can leave residual
  // padding that GXResetWriteGatherPipe later discards.
  CHECK((cpu.spr_read(&cpu, 921, 0x80004000) & 1) == 0);
  CHECK(r.Memory().Write32(0xc00, 0x60000000));
  CHECK(HardwareTestAccess::Execute(r, 0xc00));
  CHECK(gekkoaot_test_gx_bytes() == 64);
  CHECK((cpu.spr_read(&cpu, 921, 0x80004000) & 1) == 0);
  cpu.spr_write(&cpu, 921, 0x0c008000, 0x80004000); // discard, not flush
  CHECK((cpu.spr_read(&cpu, 921, 0x80004000) & 1) == 0);

  // Thirty-one command bytes plus GXFlush's fixed 32 zero bytes publish one
  // burst and leave 31 harmless padding bytes staged. The SDK's WPAR poll
  // must complete before the write pipe is reset to another FIFO target.
  for (unsigned i = 0; i < 31; ++i) write(0xcc008000, 0, 1);
  for (unsigned i = 0; i < 8; ++i) write(0xcc008000, 0, 4);
  CHECK(gekkoaot_test_gx_bytes() == 96);
  CHECK((cpu.spr_read(&cpu, 921, 0x80004000) & 1) == 0);
  cpu.spr_write(&cpu, 921, 0x0c008000, 0x80004000);
  for (unsigned i = 0; i < 8; ++i) write(0xcc008000, 0, 4);
  CHECK(gekkoaot_test_gx_bytes() == 128);
  for (unsigned i = 0; i < 8; ++i) write(0xcc008000, 0x11223344, 4);
  CHECK(gekkoaot_test_gx_bytes() == 160 && gekkoaot_test_gx_byte(128) == 0x11);
  CHECK((cpu.spr_read(&cpu, 921, 0x80004000) & 1) == 0);

  // An unlinked GP can consume a FIFO populated in RAM, with no CPU WG writes.
  write(0xcc000002, 0);
  for (unsigned i = 0; i < 8; ++i) CHECK(r.Memory().Write32(0x10060 + i * 4, 0x55667788));
  pair(0x38, 0x10060); pair(0x34, 0x10000); pair(0x30, 32);
  write(0xcc000002, 1);
  CHECK(gekkoaot_test_gx_bytes() == 192 && gekkoaot_test_gx_byte(160) == 0x55);
  CHECK(r.CommandProcessor().FifoReadPointer() == 0x10000);

  // GXEndDisplayList can switch PI back to the render FIFO while zero padding
  // from its GXFlush still occupies the same physical write-gather pipe. The
  // next writes complete that line at the newly selected destination.
  auto switched = std::make_unique<HostRuntime>();
  CHECK(switched->InitializeNativeGX(argv[1]));
  auto& switched_cpu = switched->Cpu();
  const auto switched_write = [&](unsigned address, unsigned value, unsigned size = 2) {
    switched_cpu.external_write(&switched_cpu, address, value, size);
  };
  const auto switched_pair = [&](unsigned offset, unsigned value) {
    switched_write(0xcc000000 + offset, value);
    switched_write(0xcc000002 + offset, value >> 16);
  };
  switched_pair(0x20, 0x20000); switched_pair(0x24, 0x2007c);
  switched_pair(0x34, 0x20000); switched_pair(0x38, 0x20000);
  switched_write(0xcc00300c, 0x30000, 4);
  switched_write(0xcc003010, 0x3007c, 4);
  switched_write(0xcc003014, 0x30000, 4);
  switched_write(0xcc000002, 0x10);
  for (unsigned i = 0; i < 8; ++i) switched_write(0xcc008000, 0, 4);
  for (unsigned i = 0; i < 7; ++i) switched_write(0xcc008000, 0, 1);
  switched_write(0xcc00300c, 0x20000, 4);
  switched_write(0xcc003010, 0x2007c, 4);
  switched_write(0xcc003014, 0x20000, 4);
  for (unsigned i = 0; i < 25; ++i) switched_write(0xcc008000, 7, 1);
  CHECK(!HardwareTestAccess::Faulted(*switched));
  for (unsigned i = 0; i < 32; ++i) {
    std::uint8_t byte = 0xff;
    CHECK(switched->Memory().Read8(0x20000 + i, &byte));
    CHECK(byte == (i < 7 ? 0 : 7));
  }
  switched_cpu.spr_write(&switched_cpu, 921, 0x0c008000, 0x80004000);
  for (unsigned i = 0; i < 5; ++i) switched_write(0xcc008000, 0x11, 1);
  switched_write(0xcc00300c, 0x30000, 4);
  switched_write(0xcc003010, 0x3007c, 4);
  switched_write(0xcc003014, 0x30020, 4);
  for (unsigned i = 0; i < 27; ++i) switched_write(0xcc008000, 0x22, 1);
  CHECK(!HardwareTestAccess::Faulted(*switched));
  for (unsigned i = 0; i < 32; ++i) {
    std::uint8_t byte = 0;
    CHECK(switched->Memory().Read8(0x30020 + i, &byte));
    CHECK(byte == (i < 5 ? 0x11 : 0x22));
  }
  std::cout << "WGPIPE visibility/CP stop/replay/WPAR tests passed\n";
}
