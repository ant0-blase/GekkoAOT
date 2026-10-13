// SPDX-License-Identifier: GPL-3.0-or-later
#include "NativeHW/native_hw.h"
#include "NativeHW/DI/native_di.h"
#include "NativeHW/SI/native_si.h"
#include "NativeHW/EXI/native_exi.h"
#include "NativeHW/EXI/native_card.h"
#include "NativeHW/AI/native_ai.h"
#include "NativeHW/PI/native_pi.h"
#include "NativeHW/MI/native_mi.h"
#include "NativeHW/CP/native_cp.h"
#include "NativeHW/DSP/native_dsp.h"
#include "NativeHW/events.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

namespace {
void SetFlag(const char* name, const char* value) {
#if defined(_WIN32)
  CHECK(_putenv_s(name, value ? value : "") == 0);
#else
  CHECK((value ? setenv(name, value, 1) : unsetenv(name)) == 0);
#endif
}

void TestDefaultPolicy() {
  using GekkoAOT::NativeHW::Service;
  constexpr const char* flags[] = {
      "GEKKOAOT_NATIVE_HW", "GEKKOAOT_NATIVE_HW_SI", "GEKKOAOT_NATIVE_HW_DI",
      "GEKKOAOT_NATIVE_HW_EXI", "GEKKOAOT_NATIVE_HW_VI", "GEKKOAOT_NATIVE_HW_AI",
      "GEKKOAOT_NATIVE_HW_PI", "GEKKOAOT_NATIVE_HW_MI", "GEKKOAOT_NATIVE_HW_CP",
      "GEKKOAOT_NATIVE_HW_DSP", "GEKKOAOT_NATIVE_HW_STRICT"};
  std::vector<std::optional<std::string>> previous;
  for (const char* name : flags) {
    const char* value = std::getenv(name);
    previous.emplace_back(value ? std::optional<std::string>(value) : std::nullopt);
    SetFlag(name, nullptr);
  }

  // No shell environment or saved configuration: all nine native services.
  const Service fresh;
  CHECK(fresh.Enabled() && fresh.FullNative());
  CHECK(fresh.ActiveComponentCount() == 9u && !fresh.Strict());
  SetFlag(flags[0], "");
  CHECK(Service().FullNative());

  // The legacy profile remains selectable explicitly, even with strict set.
  SetFlag(flags[10], "1");
  for (const char* value : {"0", "false", "off", "no"}) {
    SetFlag(flags[0], value);
    const Service disabled;
    CHECK(!disabled.Enabled() && !disabled.FullNative() && !disabled.Strict());
    CHECK(disabled.ActiveComponentCount() == 0u);
  }
  SetFlag(flags[0], "1");
  CHECK(Service().FullNative() && Service().Strict());
  SetFlag(flags[10], nullptr);
  SetFlag(flags[0], nullptr);

  // A single device opt-out must not disable any of the other services.
  using Device = bool (Service::*)() const;
  constexpr Device devices[] = {&Service::SI, &Service::DI, &Service::EXI,
      &Service::VI, &Service::AI, &Service::PI, &Service::MI, &Service::CP,
      &Service::DSP};
  for (unsigned i = 0; i < 9; ++i) {
    SetFlag(flags[i + 1], "0");
    const Service selected;
    CHECK(selected.Enabled() && !selected.FullNative());
    CHECK(selected.ActiveComponentCount() == 8u);
    CHECK(!(selected.*devices[i])());
    SetFlag(flags[i + 1], nullptr);
  }
  for (unsigned i = 0; i < previous.size(); ++i)
    SetFlag(flags[i], previous[i] ? previous[i]->c_str() : nullptr);
}
}

int main() {
  TestDefaultPolicy();
  using GekkoAOT::NativeHW::Config;
  using GekkoAOT::NativeHW::Service;
  const Service disabled(Config{false, true, true, true, true, true, true, true, true, true});
  CHECK(!disabled.SI() && !disabled.DI() && !disabled.EXI() && !disabled.VI() &&
        !disabled.AI() && !disabled.PI() && !disabled.MI() && !disabled.CP() && !disabled.DSP());
  const Service selected(Config{true, true, false, true, true, true, true, true, true, true});
  CHECK(selected.SI() && !selected.DI() && selected.EXI() && selected.VI() &&
        selected.AI() && selected.PI() && selected.MI() && selected.CP() && selected.DSP());
  CHECK(!selected.FullNative());
  CHECK(selected.ActiveComponentCount() == 8u);
  const Service full(Config{true, true, true, true, true, true, true, true, true, true});
  CHECK(full.FullNative());
  CHECK(full.ActiveComponentCount() == 9u);
  CHECK(Service::PreservesV4Timing());
  CHECK(Service::PreservesCpBreakpoints());
  CHECK(GekkoAOT::NativeHW::CP::SafeMaxBatchBursts() == 128u);
  CHECK(GekkoAOT::NativeHW::CP::PreserveGuestBreakpoints());
  CHECK(!GekkoAOT::NativeHW::SI::RefreshOnTransfer(false, true));
  CHECK(GekkoAOT::NativeHW::SI::RefreshOnTransfer(true, true));
  CHECK(GekkoAOT::NativeHW::SI::NeedsPeriodicHostPoll(false, true));
  CHECK(GekkoAOT::NativeHW::SI::NeedsPeriodicHostPoll(true, true));
  constexpr auto si_interval =
      GekkoAOT::NativeHW::SI::PollIntervalCycles(486000000u, 120u);
  CHECK(si_interval == 4050000u);
  CHECK(GekkoAOT::NativeHW::SI::CyclesUntilPoll(true, 0u, si_interval) == si_interval);
  CHECK(GekkoAOT::NativeHW::SI::CyclesUntilPoll(true, 100u, si_interval) == si_interval - 100u);
  CHECK(!GekkoAOT::NativeHW::DI::PreferVfs(false, false));
  CHECK(GekkoAOT::NativeHW::DI::PreferVfs(true, false));
  CHECK(GekkoAOT::NativeHW::AI::UseHostTimedInterface(true));
  CHECK(GekkoAOT::NativeHW::AI::EffectiveAidRate(true, 48043u, true) == 32028u);
  CHECK(GekkoAOT::NativeHW::AI::EffectiveAidRate(true, 48043u, false) == 48043u);
  CHECK(GekkoAOT::NativeHW::AI::TimedIrqNeedsSync(true, true));
  CHECK(!GekkoAOT::NativeHW::AI::TimedIrqNeedsSync(true, false));
  CHECK(GekkoAOT::NativeHW::PI::UseHostInterruptAggregation(true));
  CHECK(GekkoAOT::NativeHW::MI::UseHostAddressSpace(true));
  CHECK(GekkoAOT::NativeHW::CP::CanDirectSubmit(true, true, true, true, false, 0u, true));
  CHECK(!GekkoAOT::NativeHW::CP::CanDirectSubmit(true, true, true, true, true, 0u, true));
  CHECK(!GekkoAOT::NativeHW::CP::CanDirectSubmit(true, true, true, true, false, 32u, true));
  CHECK(GekkoAOT::NativeHW::DSP::HostTaskBackendActive(true, true, false));
  CHECK(!GekkoAOT::NativeHW::DSP::HostTaskBackendActive(true, false, false));
  CHECK(GekkoAOT::NativeHW::DSP::TimedIrqNeedsSync(true, true));
  CHECK(!GekkoAOT::NativeHW::DSP::TimedIrqNeedsSync(false, true));
  {
    GekkoAOT::NativeHW::Events::Deadlines deadlines{};
    deadlines.vi = 900u;
    deadlines.vi_frame = 700u;
    deadlines.si = 250u;
    deadlines.ai = 1200u;
    deadlines.dsp = 300u;
    CHECK(GekkoAOT::NativeHW::Events::Earliest(deadlines) == 250u);
    CHECK(GekkoAOT::NativeHW::Events::ClampMaintenance(3000u, 2000u) == 2000u);
  }

  GekkoAOT::NativeHW::EXI::HostService exi;
  exi.Select(0, 2);
  std::uint32_t command = 0x20000000u; // IPL RTC/SRAM read command.
  CHECK(exi.Immediate(0, 2, 1, 4, &command));
  std::uint32_t rtc = 0;
  CHECK(exi.Immediate(0, 2, 0, 4, &rtc));
  CHECK(rtc != 0);
  exi.Select(0, 0);
  exi.Select(0, 2);
  std::uint32_t sram_write = 0xa0000100u;
  CHECK(exi.Immediate(0, 2, 1, 4, &sram_write));
  std::uint32_t save_data = 0x11223344u;
  CHECK(exi.Immediate(0, 2, 1, 4, &save_data));
  exi.Select(0, 0);
  exi.Select(0, 2);
  std::uint32_t sram_read = 0x20000100u;
  CHECK(exi.Immediate(0, 2, 1, 4, &sram_read));
  std::uint32_t restored = 0;
  CHECK(exi.Immediate(0, 2, 0, 4, &restored));
  CHECK(restored == save_data);
  exi.Select(0, 0);
  CHECK(!exi.Immediate(0, 1, 1, 4, &command));
  exi.Select(0, 2);
  std::uint32_t dma_sram_command = 0x20000100u;
  CHECK(exi.Immediate(0, 2, 1, 4, &dma_sram_command));
  std::uint8_t sram_image[64]{};
  CHECK(exi.Dma(0, 2, 0, sram_image, sizeof(sram_image)));
  CHECK(sram_image[0] == 0x11u && sram_image[1] == 0x22u &&
        sram_image[2] == 0x33u && sram_image[3] == 0x44u);
  CHECK(!exi.Dma(0, 2, 0, sram_image, 1u));
  exi.Select(0, 0);

  // IPL data is read from a supplied asset, without executing its bootstrap.
  // Exercise the SDK's split command, DMA font reads and byte-stream wrapping.
  const auto rom_root = std::filesystem::temp_directory_path() /
                        "gekkoaot-native-exi-rom-test";
  std::filesystem::remove_all(rom_root);
  std::filesystem::create_directories(rom_root);
  const auto rom_path = rom_root / "IPL.bin";
  std::vector<std::uint8_t> rom_image(0x200000u);
  for (std::size_t i = 0; i < rom_image.size(); ++i)
    rom_image[i] = static_cast<std::uint8_t>((i * 13u + (i >> 8)) & 0xffu);
  {
    std::ofstream file(rom_path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(rom_image.data()), rom_image.size());
  }
#if defined(_WIN32)
  _putenv_s("GEKKOAOT_IPL_ROM", rom_path.string().c_str());
  _putenv_s("GEKKOAOT_USER_DIR", rom_root.string().c_str());
#else
  setenv("GEKKOAOT_IPL_ROM", rom_path.string().c_str(), 1);
  setenv("GEKKOAOT_USER_DIR", rom_root.string().c_str(), 1);
#endif
  {
    GekkoAOT::NativeHW::EXI::HostService rom;
    rom.Enable();
    rom.Select(0, 2);
    auto high = (0x1fcf00u << 6) & 0xffff0000u;
    auto low = (0x1fcf00u << 6) << 16;
    CHECK(rom.Immediate(0, 2, 1, 2, &high));
    CHECK(!rom.Immediate(0, 2, 0, 4, &high)); // Incomplete command.
    CHECK(rom.Immediate(0, 2, 1, 2, &low));
    std::uint8_t font[35]{};
    CHECK(rom.Dma(0, 2, 0, font, 32));
    CHECK(std::equal(font, font + 32, rom_image.begin() + 0x1fcf00u));
    std::uint32_t tail = 0;
    CHECK(rom.Immediate(0, 2, 0, 3, &tail));
    CHECK(tail == (std::uint32_t(rom_image[0x1fcf20]) << 24 |
                   std::uint32_t(rom_image[0x1fcf21]) << 16 |
                   std::uint32_t(rom_image[0x1fcf22]) << 8));
    rom.Select(0, 0); rom.Select(0, 2);
    std::uint32_t wrap = 0x1ffffeu << 6;
    CHECK(rom.Immediate(0, 2, 1, 4, &wrap));
    CHECK(rom.Dma(0, 2, 0, font, 4));
    CHECK(font[0] == rom_image[0x1ffffe] && font[1] == rom_image[0x1fffff] &&
          font[2] == rom_image[0] && font[3] == rom_image[1]);
    rom.Select(0, 0); rom.Select(0, 2);
    std::uint32_t write_rom = 0x80000000u;
    CHECK(!rom.Immediate(0, 2, 1, 4, &write_rom));
    CHECK(!rom.Dma(0, 2, 1, font, 4));
  }
#if defined(_WIN32)
  _putenv_s("GEKKOAOT_IPL_ROM", "");
  _putenv_s("GEKKOAOT_USER_DIR", "");
#else
  unsetenv("GEKKOAOT_IPL_ROM"); unsetenv("GEKKOAOT_USER_DIR");
#endif
  std::filesystem::remove_all(rom_root);

  const auto card_path = std::filesystem::temp_directory_path() /
                         "gekkoaot-native-hw-card-test.raw";
  std::filesystem::remove(card_path);
  {
    GekkoAOT::NativeHW::EXI::HostCard card(card_path, 0);
    CHECK(card.Ready());
    for (const auto byte : card.FlashId()) CHECK(byte == 0u);
    CHECK(std::filesystem::file_size(card_path) == 2u * 1024u * 1024u);
    card.Select(true);
    std::uint32_t id_command = 0x85000000u;
    CHECK(card.Immediate(1, 1, &id_command));
    std::uint32_t id = 0;
    CHECK(card.Immediate(0, 2, &id));
    CHECK(id == 0xc2c20000u);
    CHECK(card.Immediate(0, 1, &id));
    CHECK(id == 0x21000000u);
    CHECK(!card.Select(false));
    card.Select(true);
    std::uint32_t status_command = 0x83000000u;
    CHECK(card.Immediate(1, 1, &status_command));
    std::uint32_t status = 0;
    CHECK(card.Immediate(0, 1, &status));
    CHECK(status == 0x41000000u);
    CHECK(!card.Select(false));
    card.Select(true);
    std::uint32_t program = 0xf2005000u; // Page program, address 0xA000.
    CHECK(card.Immediate(1, 4, &program));
    std::uint32_t byte_address = 0;
    CHECK(card.Immediate(1, 1, &byte_address));
    std::uint32_t payload = 0xdeadbeefu;
    CHECK(card.Immediate(1, 4, &payload));
    CHECK(card.Select(false));
  }
  {
    std::ifstream file(card_path, std::ios::binary);
    std::vector<std::uint8_t> metadata(0x2000u * 5u);
    CHECK(file.read(reinterpret_cast<char*>(metadata.data()), metadata.size()));
    const auto valid_checksum = [&](std::size_t block, std::size_t first,
                                    std::size_t last, std::size_t at) {
      const auto* p = metadata.data() + block * 0x2000u;
      std::uint16_t sum = 0, inverse = 0;
      for (std::size_t i = first; i < last; i += 2) {
        const std::uint16_t word = (std::uint16_t(p[i]) << 8) | p[i + 1];
        sum = static_cast<std::uint16_t>(sum + word);
        inverse = static_cast<std::uint16_t>(inverse + (word ^ 0xffffu));
      }
      if (sum == 0xffffu) sum = 0;
      if (inverse == 0xffffu) inverse = 0;
      return p[at] == (sum >> 8) && p[at + 1] == (sum & 0xffu) &&
             p[at + 2] == (inverse >> 8) && p[at + 3] == (inverse & 0xffu);
    };
    CHECK(valid_checksum(0, 0, 0x1fc, 0x1fc));
    CHECK(valid_checksum(1, 0, 0x1ffc, 0x1ffc));
    CHECK(valid_checksum(2, 0, 0x1ffc, 0x1ffc));
    CHECK(valid_checksum(3, 4, 0x2000, 0));
    CHECK(valid_checksum(4, 4, 0x2000, 0));
    CHECK(metadata[0x22] == 0 && metadata[0x23] == 16);
  }
  {
    GekkoAOT::NativeHW::EXI::HostCard card(card_path, 0);
    CHECK(card.Ready());
    card.Select(true);
    std::uint32_t read = 0x52005000u;
    CHECK(card.Immediate(1, 4, &read));
    std::uint32_t byte_address = 0;
    CHECK(card.Immediate(1, 1, &byte_address));
    std::uint8_t restored_bytes[4]{};
    CHECK(card.Dma(0, restored_bytes, sizeof(restored_bytes)));
    CHECK(restored_bytes[0] == 0xde && restored_bytes[1] == 0xad &&
          restored_bytes[2] == 0xbe && restored_bytes[3] == 0xef);
    CHECK(!card.Select(false));
    card.Select(true);
    std::uint32_t dma_program = 0xf2005001u;
    CHECK(card.Immediate(1, 4, &dma_program));
    std::uint32_t dma_byte_address = 0;
    CHECK(card.Immediate(1, 1, &dma_byte_address));
    std::uint8_t page[128]{};
    page[0] = 0x42u;
    CHECK(card.Dma(1, page, sizeof(page)));
    CHECK(card.Select(false));
  }
  std::filesystem::remove(card_path);
  std::cout << "NativeHW policy and host RTC passed\n";
}
