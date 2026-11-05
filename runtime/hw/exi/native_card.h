#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <span>

namespace GekkoAOT::NativeHW::EXI {

// Persistent host file behind the small EXI command contract used by the
// guest CARD SDK. Storage, formatting and flushes are native file operations.
class HostCard final {
public:
  HostCard() = default;
  explicit HostCard(std::filesystem::path path, unsigned slot);
  ~HostCard();
  bool Ready() const { return ready_; }
  std::array<std::uint8_t, 12> FlashId() const;
  // Returns true when deselection completes a command that raises card IRQ.
  bool Select(bool selected);
  bool Immediate(std::uint8_t direction, std::uint8_t length, std::uint32_t* data);
  bool Dma(std::uint8_t direction, std::uint8_t* memory, std::uint32_t length);
  // The SDK file service and the EXI compatibility adapter share this image.
  // Only the runtime thread mutates it; a persistence worker may borrow it
  // while that slot is busy. Never maintain a second, divergent card cache.
  std::span<std::uint8_t> Image() { return bytes_; }
  std::span<const std::uint8_t> Image() const { return bytes_; }
  bool CommitImage(const std::atomic<bool>* cancel=nullptr);
  std::uint64_t Revision() const { return revision_; }
  void Changed() { ++revision_; }

private:
  static constexpr std::uint32_t kBlockSize = 0x2000u;
  static constexpr std::uint32_t kCardSize = 2u * 1024u * 1024u; // 16 Mbit / 251 usable blocks
  void Format(unsigned slot);
  bool Persist(std::uint32_t offset, std::uint32_t length);
  void MarkDirty(std::uint32_t offset, std::uint32_t length);
  bool FlushDirty();
  bool TransferByte(std::uint8_t direction, std::uint8_t* value);
  std::filesystem::path path_;
  std::vector<std::uint8_t> bytes_;
  std::array<std::uint8_t, 128> program_{};
  std::uint32_t address_ = 0;
  std::uint32_t position_ = 0;
  std::uint8_t command_ = 0;
  // Native file operations complete immediately: unlocked and ready.
  std::uint8_t status_ = 0x41u;
  bool ready_ = false;
  bool selected_ = false;
  bool dma_programmed_ = false;
  std::uint64_t revision_ = 0;
  // NativeHW 1.0 coalesces host-file persistence to the EXI command boundary.
  // CARD page programming can arrive as many tiny immediate/DMA writes; doing
  // an open+seek+flush for every fragment is unnecessary host work.
  std::uint32_t dirty_begin_ = kCardSize;
  std::uint32_t dirty_end_ = 0u;
};
} // namespace GekkoAOT::NativeHW::EXI
