#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace GekkoAOT::Card {

constexpr std::uint32_t kBlockSize = 0x2000;
constexpr std::uint32_t kSystemBlocks = 5;
constexpr std::uint32_t kDefaultBlocks = 256;  // Memory Card 251 / 16 Mbit
constexpr std::uint32_t kDirA = 1 * kBlockSize;
constexpr std::uint32_t kDirB = 2 * kBlockSize;
constexpr std::uint32_t kFatA = 3 * kBlockSize;
constexpr std::uint32_t kFatB = 4 * kBlockSize;
constexpr std::size_t kDirEntries = 127;
constexpr std::size_t kDirEntrySize = 0x40;

inline std::uint16_t ReadBE16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}
inline std::uint32_t ReadBE32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) |
         (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
inline void WriteBE16(std::uint8_t* p, std::uint16_t value) {
  p[0] = static_cast<std::uint8_t>(value >> 8);
  p[1] = static_cast<std::uint8_t>(value);
}
inline void WriteBE32(std::uint8_t* p, std::uint32_t value) {
  p[0] = static_cast<std::uint8_t>(value >> 24);
  p[1] = static_cast<std::uint8_t>(value >> 16);
  p[2] = static_cast<std::uint8_t>(value >> 8);
  p[3] = static_cast<std::uint8_t>(value);
}

inline std::pair<std::uint16_t, std::uint16_t> Checksum(const std::uint8_t* data,
                                                         std::size_t bytes) {
  std::uint16_t sum = 0;
  std::uint16_t inverse = 0;
  for (std::size_t i = 0; i + 1 < bytes; i += 2) {
    const std::uint16_t value = ReadBE16(data + i);
    sum = static_cast<std::uint16_t>(sum + value);
    inverse = static_cast<std::uint16_t>(inverse + static_cast<std::uint16_t>(value ^ 0xffffu));
  }
  if (sum == 0xffffu) sum = 0;
  if (inverse == 0xffffu) inverse = 0;
  return {sum, inverse};
}

inline bool Newer(std::uint16_t lhs, std::uint16_t rhs) {
  return static_cast<std::int16_t>(lhs - rhs) > 0;
}

class Image {
  mutable std::mutex mutex_;
  std::filesystem::path path_;
  std::vector<std::uint8_t> data_;
  bool mounted_ = false;
  bool dirty_ = false;
  std::int32_t result_ = 0;

  static std::filesystem::path Root() {
    if (const char* root = std::getenv("GEKKOAOT_SAVE_ROOT"); root && *root)
      return std::filesystem::path(root);
    if (const char* root = std::getenv("GEKKOAOT_VFS_ROOT"); root && *root)
      return std::filesystem::path(root) / ".gekkoaot-saves";
    return std::filesystem::current_path() / ".gekkoaot-saves";
  }

  static void DirectoryChecksum(std::array<std::uint8_t, kBlockSize>& block) {
    const auto [sum, inverse] = Checksum(block.data(), kBlockSize - 4);
    WriteBE16(block.data() + 0x1ffc, sum);
    WriteBE16(block.data() + 0x1ffe, inverse);
  }

  static void FatChecksum(std::array<std::uint8_t, kBlockSize>& block) {
    const auto [sum, inverse] = Checksum(block.data() + 4, kBlockSize - 4);
    WriteBE16(block.data(), sum);
    WriteBE16(block.data() + 2, inverse);
  }

  bool FlushLocked() {
    if (!dirty_) return true;
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    if (ec) return false;
    const auto temporary = path_.string() + ".tmp";
    {
      std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      if (!file) return false;
      file.write(reinterpret_cast<const char*>(data_.data()), static_cast<std::streamsize>(data_.size()));
      if (!file) return false;
      file.flush();
      if (!file) return false;
    }
    std::filesystem::rename(temporary, path_, ec);
    if (ec) {
      ec.clear();
      std::filesystem::remove(path_, ec);
      ec.clear();
      std::filesystem::rename(temporary, path_, ec);
    }
    if (ec) return false;
    dirty_ = false;
    return true;
  }

  void FormatLocked(std::uint32_t blocks = kDefaultBlocks, std::uint16_t encoding = 0) {
    blocks = std::clamp<std::uint32_t>(blocks, 64, 2048);
    data_.assign(static_cast<std::size_t>(blocks) * kBlockSize, 0xff);

    // Match the Dolphin SDK system-block layout. CARDID::serial is 32 bytes,
    // but only the first 12 bytes are the flash-derived serial. The remaining
    // bytes are format time/SRAM/DTV metadata and must not contain random data.
    auto* header = data_.data();
    std::fill_n(header, 32, 0);
    const std::string seed = path_.string();
    std::uint32_t hash = 2166136261u;
    for (unsigned char c : seed) hash = (hash ^ c) * 16777619u;
    for (int i = 0; i < 12; ++i) {
      hash = hash * 1664525u + 1013904223u;
      header[i] = static_cast<std::uint8_t>(hash >> 24);
    }
    // serial[12..19] format time, serial[20..23] SRAM bias,
    // serial[24..27] language and serial[28..31] DTV status stay zero.
    WriteBE16(header + 0x20, 0);  // CARDID::deviceID
    const std::uint32_t mbit =
        static_cast<std::uint32_t>((data_.size() * 8u) / (1024u * 1024u));
    WriteBE16(header + 0x22, static_cast<std::uint16_t>(mbit));
    WriteBE16(header + 0x24, encoding);
    const auto [header_sum, header_inv] = Checksum(header, 0x1fc);
    WriteBE16(header + 0x1fc, header_sum);
    WriteBE16(header + 0x1fe, header_inv);

    // The SDK initializes the redundant directory/FAT copies with distinct
    // generation counters (0 and 1), not two byte-identical copies.
    for (std::uint16_t copy = 0; copy < 2; ++copy) {
      std::array<std::uint8_t, kBlockSize> directory{};
      directory.fill(0xff);
      WriteBE16(directory.data() + 0x1ffa, copy);
      DirectoryChecksum(directory);
      const auto address = copy == 0 ? kDirA : kDirB;
      std::copy(directory.begin(), directory.end(), data_.begin() + address);
    }

    for (std::uint16_t copy = 0; copy < 2; ++copy) {
      std::array<std::uint8_t, kBlockSize> fat{};
      fat.fill(0);
      WriteBE16(fat.data() + 0x04, copy);
      WriteBE16(fat.data() + 0x06,
                static_cast<std::uint16_t>(blocks - kSystemBlocks));
      WriteBE16(fat.data() + 0x08,
                static_cast<std::uint16_t>(kSystemBlocks - 1));
      FatChecksum(fat);
      const auto address = copy == 0 ? kFatA : kFatB;
      std::copy(fat.begin(), fat.end(), data_.begin() + address);
    }
    mounted_ = true;
    dirty_ = true;
    result_ = 0;
  }

  bool ReadBlockLocked(std::uint32_t address, std::array<std::uint8_t, kBlockSize>* out) const {
    if (!out || address > data_.size() || kBlockSize > data_.size() - address) return false;
    std::copy_n(data_.data() + address, kBlockSize, out->data());
    return true;
  }

public:
  bool Mount(unsigned slot) {
    std::lock_guard lock(mutex_);
    if (mounted_) return true;
    path_ = Root() / (slot == 0 ? "MemoryCardA.raw" : "MemoryCardB.raw");
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path_, exists_error);
    if (exists_error) { result_ = -5; return false; }
    std::ifstream file(path_, std::ios::binary);
    if (exists && !file) { result_ = -5; return false; }
    if (file) {
      file.seekg(0, std::ios::end);
      const auto size = file.tellg();
      file.seekg(0, std::ios::beg);
      if (size >= static_cast<std::streamoff>(64 * kBlockSize) &&
          size <= static_cast<std::streamoff>(2048 * kBlockSize) &&
          (size % kBlockSize) == 0) {
        data_.resize(static_cast<std::size_t>(size));
        file.read(reinterpret_cast<char*>(data_.data()), size);
        mounted_ = static_cast<bool>(file);
      }
    }
    if (!mounted_ && exists) {
      // A damaged or unreadable save must never be implicitly reformatted.
      data_.clear();
      result_ = -6;
      return false;
    }
    if (!mounted_) {
      FormatLocked();
      if (!FlushLocked()) { result_ = -5; return false; }
    }
    result_ = 0;
    return true;
  }

  void Unmount() {
    std::lock_guard lock(mutex_);
    FlushLocked();
    mounted_ = false;
    data_.clear();
    result_ = 0;
  }

  bool Format(unsigned slot, std::uint16_t encoding = 0) {
    std::lock_guard lock(mutex_);
    if (path_.empty())
      path_ = Root() / (slot == 0 ? "MemoryCardA.raw" : "MemoryCardB.raw");

    // Formatting is an explicit repair operation: unlike Mount(), it is
    // allowed to replace a corrupt image. Preserve a sane physical capacity
    // when one is already known instead of silently forcing every card to 16 Mbit.
    std::uint32_t blocks = kDefaultBlocks;
    if (data_.size() >= 64u * kBlockSize && data_.size() <= 2048u * kBlockSize &&
        (data_.size() % kBlockSize) == 0) {
      blocks = static_cast<std::uint32_t>(data_.size() / kBlockSize);
    } else {
      std::error_code ec;
      const auto bytes = std::filesystem::file_size(path_, ec);
      if (!ec && bytes >= 64u * kBlockSize && bytes <= 2048u * kBlockSize &&
          (bytes % kBlockSize) == 0)
        blocks = static_cast<std::uint32_t>(bytes / kBlockSize);
    }

    FormatLocked(blocks, encoding);
    const bool ok = FlushLocked();
    result_ = ok ? 0 : -5;
    return ok;
  }

  bool Read(std::uint32_t address, std::uint32_t size, std::uint8_t* output) const {
    std::lock_guard lock(mutex_);
    if (!mounted_ || !output || address > data_.size() || size > data_.size() - address) return false;
    std::copy_n(data_.data() + address, size, output);
    return true;
  }

  bool Write(std::uint32_t address, std::uint32_t size, const std::uint8_t* input) {
    std::lock_guard lock(mutex_);
    if (!mounted_ || !input || address > data_.size() || size > data_.size() - address) return false;
    std::copy_n(input, size, data_.data() + address);
    dirty_ = true;
    if (!FlushLocked()) { result_ = -5; return false; }
    result_ = 0;
    return true;
  }

  std::uint32_t Size() const {
    std::lock_guard lock(mutex_);
    return static_cast<std::uint32_t>(data_.size());
  }
  std::uint32_t BlockCount() const { return Size() / kBlockSize; }
  std::uint16_t MemSizeMbit() const {
    return static_cast<std::uint16_t>((static_cast<std::uint64_t>(Size()) * 8u) / (1024u * 1024u));
  }
  std::int32_t Result() const { std::lock_guard lock(mutex_); return result_; }
  void SetResult(std::int32_t result) { std::lock_guard lock(mutex_); result_ = result; }

  bool Directory(std::array<std::uint8_t, kBlockSize>* selected) const {
    std::lock_guard lock(mutex_);
    std::array<std::uint8_t, kBlockSize> first{}, second{};
    if (!mounted_ || !ReadBlockLocked(kDirA, &first) || !ReadBlockLocked(kDirB, &second) || !selected)
      return false;
    const auto valid = [](const auto& block) {
      const auto [sum, inverse] = Checksum(block.data(), kBlockSize - 4);
      return ReadBE16(block.data() + 0x1ffc) == sum &&
             ReadBE16(block.data() + 0x1ffe) == inverse;
    };
    const bool first_valid = valid(first), second_valid = valid(second);
    if (!first_valid && !second_valid) return false;
    if (!first_valid) { *selected = second; return true; }
    if (!second_valid) { *selected = first; return true; }
    const auto a = ReadBE16(first.data() + 0x1ffa);
    const auto b = ReadBE16(second.data() + 0x1ffa);
    *selected = Newer(b, a) ? second : first;
    return true;
  }

  bool Fat(std::array<std::uint8_t, kBlockSize>* selected) const {
    std::lock_guard lock(mutex_);
    std::array<std::uint8_t, kBlockSize> first{}, second{};
    if (!mounted_ || !ReadBlockLocked(kFatA, &first) || !ReadBlockLocked(kFatB, &second) || !selected)
      return false;
    const auto valid = [](const auto& block) {
      const auto [sum, inverse] = Checksum(block.data() + 4, kBlockSize - 4);
      return ReadBE16(block.data()) == sum && ReadBE16(block.data() + 2) == inverse;
    };
    const bool first_valid = valid(first), second_valid = valid(second);
    if (!first_valid && !second_valid) return false;
    if (!first_valid) { *selected = second; return true; }
    if (!second_valid) { *selected = first; return true; }
    const auto a = ReadBE16(first.data() + 0x04);
    const auto b = ReadBE16(second.data() + 0x04);
    *selected = Newer(b, a) ? second : first;
    return true;
  }

  bool CommitDirectory(std::array<std::uint8_t, kBlockSize> directory) {
    std::lock_guard lock(mutex_);
    if (!mounted_) return false;
    WriteBE16(directory.data() + 0x1ffa,
              static_cast<std::uint16_t>(ReadBE16(directory.data() + 0x1ffa) + 1));
    DirectoryChecksum(directory);
    std::copy(directory.begin(), directory.end(), data_.begin() + kDirA);
    std::copy(directory.begin(), directory.end(), data_.begin() + kDirB);
    dirty_ = true;
    const bool ok = FlushLocked();
    result_ = ok ? 0 : -5;
    return ok;
  }

  bool CommitFat(std::array<std::uint8_t, kBlockSize> fat) {
    std::lock_guard lock(mutex_);
    if (!mounted_) return false;
    WriteBE16(fat.data() + 0x04,
              static_cast<std::uint16_t>(ReadBE16(fat.data() + 0x04) + 1));
    FatChecksum(fat);
    std::copy(fat.begin(), fat.end(), data_.begin() + kFatA);
    std::copy(fat.begin(), fat.end(), data_.begin() + kFatB);
    dirty_ = true;
    const bool ok = FlushLocked();
    result_ = ok ? 0 : -5;
    return ok;
  }

  bool Check() const {
    std::lock_guard lock(mutex_);
    if (!mounted_ || data_.size() < kSystemBlocks * kBlockSize) return false;
    const auto* header = data_.data();
    const auto [header_sum, header_inv] = Checksum(header, 0x1fc);
    const auto expected_mbit = static_cast<std::uint16_t>(
        (static_cast<std::uint64_t>(data_.size()) * 8u) / (1024u * 1024u));
    if (ReadBE16(header + 0x1fc) != header_sum ||
        ReadBE16(header + 0x1fe) != header_inv ||
        ReadBE16(header + 0x20) != 0 ||
        ReadBE16(header + 0x22) != expected_mbit)
      return false;
    auto check_directory = [&](std::uint32_t address) {
      const auto* b = data_.data() + address;
      const auto [sum, inv] = Checksum(b, kBlockSize - 4);
      return ReadBE16(b + 0x1ffc) == sum && ReadBE16(b + 0x1ffe) == inv;
    };
    auto check_fat = [&](std::uint32_t address) {
      const auto* b = data_.data() + address;
      const auto [sum, inv] = Checksum(b + 4, kBlockSize - 4);
      return ReadBE16(b) == sum && ReadBE16(b + 2) == inv;
    };
    return (check_directory(kDirA) || check_directory(kDirB)) &&
           (check_fat(kFatA) || check_fat(kFatB));
  }
};

class Service {
  std::array<Image, 2> slots_;
public:
  Image* Slot(int channel) {
    if (channel < 0 || channel > 1) return nullptr;
    if (!slots_[channel].Mount(static_cast<unsigned>(channel))) return nullptr;
    return &slots_[channel];
  }
  Image* SlotForFormat(int channel) {
    if (channel < 0 || channel > 1) return nullptr;
    return &slots_[channel];
  }
  void Unmount(int channel) {
    if (channel >= 0 && channel < 2) slots_[channel].Unmount();
  }
};

inline Service& HostCards() {
  static Service service;
  return service;
}

}  // namespace GekkoAOT::Card
