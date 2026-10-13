// SPDX-License-Identifier: GPL-3.0-or-later
#include "dsp/native/native_dsp.h"

#include <cstdlib>
#include <iostream>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

static void CheckCompactAxParameterAbi() {
  using namespace GekkoAOT;
  Native::AddressSpace memory(0x1000u, 0u);
  DSP::NativeDSP dsp(&memory);
  DSP::NativeAXHLE ax(&dsp);
  // Independently authored markers and suffix: image hash 4e8a8b21,
  // CRC32 3918b1a2. This distinguishes the two identification algorithms.
  const std::uint8_t prefix[]={0x00,0x9f,0xba,0xbe,0x16,0xfc,0xdc,0xd1,0x26,0xfe};
  const std::uint8_t suffix[]={0xaf,0,0,3,1,1,0,6,3,0,0,0};
  for(unsigned i=0;i<sizeof(prefix);++i)CHECK(memory.Write8(0x80000400u+i,prefix[i]));
  for(unsigned i=0;i<sizeof(suffix);++i)CHECK(memory.Write8(0x80000400u+244u+i,suffix[i]));
  CHECK(ax.ProbeTask(0x80000400u,256u));
  CHECK(ax.UcodeCrc32()==0x3918b1a2u);
  // Put the 118-word PB exactly at the end of RAM. Reading the 122-word
  // newer ABI fails, whereas the compact ABI must process/write its update.
  constexpr std::uint32_t pb=0x80001000u-118u*2u;
  CHECK(memory.Write16(pb+34u*2u,1u));
  CHECK(memory.Write32(pb+39u*2u,0x80000700u));
  CHECK(memory.Write16(0x80000700u,97u)); // canonical DSP loop-counter offset
  CHECK(memory.Write16(0x80000702u,0x1234u));
  const std::uint16_t commands[]={2u,static_cast<std::uint16_t>(pb>>16),
      static_cast<std::uint16_t>(pb),3u,15u};
  for(unsigned i=0;i<sizeof(commands)/sizeof(commands[0]);++i)
    CHECK(memory.Write16(0x80000600u+i*2u,commands[i]));
  CHECK(ax.HandleCpuMail(0xbabe0000u|sizeof(commands)));
  CHECK(ax.HandleCpuMail(0x80000600u));
  std::uint16_t value=0;
  CHECK(memory.Read16(pb+93u*2u,&value));CHECK(value==0x1234u);
  CHECK(memory.Read16(pb+97u*2u,&value));CHECK(value==0u);
}

int main() {
  CheckCompactAxParameterAbi();
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
  const auto aram_dma = [&](std::uint32_t main_address,
                            std::uint32_t aram_address,
                            std::uint32_t bytes, bool aram_to_main) {
    CHECK((main_address & 31u) == 0u);
    CHECK((aram_address & 31u) == 0u);
    CHECK((bytes & 31u) == 0u);
    write(0x20, (main_address >> 16) & 0x03ffu);
    write(0x22, main_address & 0xffe0u);
    write(0x24, (aram_address >> 16) & 0x03ffu);
    write(0x26, aram_address & 0xffe0u);
    write(0x28, ((bytes >> 16) & 0x03ffu) | (aram_to_main ? 0x8000u : 0u));
    write(0x2a, bytes & 0xffe0u);
  };

  // GEKKOAOT_ARAM_HSP_NULL_V218: retail ARAM ends at 16 MiB. The upper
  // 48 MiB of the 26-bit AR DMA aperture belongs to the High Speed Port and
  // must not alias back onto onboard ARAM. Nintendo ARInit probes exactly this
  // distinction when detecting expansion RAM. With the default/null HSP, an
  // out-of-range write is discarded and an out-of-range read leaves MRAM
  // untouched.
  constexpr std::uint32_t dma_src = 0x80010000u;
  constexpr std::uint32_t dma_dst = 0x80010100u;
  for (std::uint32_t i = 0; i < 32u; ++i) {
    CHECK(memory.Write8(dma_src + i, static_cast<std::uint8_t>(0x80u + i)));
    CHECK(memory.Write8(dma_dst + i, 0x5au));
  }
  aram_dma(0x00010000u, NativeDSP::AramSize, 32u, false);
  dsp.AdvanceCycles(246u);
  aram_dma(0x00010100u, NativeDSP::AramSize, 32u, true);
  dsp.AdvanceCycles(246u);
  for (std::uint32_t i = 0; i < 32u; ++i) {
    std::uint8_t value = 0u;
    CHECK(memory.Read8(dma_dst + i, &value));
    CHECK(value == 0x5au);
  }

  // In-range ARAM DMA remains a normal round trip.
  aram_dma(0x00010000u, 0u, 32u, false);
  dsp.AdvanceCycles(246u);
  for (std::uint32_t i = 0; i < 32u; ++i) CHECK(memory.Write8(dma_dst + i, 0u));
  aram_dma(0x00010100u, 0u, 32u, true);
  dsp.AdvanceCycles(246u);
  for (std::uint32_t i = 0; i < 32u; ++i) {
    std::uint8_t value = 0u;
    CHECK(memory.Read8(dma_dst + i, &value));
    CHECK(value == static_cast<std::uint8_t>(0x80u + i));
  }
  // The DMA tests intentionally raised ARAM completion. Clear that W1C cause
  // while preserving the reset-time HALT+INIT state expected below.
  write(0x0a, 0x0824u);

  // Merely releasing HALT after host startup must not fabricate the ROM-loader
  // ready mailbox.  The ROM loader is entered by an explicit guest DSPReset;
  // otherwise this stale 0x8071feed mail can block OSInitAudioSystem.
  write(0x0a, 0x0800);
  dsp.AdvanceCycles(1);
  CHECK(!dsp.DspMailboxPending());

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
  // Independently authored light-protocol task marker: two immediate DSP
  // mailbox stores. Startup must remain polled, even with DSP IRQs enabled.
  CHECK(memory.Write32(0x80005000, 0x16fc8888));
  CHECK(memory.Write32(0x80005004, 0x16fd1111));
  write(0x0a, 0x0980); // clear old DSP cause, enable its mask, keep HALT clear
  mail(0x80f3a001); mail(0x80005000);
  mail(0x80f3c002); mail(0);
  mail(0x80f3a002); mail(0x20);
  mail(0x80f3b002); mail(0);
  mail(0x80f3d001); mail(0);
  dsp.AdvanceCycles(1000000);
  CHECK(receive() == 0x88881111);
  CHECK(!dsp.InterruptPending());
  // Setup and sync commands are streamed directly, without a count prefix.
  mail(0x81000001); mail(0x80006000); mail(0x80007000);
  mail(0x80008000); mail(0x80009000);
  dsp.AdvanceCycles(1000000);
  CHECK(receive() == 0x80000064);
  CHECK(!dsp.InterruptPending());
  CHECK(dsp.NativeJAudioHleActive());
  CHECK(memory.Write16(0x8000a000, 0x1234));
  mail(0x82014000); mail(0x8000a000); mail(0x8000b000);
  dsp.AdvanceCycles(1000000);
  CHECK(read(4) & 0x8000);
  (void)receive();
  std::uint16_t sample=0;
  CHECK(memory.Read16(0x8000a000, &sample) && sample == 0x1234);
  mail(0); // render only at the explicit sync boundary
  CHECK(!dsp.InterruptPending());
  dsp.AdvanceCycles(1000000);
  CHECK(dsp.InterruptPending());
  CHECK(!dsp.DspMailboxPending());
  CHECK(memory.Read16(0x8000a000, &sample) && sample == 0);
  write(0x0a, 0x0951);
  dsp.AdvanceCycles(1);
  CHECK(receive() == 0x8071feed);
  load_task(); // task reset must also drop light-protocol state
  std::cout << "DSP reset/init/task lifecycle passed\n";
}
