// SPDX-License-Identifier: GPL-3.0-or-later
#include "dsp/native/native_dsp.h"

#include <cstdlib>
#include <iostream>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

int main() {
  using GekkoAOT::DSP::NativeDSP;
  GekkoAOT::Native::AddressSpace memory;
  NativeDSP dsp(&memory);
  const auto write = [&](unsigned offset, unsigned value) {
    CHECK(dsp.Write(0xcc005000u + offset, value, 2));
  };
  const auto read = [&](unsigned offset) {
    std::uint64_t value = 0;
    CHECK(dsp.Read(0xcc005000u + offset, 2, &value));
    return static_cast<unsigned>(value);
  };
  const auto mail = [&](std::uint32_t value) {
    write(0, value >> 16); write(2, value & 0xffff);
    CHECK((read(0) & 0x8000) == 0);
  };
  const auto receive = [&] {
    CHECK(read(4) & 0x8000);
    return (read(4) << 16) | read(6);
  };

  // Synthetic vector layout only: no proprietary initialization program.
  CHECK(memory.Write16(0x81000000, 0x029f));
  CHECK(memory.Write16(0x81000002, 0x0010));
  for (unsigned v = 1; v < 8; ++v) {
    CHECK(memory.Write16(0x81000000 + v * 4, 0x029f));
    CHECK(memory.Write16(0x81000002 + v * 4, 0x0030 + v - 1));
  }
  write(0x0a, 0x0805); // reset with INIT and HALT preserved
  CHECK((read(0x0a) & 0x0805) == 0x0804);
  dsp.AdvanceCycles(1000000);
  CHECK(!dsp.DspMailboxPending());
  CHECK(!dsp.SdkBootstrapLoaded()); // reset does not perform an init DMA
  write(0x0a, 0x0004); // falling INIT edge performs the init DMA
  CHECK(dsp.SdkBootstrapLoaded());
  CHECK(!dsp.SdkBootstrapRunning());
  write(0x0a, 0x0000); // unhalt the initialization program
  CHECK(dsp.SdkBootstrapRunning());
  dsp.AdvanceCycles(1000000);
  CHECK(receive() == 0x80544348);

  // This buffer is ordinary game RAM again after initialization. A subsequent
  // task reset must use the ROM loader even though its contents are invalid.
  CHECK(memory.Write32(0x81000000, 0xdeadbeef));
  write(0x0a, 0x0951); // SDK DSPReset with HALT clear
  CHECK((read(0x0a) & 0x0805) == 0x0800);
  CHECK(!dsp.SdkBootstrapLoaded());
  CHECK(!dsp.DspMailboxPending()); // delivery at device execution boundary
  dsp.AdvanceCycles(1);
  CHECK(receive() == 0x8071feed);
  CHECK(!dsp.InterruptPending());
  dsp.AdvanceCycles(1000000);
  CHECK(!dsp.DspMailboxPending()); // exactly one ready message per reset

  // Synthetic instruction markers recognized by the existing native AX
  // backend. They exercise task activation without shipping a game ucode.
  CHECK(memory.Write32(0x80004000, 0x009fbabe));
  CHECK(memory.Write32(0x80004004, 0x16fcdcd1));
  CHECK(memory.Write16(0x80004008, 0x26fe));
  const auto load_task = [&] {
    mail(0x80f3a001); mail(0x80004000);
    mail(0x80f3c002); mail(0);
    mail(0x80f3a002); mail(0x100);
    mail(0x80f3b002); mail(0);
    mail(0x80f3d001); mail(0x10);
    CHECK(dsp.NativeAxHleActive());
    CHECK(dsp.SdkTaskInitPending());
    dsp.AdvanceCycles(1000000);
    CHECK(receive() == 0xdcd10000);
  };
  load_task();
  write(0x0a, 0x0951);
  CHECK(!dsp.NativeAxHleActive());
  CHECK(!dsp.SdkTaskInitPending());
  CHECK(dsp.SdkTaskLoaderStage() == 0);
  dsp.AdvanceCycles(1);
  CHECK(receive() == 0x8071feed);
  load_task(); // old AX must not consume the new loader's command packets

  write(0x0a, 0x0805);
  dsp.AdvanceCycles(1000000);
  CHECK(!dsp.DspMailboxPending());
  write(0x0a, 0x0800);
  dsp.AdvanceCycles(1);
  CHECK(receive() == 0x8071feed);
  std::cout << "DSP reset/init/task lifecycle passed\n";
}
