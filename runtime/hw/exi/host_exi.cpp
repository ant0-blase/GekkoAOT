// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/exi/host_exi.h"
#include "native/atomic_file.h"
#include <cstring>
#include "hw/exi/ipl_font.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>

namespace GekkoAOT::NativeHW::EXI {
namespace {
constexpr std::uint32_t kSramBase = 0x800000u;
constexpr std::uint32_t kSramSize = 0x44u;
constexpr std::uint32_t kGcEpoch = 0x386d4380u;
constexpr std::uint32_t kRomSize = 0x200000u;
}

HostService::HostService() {
  // RTC occupies the first four bytes. The remaining 64 bytes are the
  // console's system SRAM, kept in a host file when a user directory exists.
  sram_[4] = 0x00; sram_[5] = 0x2c;
  sram_[6] = 0xff; sram_[7] = 0xd0;
  sram_[4 + 19] = 0x20;
}

HostService::~HostService() {
  if (sram_dirty_) Save();
}

void HostService::LoadRom() {
  // Only data is read from the user's IPL. Its executable bootstrap is never
  // run, and encrypted bootstrap bytes remain untouched. Header and compressed
  // fonts are outside the encrypted section.
  std::vector<std::filesystem::path> paths;
  if (const char* file = std::getenv("GEKKOAOT_IPL_ROM"); file && *file) {
    paths.emplace_back(file);
  } else {
    if (const char* dir = std::getenv("GEKKOAOT_USER_DIR"); dir && *dir)
      paths.emplace_back(std::filesystem::path(dir) / "GC" / "IPL.bin");
    const auto state = [] {
      if (const char* dir = std::getenv("GEKKOAOT_STATE_DIR"); dir && *dir)
        return std::filesystem::path(dir);
      return std::filesystem::current_path() / ".gekkoaot";
    }();
    paths.emplace_back(state / "GC" / "IPL.bin");
    for (const char* region : {"USA", "EUR", "JAP"})
      paths.emplace_back(state / "GC" / region / "IPL.bin");
#if !defined(_WIN32)
    std::filesystem::path data;
    if (const char* dir = std::getenv("XDG_DATA_HOME"); dir && *dir) data = dir;
    else if (const char* dir = std::getenv("HOME"); dir && *dir)
      data = std::filesystem::path(dir) / ".local" / "share";
    if (!data.empty())
      for (const char* region : {"USA", "EUR", "JAP"})
        paths.emplace_back(data / "dolphin-emu" / "GC" / region / "IPL.bin");
#endif
  }
  rom_.clear();
  for (const auto& path : paths) {
    std::error_code error;
    if (std::filesystem::file_size(path, error) != kRomSize || error) continue;
    std::ifstream file(path, std::ios::binary);
    if (!file) continue;
    rom_.resize(kRomSize);
    if (file.read(reinterpret_cast<char*>(rom_.data()), rom_.size())) {
      std::fprintf(stderr, "GEKKOAOT_NATIVE_EXI_ROM_V248=1 bytes=%u source=\"%s\" policy=read-only-data\n",
                   kRomSize, path.string().c_str());
      return;
    }
    rom_.clear();
  }
}

bool HostService::RomReadable(std::uint32_t address) {
  if (!rom_.empty() && !rom_replacement_) return true;
  // OSInitFont/OSLoadFont read the boot ROM font. Without a user IPL, build
  // GekkoAOT's replacement on first use; the rest of the ROM stays absent.
  if (address < IplFont::kAnsiRomOffset || address - IplFont::kAnsiRomOffset >= IplFont::kAnsiRomSize)
    return false;
  if (rom_.empty() && !rom_replacement_failed_) {
    if (const char* knob = std::getenv("GEKKOAOT_ROM_FONT"); knob && knob[0] == '0' && knob[1] == '\0') {
      rom_replacement_failed_ = true;
      return false;
    }
    rom_.assign(kRomSize, 0u);
    if (IplFont::InstallReplacementFonts(rom_)) {
      rom_replacement_ = true;
      std::fprintf(stderr, "GEKKOAOT_NATIVE_EXI_ROM_FONT_V366=1 source=gekkoaot-replacement encode=ansi policy=no-user-ipl\n");
    } else {
      rom_.clear();
      rom_replacement_failed_ = true;
    }
  }
  return rom_replacement_;
}

void HostService::Enable() {
  LoadRom();
  std::filesystem::path dir;
  if (const char* explicit_dir = std::getenv("GEKKOAOT_USER_DIR"); explicit_dir && *explicit_dir)
    dir = explicit_dir;
#if defined(_WIN32)
  else if (const char* app_data = std::getenv("LOCALAPPDATA"); app_data && *app_data)
    dir = std::filesystem::path(app_data) / "GekkoAOT";
#else
  else if (const char* data_home = std::getenv("XDG_DATA_HOME"); data_home && *data_home)
    dir = std::filesystem::path(data_home) / "gekkoaot";
  else if (const char* home_dir = std::getenv("HOME"); home_dir && *home_dir)
    dir = std::filesystem::path(home_dir) / ".local" / "share" / "gekkoaot";
#endif
  if (!dir.empty()) {
    save_path_ = dir / "SRAM.raw";
    { std::array<std::uint8_t,8> raw{}; std::ifstream clock(dir/"RTC.bias",std::ios::binary);
      if(clock.read(reinterpret_cast<char*>(raw.data()),raw.size())) {
        std::uint64_t value=0; for(auto byte:raw) value=(value<<8)|byte;
        rtc_bias_=static_cast<std::int64_t>(value);
      } }

    std::ifstream file(save_path_, std::ios::binary);
    if (file) file.read(reinterpret_cast<char*>(sram_.data() + 4), 64);
    cards_[0] = HostCard(dir / "cards" / "MemoryCardA.raw", 0);
    // One persistent host save slot is the default PC service. A second slot
    // is available for titles that explicitly need it, without presenting an
    // extra blank card to every game at boot.
    const char* second = std::getenv("GEKKOAOT_NATIVE_HW_CARD_B");
    std::error_code card_error;
    const bool existing_second = std::filesystem::is_regular_file(dir / "cards" / "MemoryCardB.raw", card_error);
    if (second ? (*second && *second != '0') : existing_second)
      cards_[1] = HostCard(dir / "cards" / "MemoryCardB.raw", 1);
    // The CARD SDK checks the SRAM flash ID and its checksum before reading
    // the card header. Derive both from the host file, as the console does
    // when a card is inserted. Offsets are in the 68-byte RTC + SRAM image.
    for (unsigned slot = 0; slot < cards_.size(); ++slot) {
      if (!cards_[slot].Ready()) continue;
      const auto id = cards_[slot].FlashId();
      std::uint8_t sum = 0;
      for (unsigned i = 0; i < id.size(); ++i) {
        sram_[24u + slot * 12u + i] = id[i];
        sum = static_cast<std::uint8_t>(sum + id[i]);
      }
      sram_[62u + slot] = sum ^ 0xffu;
    }
  }
  std::fprintf(stderr, "GEKKOAOT_NATIVE_EXI_HOST_V1=1 rtc=host sram=%u card_a=%u card_b=%u storage=host-files\n",
               save_path_.empty() ? 0u : 1u, cards_[0].Ready() ? 1u : 0u,
               cards_[1].Ready() ? 1u : 0u);
  initialized_ = true;
}

bool HostService::ReadRom(std::uint32_t offset, std::span<std::uint8_t> out) {
  if (offset > kRomSize || out.size() > kRomSize - offset) return false;
  if (out.empty()) return true;
  if (!RomReadable(offset) || !RomReadable(offset + out.size() - 1)) return false;
  std::copy_n(rom_.data() + offset, out.size(), out.data());
  return true;
}

bool HostService::Select(std::uint32_t channel, std::uint8_t chip_select) {
  const bool command_done = channel < cards_.size() &&
                            cards_[channel].Select(chip_select == 1u);
  if (channel != 0u) return command_done;
  if (selected_ == 2u && chip_select != 2u && sram_dirty_) {
    Save();
    sram_dirty_ = false;
  }
  selected_ = chip_select;
  command_valid_ = false;
  command_bytes_ = 0;
  command_ = 0;
  region_ = Region::None;
  cursor_ = 0;
  return command_done;
}

bool HostService::Present(std::uint32_t channel, std::uint8_t chip_select) const {
  return channel < cards_.size() && chip_select == 1u && cards_[channel].Ready();
}

bool HostService::Immediate(std::uint32_t channel, std::uint8_t chip_select,
                            std::uint8_t direction, std::uint8_t length,
                            std::uint32_t* data) {
  // Card slots are host-backed files on channels 0/1 CS1. IPL/SRAM is channel
  // 0 CS2. The IPL header/font data is a read-only host asset; debug UART and
  // unsupported commands retain the fallback.
  if (Present(channel, chip_select))
    return cards_[channel].Immediate(direction, length, data);
  if (channel != 0u || chip_select != 2u || !data || length == 0u || length > 4u)
    return false;
  if (selected_ != chip_select) Select(channel, chip_select);
  if (!command_valid_) {
    if (direction != 1u || length > 4u - command_bytes_) return false;
    for (std::uint8_t i = 0; i < length; ++i) {
      command_ = (command_ << 8) | ((*data >> (24u - i * 8u)) & 0xffu);
      ++command_bytes_;
    }
    if (command_bytes_ != 4u) return true;
    const std::uint32_t address = (command_ >> 6) & 0x1ffffffu;
    if (address >= kSramBase && address < kSramBase + kSramSize) {
      region_ = Region::Sram;
      cursor_ = address - kSramBase;
    } else if (address < kRomSize && !(command_ & 0x80000000u) && RomReadable(address)) {
      region_ = Region::Rom;
      cursor_ = address;
    } else {
      return false;
    }
    command_valid_ = true;
    if (region_ == Region::Rom) return true;
    const std::uint32_t rtc = Rtc();
    for (unsigned i = 0; i < 4; ++i) sram_[i] = static_cast<std::uint8_t>(rtc >> (24u - i * 8u));
    return true;
  }
  const bool write = (command_ & 0x80000000u) != 0u;
  if (direction != (write ? 1u : 0u)) return false;
  if (region_ == Region::Rom) {
    std::uint32_t word = 0;
    for (std::uint8_t i = 0; i < length; ++i)
      word |= std::uint32_t(rom_[cursor_++ & (kRomSize - 1u)]) << (24u - i * 8u);
    *data = word;
    return true;
  }
  std::uint32_t word = 0;
  bool changed = false;
  for (std::uint8_t i = 0; i < length; ++i) {
    const std::uint32_t offset = (cursor_++) % kSramSize;
    const std::uint8_t input = static_cast<std::uint8_t>(*data >> (24u - i * 8u));
    if (write) { sram_[offset] = input; if(offset>=4u) changed=true; }
    word |= static_cast<std::uint32_t>(sram_[offset]) << (24u - i * 8u);
  }
  if (!write) *data = word;
  if(write && cursor_==4u) { const std::uint32_t rtc=(std::uint32_t(sram_[0])<<24)|(std::uint32_t(sram_[1])<<16)|(std::uint32_t(sram_[2])<<8)|sram_[3]; if(!SetRtc(rtc)) return false; }
  if (changed) sram_dirty_ = true;
  return true;
}

bool HostService::Dma(std::uint32_t channel, std::uint8_t chip_select,
                      std::uint8_t direction, std::uint8_t* memory,
                      std::uint32_t length) {
  if (Present(channel, chip_select))
    return cards_[channel].Dma(direction, memory, length);
  if (channel != 0u || chip_select != 2u || !command_valid_ || !memory ||
      direction > 1u || length == 0u)
    return false;
  const bool write = (command_ & 0x80000000u) != 0u;
  if (direction != (write ? 1u : 0u)) return false;
  if (region_ == Region::Rom) {
    // Split at the ROM boundary, matching the immediate byte stream.
    std::uint32_t remaining = length;
    while (remaining) {
      const auto offset = cursor_ & (kRomSize - 1u);
      const auto count = std::min(remaining, kRomSize - offset);
      std::copy_n(rom_.data() + offset, count, memory);
      memory += count;
      cursor_ += count;
      remaining -= count;
    }
    return true;
  }
  if (region_ != Region::Sram || cursor_ > kSramSize || length > kSramSize - cursor_)
    return false;
  if (write) {
    for (std::uint32_t i = 0; i < length; ++i)
      if (cursor_ + i >= 4u) sram_[cursor_ + i] = memory[i];
    sram_dirty_ = true;
  } else {
    std::copy_n(sram_.data() + cursor_, length, memory);
  }
  cursor_ += length;
  return true;
}

bool HostService::Save() const {
  return !save_path_.empty() && GekkoAOT::Native::ReplaceFile(save_path_,Sram());
}

bool HostService::CommitSram(std::span<const std::uint8_t,64> bytes) {
  std::array<std::uint8_t,64> before{};
  std::copy_n(sram_.data()+4,64,before.data());
  std::copy(bytes.begin(),bytes.end(),sram_.begin()+4);
  if(!Save()) { std::copy(before.begin(),before.end(),sram_.begin()+4); return false; }
  sram_dirty_=false; return true;
}
std::uint32_t HostService::Rtc() const {
  const auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  return static_cast<std::uint32_t>(now-kGcEpoch+rtc_bias_);
}
bool HostService::SetRtc(std::uint32_t seconds) {
  const auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  const std::int64_t bias=std::int64_t(seconds)-(now-kGcEpoch);
  std::array<std::uint8_t,8> encoded{};
  for(unsigned i=0;i<8;++i) encoded[i]=static_cast<std::uint8_t>(std::uint64_t(bias)>>(56-i*8));
  auto path=save_path_; path.replace_filename("RTC.bias");
  if(save_path_.empty() || !GekkoAOT::Native::ReplaceFile(path,encoded)) return false;
  rtc_bias_=bias; return true;
}
} // namespace GekkoAOT::NativeHW::EXI
