// SPDX-License-Identifier: GPL-3.0-or-later
#include "native/runtime.h"
#include <cstdlib>
#include <iostream>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
extern "C" std::uint32_t gekkoaot_test_gx_bytes();
extern "C" std::uint8_t gekkoaot_test_gx_byte(std::uint32_t);
namespace GekkoAOT::Native {
struct HardwareTestAccess {
  static bool Execute(HostRuntime& r, std::uint32_t pc) {
    std::uint64_t cycles = 0;
    return r.ExecuteLowMemoryInstruction(pc, &cycles);
  }
  static void ServiceCpBoundary(HostRuntime& r) {
    r.cp_.ServiceRegisterDrain();
    r.SyncNativeCPInterrupt();
  }
  static bool Faulted(const HostRuntime& r) { return r.fault_.kind != HostRuntime::FaultKind::None; }
  static bool LoadPositionMatrix(HostRuntime& r, std::uint32_t address) {
    return r.DispatchSimpleHle(SimpleHleKind::GxLoadPosMtxImm, address);
  }
  static std::uint32_t CpuGatherBytes(const HostRuntime& r) { return r.cpu_fifo_gather_size_; }
  static std::uint32_t GpuGatherBytes(const HostRuntime& r) { return r.gpu_fifo_gather_size_; }
  static std::uint32_t FirstPc(const HostRuntime& r) { return r.gpu_fifo_first_pc_; }
  static void EnableFifoIrqFence(HostRuntime& r) { r.fifo_irq_fence_enabled_ = true; }
};
}
using namespace GekkoAOT::Native;
static void Store(CPUState& cpu, unsigned address, std::uint64_t value, unsigned size, bool direct) {
  if (direct && (address & 0x3fffffe0u) == 0x0c008000u) {
    const CPUState before = cpu;
    const auto accepted = HostRuntime::NativeWgpipeTryServices().try_write(&cpu, address, value, size, cpu.pc);
    CHECK(std::memcmp(&before, &cpu, sizeof(cpu)) == 0);
    if (!accepted) HostRuntime::NativeHardwareServices().wgpipe_write(&cpu, address, value, size);
  } else cpu.external_write(&cpu, address, value, size);
}
static void VerifyFifoIrqRestart(const char* bridge, bool direct) {
  auto runtime = std::make_unique<HostRuntime>();
  auto& r = *runtime;
  auto& cpu = r.Cpu();
  CHECK(r.InitializeNativeGX(bridge));
  HardwareTestAccess::EnableFifoIrqFence(r);
  const auto write = [&](unsigned address, unsigned value, unsigned size = 2) {
    Store(cpu, address, value, size, direct);
  };
  const auto pair = [&](unsigned offset, unsigned value) {
    write(0xcc000000 + offset, value); write(0xcc000002 + offset, value >> 16);
  };
  pair(0x20, 0x90000); pair(0x24, 0x9007c);
  pair(0x28, 32); pair(0x2c, 0);
  pair(0x34, 0x90000); pair(0x38, 0x90000); pair(0x3c, 0x90000);
  write(0xcc00300c, 0x90000, 4); write(0xcc003010, 0x9007c, 4);
  write(0xcc003014, 0x90000, 4); write(0xcc003000, 0x800, 4);
  write(0xcc003004, 0x800, 4);
  write(0xcc000002, 0x17); // breakpoint blocks reader; overflow IRQ enabled
  cpu.pc = 0x80004000;
  CHECK(r.Memory().Write32(cpu.pc, 0x90640000)); // stw r3,0(r4), restartable
  cpu.msr = 0x3032; // EE off: IRQ cannot preempt the producer
  for (unsigned i = 0; i < 16; ++i) write(0xcc008000, 0x12345678, 4);
  CHECK(r.CommandProcessor().FifoReadWriteDistance() == 64);
  CHECK(r.ProcessorInterface().InterruptPending());
  for (unsigned i = 0; i < 7; ++i) write(0xcc008000, 0xaabbccdd, 4);
  CHECK(HardwareTestAccess::GpuGatherBytes(r) == 28);
  for (unsigned i = 0; i < 32; ++i) {
    cpu.gpr[i] = 0x1000u + i;
    cpu.fpr[i] = double(i) + 0.25;
    cpu.ps1[i] = double(i) + 0.75;
  }
  cpu.msr |= 0x8000;
  const auto saved_msr = cpu.msr;
  write(0xcc008000, 0x11223344, 4);
  CHECK(cpu.pc == 0x500 && cpu.srr0 == 0x80004000 && cpu.srr1 == saved_msr);
  CHECK(cpu.exception == 0x40 && (cpu.msr & 0x8000) == 0);
  CHECK(HardwareTestAccess::GpuGatherBytes(r) == 28);
  CHECK(r.ProcessorInterface().FifoWritePointer() == 0x90040);
  CHECK(r.CommandProcessor().FifoReadWriteDistance() == 64);
  for (unsigned i = 0; i < 32; ++i) {
    CHECK(cpu.gpr[i] == 0x1000u + i);
    CHECK(cpu.fpr[i] == double(i) + 0.25 && cpu.ps1[i] == double(i) + 0.75);
  }
  CHECK(r.Memory().Write32(0x500, 0x60000000));
  CHECK(HardwareTestAccess::Execute(r, 0x500));
  CHECK(cpu.exception == 0);
  const auto renderer_before = gekkoaot_test_gx_bytes();
  write(0xcc000002, 0x11); // handler releases reader, clears overflow enable
  CHECK(gekkoaot_test_gx_bytes() == renderer_before);
  HardwareTestAccess::ServiceCpBoundary(r);
  CHECK(gekkoaot_test_gx_bytes() == renderer_before + 64);
  cpu.pc = cpu.srr0; cpu.msr = cpu.srr1; // RFI resumes the unaccepted store
  write(0xcc008000, 0x11223344, 4);
  CHECK(HardwareTestAccess::GpuGatherBytes(r) == 0);
  CHECK(gekkoaot_test_gx_bytes() == renderer_before + 96);
  for (unsigned i = 0; i < 28; ++i)
    CHECK(gekkoaot_test_gx_byte(renderer_before + 64 + i) == ((0xaabbccddu >> (24 - (i % 4) * 8)) & 0xff));
  CHECK(gekkoaot_test_gx_byte(renderer_before + 92) == 0x11);
  CHECK(gekkoaot_test_gx_byte(renderer_before + 95) == 0x44);
  CHECK(!HardwareTestAccess::Faulted(r));

  // An SDK packet is retried at entry rather than replayed after a partial
  // header/matrix. The overflow fence must publish zero bytes in this case.
  pair(0x3c, r.CommandProcessor().FifoReadPointer());
  write(0xcc000002, 0x17);
  cpu.msr &= ~0x8000u;
  for (unsigned i = 0; i < 16; ++i) write(0xcc008000, 0, 4);
  cpu.pc = 0x80006000; cpu.lr = 0x80008000;
  cpu.gpr[3] = 0x800a0000; cpu.gpr[4] = 0;
  for (unsigned i = 0; i < 12; ++i) CHECK(r.Memory().Write32(cpu.gpr[3] + i * 4, 0x3f800000));
  cpu.msr |= 0x8000u;
  const auto pointer_before = r.ProcessorInterface().FifoWritePointer();
  CHECK(HardwareTestAccess::LoadPositionMatrix(r, cpu.pc));
  CHECK(cpu.pc == 0x500 && cpu.srr0 == 0x80006000 && cpu.exception == 0x40);
  CHECK(r.ProcessorInterface().FifoWritePointer() == pointer_before);
  CHECK(HardwareTestAccess::GpuGatherBytes(r) == 0);
  CHECK(HardwareTestAccess::Execute(r, 0x500));
  write(0xcc000002, 0x11);
  HardwareTestAccess::ServiceCpBoundary(r);
  cpu.pc = cpu.srr0; cpu.msr = cpu.srr1;
  CHECK(HardwareTestAccess::LoadPositionMatrix(r, cpu.pc));
  CHECK(cpu.pc == cpu.lr && HardwareTestAccess::GpuGatherBytes(r) == 21);
  CHECK(!HardwareTestAccess::Faulted(r));
}
static void RunAll(const char* bridge, bool direct) {
  auto runtime = std::make_unique<HostRuntime>();
  auto& r = *runtime;
  auto& cpu = r.Cpu();
  CHECK(r.InitializeNativeGX(bridge));
  const auto write = [&](unsigned address, unsigned value, unsigned size = 2) {
    Store(cpu, address, value, size, direct);
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
  HardwareTestAccess::ServiceCpBoundary(r);
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
  CHECK(switched->InitializeNativeGX(bridge));
  auto& switched_cpu = switched->Cpu();
  const auto switched_write = [&](unsigned address, unsigned value, unsigned size = 2) {
    Store(switched_cpu, address, value, size, direct);
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

  // SMB1 stores its position matrix in locked cache while recording a display
  // list. The native SDK leaf must read that aperture and write through the
  // selected CPU FIFO, including its partial final gather line.
  auto matrix_runtime = std::make_unique<HostRuntime>();
  CHECK(matrix_runtime->InitializeNativeGX(bridge));
  auto& matrix_cpu = matrix_runtime->Cpu();
  matrix_cpu.msr |= 0x2000u;
  Store(matrix_cpu, 0xcc00300c, 0x40000, 4, direct);
  Store(matrix_cpu, 0xcc003010, 0x4007c, 4, direct);
  Store(matrix_cpu, 0xcc003014, 0x40000, 4, direct);
  for (unsigned i = 0; i < 12; ++i)
    Store(matrix_cpu, 0xe0000000 + i * 4, 0x3f800000, 4, direct);
  matrix_cpu.gpr[3] = 0xe0000000;
  matrix_cpu.gpr[4] = 0;
  matrix_cpu.pc = 0x800e4580;
  matrix_cpu.lr = 0x800acd3c;
  CHECK(HardwareTestAccess::LoadPositionMatrix(*matrix_runtime, matrix_cpu.pc));
  CHECK(!HardwareTestAccess::Faulted(*matrix_runtime));
  CHECK(HardwareTestAccess::CpuGatherBytes(*matrix_runtime) == 21);
  CHECK(HardwareTestAccess::GpuGatherBytes(*matrix_runtime) == 0);
  std::uint32_t matrix_header = 0;
  CHECK(matrix_runtime->Memory().Read32(0x40000, &matrix_header));
  CHECK(matrix_header == 0x10000b00);
  std::uint8_t matrix_byte = 0;
  CHECK(matrix_runtime->Memory().Read8(0x40004, &matrix_byte));
  CHECK(matrix_byte == 0);
  for (unsigned i = 0; i < 11; ++i)
    Store(matrix_cpu, 0xcc008000, 0, 1, direct);
  CHECK(!HardwareTestAccess::Faulted(*matrix_runtime));
  CHECK(HardwareTestAccess::CpuGatherBytes(*matrix_runtime) == 0);
  CHECK(matrix_runtime->Memory().Read32(0x40020, &matrix_header));
  CHECK(matrix_header == 0x003f8000);
  // Rising Sun's GXRedirectWriteGatherPipe uses [0, 64 MiB] as a CPU-only
  // destination range for raw vertex data. It must never reach the GX parser.
  auto redirected = std::make_unique<HostRuntime>();
  CHECK(redirected->InitializeNativeGX(bridge));
  auto& redirected_cpu = redirected->Cpu();
  const auto redirected_write = [&](unsigned address, unsigned value) {
    Store(redirected_cpu, address, value, 4, direct);
  };
  redirected_write(0xcc00300c, 0);
  redirected_write(0xcc003010, 0x04000000);
  redirected_write(0xcc003014, 0x60000);
  CHECK(redirected->ProcessorInterface().FifoEnd() == 0x04000000);
  const auto gx_before = gekkoaot_test_gx_bytes();
  for (unsigned i = 0; i < 16; ++i)
    redirected_write(0xcc008000, 0x43e00000 + i);
  CHECK(!HardwareTestAccess::Faulted(*redirected));
  CHECK(gekkoaot_test_gx_bytes() == gx_before);
  CHECK(redirected->ProcessorInterface().FifoWritePointer() == 0x60040);
  CHECK(redirected->Memory().Read32(0x60000, &word) && word == 0x43e00000);
  CHECK(redirected->Memory().Read32(0x6003c, &word) && word == 0x43e0000f);

  // v219: 16-bit WGPIPE stores are common in retail vertex submission (CoD
  // Finest Hour is dominated by them).  A full line assembled from halfwords
  // must be byte-identical to the generic gather path.
  auto halfword = std::make_unique<HostRuntime>();
  CHECK(halfword->InitializeNativeGX(bridge));
  auto& halfword_cpu = halfword->Cpu();
  const auto halfword_write = [&](unsigned address, unsigned value, unsigned size = 2) {
    Store(halfword_cpu, address, value, size, direct);
  };
  const auto halfword_pair = [&](unsigned offset, unsigned value) {
    halfword_write(0xcc000000 + offset, value);
    halfword_write(0xcc000002 + offset, value >> 16);
  };
  halfword_pair(0x20, 0x70000); halfword_pair(0x24, 0x7007c);
  halfword_pair(0x34, 0x70000); halfword_pair(0x38, 0x70000);
  halfword_write(0xcc00300c, 0x70000, 4);
  halfword_write(0xcc003010, 0x7007c, 4);
  halfword_write(0xcc003014, 0x70000, 4);
  halfword_write(0xcc000002, 0x10);
  const auto halfword_before = gekkoaot_test_gx_bytes();
  for (unsigned i = 0; i < 16; ++i)
    halfword_write(0xcc008000, 0x1200u + i, 2);
  halfword_pair(0x3c, 0x70020);
  halfword_write(0xcc000002, 0x11);
  CHECK(!HardwareTestAccess::Faulted(*halfword));
  CHECK(gekkoaot_test_gx_bytes() == halfword_before + 32u);
  CHECK(gekkoaot_test_gx_byte(halfword_before + 0u) == 0x12u);
  CHECK(gekkoaot_test_gx_byte(halfword_before + 1u) == 0x00u);
  CHECK(gekkoaot_test_gx_byte(halfword_before + 30u) == 0x12u);
  CHECK(gekkoaot_test_gx_byte(halfword_before + 31u) == 0x0fu);

  // Mixed scalar widths include an unaligned 64-bit append and a store
  // crossing the 32-byte line. Both sides of the fast-path limit must preserve
  // byte order, one visibility epoch and the remaining staged bytes.
  auto mixed = std::make_unique<HostRuntime>();
  CHECK(mixed->InitializeNativeGX(bridge));
  auto& mixed_cpu = mixed->Cpu();
  const auto mw = [&](unsigned address, std::uint64_t value, unsigned size) {
    Store(mixed_cpu, address, value, size, direct);
  };
  const auto mp = [&](unsigned offset, unsigned value) {
    mw(0xcc000000 + offset, value, 2); mw(0xcc000002 + offset, value >> 16, 2);
  };
  mp(0x20, 0x80000); mp(0x24, 0x8007c);
  mp(0x34, 0x80000); mp(0x38, 0x80000);
  mw(0xcc00300c, 0x80000, 4); mw(0xcc003010, 0x8007c, 4);
  mw(0xcc003014, 0x80000, 4); mw(0xcc000002, 0x11, 2);
  const auto mixed_before = gekkoaot_test_gx_bytes();
  const auto epoch_before = mixed_cpu.external_write_count;
  unsigned byte = 0;
  for (const unsigned width : {1u, 2u, 4u, 8u, 8u, 8u, 8u}) {
    std::uint64_t value = 0;
    for (unsigned n=0; n<width; ++n) value = (value << 8u) | byte++;
    mw(0x8c00801fu, value, width);
    if (byte < 32) CHECK(mixed_cpu.external_write_count == epoch_before);
  }
  CHECK(gekkoaot_test_gx_bytes() == mixed_before + 32);
  for (unsigned n=0; n<32; ++n) CHECK(gekkoaot_test_gx_byte(mixed_before+n) == n);
  CHECK(HardwareTestAccess::GpuGatherBytes(*mixed) == 7);
  CHECK(mixed_cpu.external_write_count == epoch_before + 1);
  CHECK(!HardwareTestAccess::Faulted(*mixed));

  VerifyFifoIrqRestart(bridge, direct);
  std::cout << "WGPIPE visibility/CP stop/replay/WPAR/IRQ restart tests passed\n";
}

static void VerifyTryContract(const char* bridge) {
  auto r = std::make_unique<HostRuntime>();
  auto& cpu = r->Cpu();
  const auto tryWrite = HostRuntime::NativeWgpipeTryServices().try_write;
  CHECK(tryWrite(&cpu, 0xcc008000, 0x11223344, 4, 0x80004008) == 0); // no GPU
  CHECK(r->InitializeNativeGX(bridge));
  cpu.pc = 0x81200000; cpu.external_write_count = 254;
  const CPUState before = cpu;
  CHECK(tryWrite(&cpu, 0xcc008000, 0x11223344, 4, 0x80004008) == 1);
  CHECK(std::memcmp(&before, &cpu, sizeof(cpu)) == 0);
  CHECK(HardwareTestAccess::GpuGatherBytes(*r) == 4);
  CHECK(HardwareTestAccess::FirstPc(*r) == 0x80004008);
  CHECK(tryWrite(&cpu, 0x80010000, 0xaabbccdd, 4, 0x8000400c) == 0);
  CHECK(tryWrite(&cpu, 0xcc008000, 0xaabbccdd, 3, 0x8000400c) == 0);
  CHECK(tryWrite(&cpu, 0xcc008000, 0xc274d554, 4, 0x8000400c) == 0);
  CHECK(tryWrite(&cpu, 0xcc008000, 0x80000000, 4, 0x800228c0) == 0);
  CHECK(std::memcmp(&before, &cpu, sizeof(cpu)) == 0);
  CHECK(HardwareTestAccess::GpuGatherBytes(*r) == 4);
  cpu.exception = 0x40;
  CHECK(tryWrite(&cpu, 0xcc008000, 0x99, 1, 0x8000400c) == 0);
  CHECK(HardwareTestAccess::GpuGatherBytes(*r) == 4 && cpu.exception == 0x40);
}

int main(int argc, char** argv) {
  CHECK(argc == 2);
  VerifyTryContract(argv[1]);
  RunAll(argv[1], false);
  RunAll(argv[1], true);
}
