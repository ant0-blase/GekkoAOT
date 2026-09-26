#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/cpu_state.h"
#include "native/address_space.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace GekkoAOT::Native {

// Minimal GameCube BS2/IPL state needed when booting a DOL directly.  This is
// intentionally host-owned: the standalone runtime must not need Dolphin's
// Boot_BS2Emu merely to reach the SDK/game entrypoint.
struct GameCubeBootConfig {
  enum class VideoMode : std::uint8_t { Auto, Ntsc, Pal };

  VideoMode video_mode = VideoMode::Auto;
  std::uint64_t rtc_timebase_ticks = 0; // 0 => derive from host wall clock.
};

struct GameCubeBootResult {
  std::string game_id;
  bool ntsc = true;
  std::uint64_t rtc_timebase_ticks = 0;
};

struct GameCubeFstResult {
  std::uint32_t address = 0;
  std::uint32_t size = 0;
};

struct GameCubeBi2Result {
  std::uint32_t address = 0;
  std::uint32_t size = 0;
};

namespace BootDetail {
constexpr std::uint32_t kDiscMagic = 0xc2339f3du;
constexpr std::uint32_t kBootedFromBootrom = 0x0d15ea5eu;
constexpr std::uint32_t kLatestDevkit = 0x10000006u;
constexpr std::uint32_t kAramSize = 0x01000000u;
constexpr std::uint32_t kBusClockHz = 0x09a7ec80u;
constexpr std::uint32_t kCpuClockHz = 0x1cf7c580u;
constexpr std::uint32_t kRfi = 0x4c000064u;
constexpr std::uint32_t kInitialMsr = 0x00002032u;
constexpr std::uint32_t kInitialHid2 = 0xe0000000u;
constexpr std::uint64_t kTimebaseHz = 40'500'000ull;
constexpr std::uint64_t kGameCubeEpochUnix = 0x386d4380ull; // 2000-01-01 UTC.
constexpr std::uint32_t kMem1CachedBase = 0x80000000u;
constexpr std::uint32_t kMem1CachedEnd = 0x81800000u;

inline std::uint32_t ReadBe32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  if (offset > bytes.size() || 4u > bytes.size() - offset)
    throw std::runtime_error("boot.bin is truncated");
  return (std::uint32_t(bytes[offset]) << 24) |
         (std::uint32_t(bytes[offset + 1]) << 16) |
         (std::uint32_t(bytes[offset + 2]) << 8) |
         std::uint32_t(bytes[offset + 3]);
}

inline bool LooksLikeGameId(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 6) return false;
  for (std::size_t i = 0; i < 6; ++i) {
    const unsigned c = bytes[i];
    // Retail IDs are normally uppercase, but Datel products are known to use
    // a lowercase developer byte (for example GNHE5d). The disc header is
    // otherwise validated by magic, so accept ASCII alphanumerics here.
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9'))) return false;
  }
  return true;
}

inline bool NtscFromRegionCode(char code) {
  // Retail GC IDs use E (USA), J (Japan) and K (Korea) for NTSC regions.
  // All known European/Australian language/region codes are PAL. Unknown
  // synthetic DOL IDs stay NTSC unless the CLI explicitly selects PAL.
  switch (code) {
  case 'E': case 'J': case 'K': return true;
  case 'P': case 'D': case 'F': case 'I': case 'S': case 'H':
  case 'X': case 'Y': case 'L': case 'M': case 'U': case 'R': return false;
  default: return true;
  }
}

inline std::uint64_t HostRtcTicks() {
  using namespace std::chrono;
  const auto unix_seconds = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
  const std::uint64_t seconds_since_gc =
      unix_seconds > static_cast<std::int64_t>(kGameCubeEpochUnix)
          ? static_cast<std::uint64_t>(unix_seconds) - kGameCubeEpochUnix
          : 0ull;
  return seconds_since_gc * kTimebaseHz;
}

inline void RequireWrite(bool ok, const char* field) {
  if (!ok) throw std::runtime_error(std::string("failed to initialize GameCube low memory: ") + field);
}
} // namespace BootDetail

inline GameCubeBootResult InitializeGameCubeBoot(
    AddressSpace& memory, CPUState& cpu, std::span<const std::uint8_t> boot_bin,
    const GameCubeBootConfig& config = {}) {
  using namespace BootDetail;
  if (boot_bin.size() < 0x20)
    throw std::runtime_error("boot.bin must contain at least the 0x20-byte disc header");
  if (!LooksLikeGameId(boot_bin))
    throw std::runtime_error("boot.bin has an invalid six-character GameCube ID");
  if (ReadBe32(boot_bin, 0x1c) != kDiscMagic)
    throw std::runtime_error("boot.bin does not contain the GameCube disc magic");

  auto* disc_header = memory.Resolve(0x80000000u, 0x20u);
  if (!disc_header) throw std::runtime_error("MEM1 is unavailable for the GameCube disc header");
  std::copy_n(boot_bin.data(), 0x20u, disc_header);

  std::string game_id(reinterpret_cast<const char*>(boot_bin.data()), 6);
  bool ntsc = NtscFromRegionCode(game_id[3]);
  if (config.video_mode == GameCubeBootConfig::VideoMode::Ntsc) ntsc = true;
  if (config.video_mode == GameCubeBootConfig::VideoMode::Pal) ntsc = false;

  // Mirror the GameCube BS2 HLE state without pulling in Dolphin's boot core.
  RequireWrite(memory.Write32(0x80000020u, kBootedFromBootrom), "boot source");
  RequireWrite(memory.Write32(0x80000028u, AddressSpace::ReportedMem1Size()),
               "physical memory size");
  RequireWrite(memory.Write32(0x8000002cu, kLatestDevkit), "console type");
  RequireWrite(memory.Write32(0x800000ccu, ntsc ? 0u : 1u), "VI mode");
  RequireWrite(memory.Write32(0x800000d0u, kAramSize), "ARAM size");
  RequireWrite(memory.Write32(0x800000ecu, kMem1CachedEnd), "debug monitor address");
  RequireWrite(memory.Write32(0x800000f0u, AddressSpace::ReportedMem1Size()),
               "simulated memory size");
  RequireWrite(memory.Write32(0x800000f4u, 0u), "BI2 pointer");
  RequireWrite(memory.Write32(0x800000f8u, kBusClockHz), "bus clock");
  RequireWrite(memory.Write32(0x800000fcu, kCpuClockHz), "CPU clock");
  RequireWrite(memory.Write32(0x80000300u, kRfi), "DSI handler");
  RequireWrite(memory.Write32(0x80000800u, kRfi), "FPU handler");
  RequireWrite(memory.Write32(0x80000c00u, kRfi), "syscall handler");

  const std::uint64_t rtc_ticks =
      config.rtc_timebase_ticks ? config.rtc_timebase_ticks : HostRtcTicks();
  RequireWrite(memory.Write64(0x800030d8u, rtc_ticks), "RTC timebase preset");

  cpu.msr = kInitialMsr;
  cpu.hid2 = kInitialHid2;
  return {game_id, ntsc, rtc_ticks};
}

inline GameCubeFstResult InitializeGameCubeFst(
    AddressSpace& memory, std::span<const std::uint8_t> fst) {
  using namespace BootDetail;
  if (fst.size() < 12u) throw std::runtime_error("fst.bin is truncated");
  if (fst[0] != 1u || ReadBe32(fst, 4u) != 0u)
    throw std::runtime_error("fst.bin has an invalid root entry");
  const std::uint32_t entries = ReadBe32(fst, 8u);
  if (!entries || std::uint64_t(entries) * 12ull > fst.size())
    throw std::runtime_error("fst.bin has an invalid entry count");
  if (fst.size() > AddressSpace::ReportedMem1Size())
    throw std::runtime_error("fst.bin does not fit in retail MEM1");

  // The GC apploader places the FST at the top of MEM1 and lowers ArenaHi to
  // the start of that allocation.  Keep the same contract when we bypass the
  // apploader and launch the already-extracted DOL directly.
  const std::uint32_t aligned_size =
      static_cast<std::uint32_t>((fst.size() + 31u) & ~std::size_t(31u));
  const std::uint32_t address = (kMem1CachedEnd - aligned_size) & ~31u;
  if (address < kMem1CachedBase + 0x3100u)
    throw std::runtime_error("fst.bin leaves no usable MEM1 below ArenaHi");
  auto* destination = memory.Resolve(address, aligned_size);
  if (!destination) throw std::runtime_error("MEM1 is unavailable for fst.bin");
  std::copy(fst.begin(), fst.end(), destination);
  if (aligned_size > fst.size())
    std::fill(destination + fst.size(), destination + aligned_size, 0);

  RequireWrite(memory.Write32(0x80000024u, 1u), "apploader version");
  RequireWrite(memory.Write32(0x80000030u, 0u), "ArenaLo");
  RequireWrite(memory.Write32(0x80000034u, address), "ArenaHi");
  RequireWrite(memory.Write32(0x80000038u, address), "FST location");
  RequireWrite(memory.Write32(0x8000003cu, static_cast<std::uint32_t>(fst.size())),
               "FST maximum size");
  return {address, static_cast<std::uint32_t>(fst.size())};
}

inline GameCubeBi2Result InitializeGameCubeBi2(
    AddressSpace& memory, std::span<const std::uint8_t> bi2) {
  using namespace BootDetail;
  // Retail apploaders expose the BI2 debug block through low-memory word
  // 0x800000F4. Older SDK OSInit builds inspect its debug/pad fields before
  // completing DVD filesystem initialization. Standalone boot previously left
  // this apploader contract at zero, which changes early-SDK startup paths.
  if (bi2.size() < 0x28u) throw std::runtime_error("bi2.bin is truncated");
  if (bi2.size() > 0x20000u) throw std::runtime_error("bi2.bin is implausibly large");

  std::uint32_t upper = kMem1CachedEnd;
  std::uint32_t fst_address = 0;
  if (memory.Read32(0x80000038u, &fst_address) &&
      fst_address >= kMem1CachedBase && fst_address <= kMem1CachedEnd)
    upper = fst_address;

  const std::uint32_t aligned_size =
      static_cast<std::uint32_t>((bi2.size() + 31u) & ~std::size_t(31u));
  if (aligned_size > upper - kMem1CachedBase)
    throw std::runtime_error("bi2.bin does not fit in MEM1");
  const std::uint32_t address = (upper - aligned_size) & ~31u;
  if (address < kMem1CachedBase + 0x3100u)
    throw std::runtime_error("bi2.bin leaves no usable MEM1 below ArenaHi");

  auto* destination = memory.Resolve(address, aligned_size);
  if (!destination) throw std::runtime_error("MEM1 is unavailable for bi2.bin");
  std::copy(bi2.begin(), bi2.end(), destination);
  if (aligned_size > bi2.size())
    std::fill(destination + bi2.size(), destination + aligned_size, 0);

  RequireWrite(memory.Write32(0x800000f4u, address), "BI2 pointer");
  std::uint32_t arena_hi = 0;
  if (!memory.Read32(0x80000034u, &arena_hi) || arena_hi == 0u || address < arena_hi)
    RequireWrite(memory.Write32(0x80000034u, address), "ArenaHi below BI2");
  return {address, static_cast<std::uint32_t>(bi2.size())};
}

} // namespace GekkoAOT::Native
